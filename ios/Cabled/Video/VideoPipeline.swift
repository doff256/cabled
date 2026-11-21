import AVFoundation
import Foundation

/// Camera -> H.264. R3 scope: rear main camera (D-26), fixed size/fps from CONFIGURE, 420v,
/// stabilisation off, landscape buffers (sensor orientation; auto-rotation comes in R6/R7).
final class VideoPipeline: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate,
                           AVCaptureMetadataOutputObjectsDelegate {
    struct Settings {
        var width: Int32 = 1920
        var height: Int32 = 1080
        var fps: Double = 30
        var bitrate: Int = 25_000_000
        var hevc = false
        var cameraId: String?  // nil = default (rear main, D-26)
        var zoom: Double = 0    // device zoom factor; 0 = the camera's default
    }

    struct Applied {
        var width: Int32
        var height: Int32
        var fps: Double
        var hevc: Bool
        var lowLatencyRateControl: Bool
        var cameraId: String
    }

    struct PipelineError: Error, CustomStringConvertible {
        let description: String
    }

    /// meta: VIDEO metadata for this frame (faces JSON, R7).
    typealias EncodedHandler = (_ avcc: [UInt8], _ keyframe: Bool, _ ptsNs: UInt64, _ meta: [UInt8]) -> Void
    /// STATUS bodies when the camera is interrupted or comes back.
    typealias StatusHandler = (_ status: [String: Any]) -> Void
    /// CONTROL_STATE bodies: {camera, zoom, lens} whenever they change.
    typealias ControlStateHandler = (_ state: [String: Any]) -> Void

    private let session = AVCaptureSession()
    private var device: AVCaptureDevice?
    private let onControlState: ControlStateHandler
    private var controlTimer: DispatchSourceTimer?
    private var lastReported: (zoom: Double, lens: String) = (0, "")

    // Rotation (R7): iOS tells us the angle that keeps the picture level; the capture hardware
    // rotates. When the frame flips between landscape and portrait the encoder is recreated.
    private var videoConnection: AVCaptureConnection?
    private var rotationCoordinator: AVCaptureDevice.RotationCoordinator?
    private var rotationObservation: NSKeyValueObservation?
    private var rotationAngle: CGFloat = 0  // camera queue
    private var encoderSize: (width: Int, height: Int) = (0, 0)
    private var encoderHevc = false
    private var encoderFps: Double = 30

    // Faces (R7): from the camera hardware, rotated into the sent frame's coordinates.
    private let metadataOutput = AVCaptureMetadataOutput()
    private let facesLock = NSLock()
    private var facesMeta: [UInt8] = Array(#"{"faces":[]}"#.utf8)
    private var facesUpdated = Date.distantPast
    private let queue = DispatchQueue(label: "cabled.camera", qos: .userInteractive)
    private var encoder: VideoEncoder?
    private let onEncoded: EncodedHandler
    private let onStatus: StatusHandler
    private var observers: [NSObjectProtocol] = []
    // Bitrate adaptation (camera queue): cut on stalls, creep back up when the link is healthy.
    private var targetBitrate = 25_000_000
    private var currentBitrate = 25_000_000
    private var lastStall = Date.distantPast
    private var recoveryTimer: DispatchSourceTimer?

    init(onEncoded: @escaping EncodedHandler, onStatus: @escaping StatusHandler,
         onControlState: @escaping ControlStateHandler) {
        self.onEncoded = onEncoded
        self.onStatus = onStatus
        self.onControlState = onControlState
    }

    deinit { stop() }

    /// Blocking: configures and starts the camera. Call from a background thread.
    func start(_ settings: Settings) throws -> Applied {
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            throw PipelineError(description: "Camera access is off. Allow it in Settings → Cabled → Camera.")
        }
        guard let device = CameraCatalog.device(id: settings.cameraId) else {
            throw PipelineError(description: "No camera found")
        }
        let input = try AVCaptureDeviceInput(device: device)
        let output = AVCaptureVideoDataOutput()
        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: queue)

        session.beginConfiguration()
        guard session.canAddInput(input), session.canAddOutput(output) else {
            session.commitConfiguration()
            throw PipelineError(description: "Camera input/output not accepted")
        }
        session.addInput(input)
        session.addOutput(output)
        if session.canAddOutput(metadataOutput) {
            session.addOutput(metadataOutput)
            if metadataOutput.availableMetadataObjectTypes.contains(.face) {
                metadataOutput.metadataObjectTypes = [.face]
                metadataOutput.setMetadataObjectsDelegate(self, queue: queue)
            }
        }

        var width = settings.width, height = settings.height
        if let format = Self.pickFormat(device, width: settings.width, height: settings.height, fps: settings.fps) {
            try device.lockForConfiguration()
            device.activeFormat = format
            let frameDuration = CMTime(value: 1, timescale: CMTimeScale(settings.fps.rounded()))
            device.activeVideoMinFrameDuration = frameDuration
            device.activeVideoMaxFrameDuration = frameDuration
            device.unlockForConfiguration()
        } else {
            session.sessionPreset = .hd1920x1080  // closest safe fallback
            width = 1920
            height = 1080
        }
        if let connection = output.connection(with: .video) {
            if connection.isVideoStabilizationSupported {
                connection.preferredVideoStabilizationMode = .off  // adds latency and crops
            }
            videoConnection = connection
        }
        // Starting zoom: what the PC remembered for this camera, else the main lens.
        if (try? device.lockForConfiguration()) != nil {
            let wanted = settings.zoom > 0 ? settings.zoom : CameraCatalog.defaultZoom(device)
            device.videoZoomFactor = CGFloat(clampZoom(wanted, device))
            device.unlockForConfiguration()
        }
        session.commitConfiguration()

        let encoder = try makeEncoder(hevc: settings.hevc, width: Int(width), height: Int(height),
                                      fps: settings.fps, bitrate: settings.bitrate)
        queue.sync {
            self.encoder = encoder
            self.encoderSize = (Int(width), Int(height))
            self.encoderHevc = encoder.hevc
            self.encoderFps = settings.fps
            self.device = device
            self.targetBitrate = settings.bitrate
            self.currentBitrate = settings.bitrate
            self.lastReported = (0, "")
        }
        startRecoveryTimer()
        startControlTimer()
        observeInterruptions()
        startRotation(device)
        session.startRunning()
        return Applied(width: width, height: height, fps: settings.fps, hevc: encoder.hevc,
                       lowLatencyRateControl: encoder.lowLatencyRateControl, cameraId: device.uniqueID)
    }

    private func makeEncoder(hevc: Bool, width: Int, height: Int, fps: Double, bitrate: Int) throws -> VideoEncoder {
        let deliver: VideoEncoder.Output = { [weak self] data, keyframe, ptsNs in
            guard let self else { return }
            self.onEncoded(data, keyframe, ptsNs, self.currentFacesMeta())
        }
        if hevc, let e = try? VideoEncoder(hevc: true, width: Int32(width), height: Int32(height), fps: fps,
                                           bitrate: bitrate, output: deliver) {
            return e
        }
        // H.264 asked for, or HEVC wouldn't start.
        return try VideoEncoder(hevc: false, width: Int32(width), height: Int32(height), fps: fps, bitrate: bitrate,
                                output: deliver)
    }

    // MARK: Rotation (R7)

    private func startRotation(_ device: AVCaptureDevice) {
        let coordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: nil)
        rotationCoordinator = coordinator
        rotationObservation = coordinator.observe(\.videoRotationAngleForHorizonLevelCapture,
                                                  options: [.initial, .new]) { [weak self] c, _ in
            let angle = c.videoRotationAngleForHorizonLevelCapture
            self?.queue.async { self?.applyRotation(angle) }
        }
    }

    /// Camera queue. Frames after this come out rotated; captureOutput notices the new size.
    private func applyRotation(_ angle: CGFloat) {
        guard let connection = videoConnection, connection.isVideoRotationAngleSupported(angle),
              connection.videoRotationAngle != angle else { return }
        connection.videoRotationAngle = angle
        rotationAngle = angle
    }

    // MARK: Faces (R7)

    func metadataOutput(_ output: AVCaptureMetadataOutput, didOutput metadataObjects: [AVMetadataObject],
                        from connection: AVCaptureConnection) {
        var faces: [[String: Any]] = []
        for case let face as AVMetadataFaceObject in metadataObjects {
            let r = Self.rotate(face.bounds, by: rotationAngle)
            faces.append(["id": face.faceID, "x": Self.round3(r.minX), "y": Self.round3(r.minY),
                          "w": Self.round3(r.width), "h": Self.round3(r.height)])
        }
        let data = (try? JSONSerialization.data(withJSONObject: ["faces": faces])) ?? Data()
        facesLock.lock()
        facesMeta = [UInt8](data)
        facesUpdated = Date()
        facesLock.unlock()
    }

    /// Faces for the frame being sent; stale detections (> 0.5 s) count as none.
    private func currentFacesMeta() -> [UInt8] {
        facesLock.lock()
        defer { facesLock.unlock() }
        if Date().timeIntervalSince(facesUpdated) > 0.5 { return Array(#"{"faces":[]}"#.utf8) }
        return facesMeta
    }

    private static func round3(_ v: CGFloat) -> Double { (Double(v) * 1000).rounded() / 1000 }

    /// Metadata comes in the sensor's unrotated, normalized coordinates (top-left origin); the frames
    /// we send are rotated clockwise by `angle`. Map the box into the sent frame.
    static func rotate(_ r: CGRect, by angle: CGFloat) -> CGRect {
        switch Int(angle.rounded()) % 360 {
        case 90: return CGRect(x: 1 - r.maxY, y: r.minX, width: r.height, height: r.width)
        case 180: return CGRect(x: 1 - r.maxX, y: 1 - r.maxY, width: r.width, height: r.height)
        case 270: return CGRect(x: r.minY, y: 1 - r.maxX, width: r.height, height: r.width)
        default: return r
        }
    }

    // MARK: Zoom (R6)

    private func clampZoom(_ factor: Double, _ device: AVCaptureDevice) -> Double {
        min(max(factor, Double(device.minAvailableVideoZoomFactor)), CameraCatalog.maxZoom(device))
    }

    /// rampRate > 0: glide there (in powers of two per second); 0: jump.
    func setZoom(_ factor: Double, rampRate: Double) {
        queue.async {
            guard let device = self.device, (try? device.lockForConfiguration()) != nil else { return }
            let target = CGFloat(self.clampZoom(factor, device))
            if rampRate > 0 {
                device.ramp(toVideoZoomFactor: target, withRate: Float(rampRate))
            } else {
                device.cancelVideoZoomRamp()
                device.videoZoomFactor = target
            }
            device.unlockForConfiguration()
        }
    }

    /// Reports zoom and the active lens to the PC when they change (also during ramps).
    private func startControlTimer() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 0.1, repeating: 0.2)
        timer.setEventHandler { [weak self] in
            guard let self, let device = self.device else { return }
            let zoom = (Double(device.videoZoomFactor) * 100).rounded() / 100
            let lens = CameraCatalog.lensName(device)
            if zoom != self.lastReported.zoom || lens != self.lastReported.lens {
                self.lastReported = (zoom, lens)
                self.onControlState(["camera": device.uniqueID, "zoom": zoom, "lens": lens])
            }
        }
        timer.resume()
        controlTimer = timer
    }

    /// The link couldn't keep up (send queue overflowed): drop the bitrate by a quarter, down to a
    /// quarter of the target, and start over from a keyframe.
    func linkStalled() {
        queue.async {
            self.lastStall = Date()
            self.currentBitrate = max(self.targetBitrate / 4, self.currentBitrate * 3 / 4)
            self.encoder?.setBitrate(self.currentBitrate)
            self.encoder?.requestKeyframe()
        }
    }

    private func startRecoveryTimer() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 2, repeating: 2)
        timer.setEventHandler { [weak self] in
            guard let self, self.currentBitrate < self.targetBitrate,
                  Date().timeIntervalSince(self.lastStall) > 2 else { return }
            self.currentBitrate = min(self.targetBitrate, self.currentBitrate + max(self.currentBitrate / 20, 250_000))
            self.encoder?.setBitrate(self.currentBitrate)
        }
        timer.resume()
        recoveryTimer = timer
    }

    func stop() {
        rotationObservation?.invalidate()
        rotationObservation = nil
        rotationCoordinator = nil
        recoveryTimer?.cancel()
        recoveryTimer = nil
        controlTimer?.cancel()
        controlTimer = nil
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers.removeAll()
        if session.isRunning { session.stopRunning() }
        queue.sync {
            encoder?.invalidate()
            encoder = nil
        }
    }

    func requestKeyframe() {
        queue.async { self.encoder?.requestKeyframe() }
    }

    // MARK: AVCaptureVideoDataOutputSampleBufferDelegate (camera queue)

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard var encoder, let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        // Rotated between landscape and portrait: the encoder must match the new frame size.
        let w = CVPixelBufferGetWidth(pixelBuffer), h = CVPixelBufferGetHeight(pixelBuffer)
        if w != encoderSize.width || h != encoderSize.height {
            encoder.invalidate()
            guard let replacement = try? makeEncoder(hevc: encoderHevc, width: w, height: h, fps: encoderFps,
                                                     bitrate: currentBitrate) else { return }
            self.encoder = replacement
            encoderSize = (w, h)
            encoder = replacement  // starts with a keyframe carrying the new size
        }
        encoder.encode(pixelBuffer, pts: CMSampleBufferGetPresentationTimeStamp(sampleBuffer))
    }

    // MARK: Helpers

    private static func pickFormat(_ device: AVCaptureDevice, width: Int32, height: Int32,
                                   fps: Double) -> AVCaptureDevice.Format? {
        let candidates = device.formats.filter { format in
            let d = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            let subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription)
            return d.width == width && d.height == height
                && subtype == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                && format.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= fps + 0.01 && $0.maxFrameRate >= fps - 0.01 }
        }
        // Prefer formats that read the full sensor area (not binned).
        return candidates.first { !$0.isVideoBinned } ?? candidates.first
    }

    private func observeInterruptions() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: session,
                                            queue: nil) { [weak self] note in
            let raw = note.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int ?? -1
            self?.onStatus(["session": "interrupted", "reason": Self.reasonName(raw)])
        })
        observers.append(center.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: session,
                                            queue: nil) { [weak self] _ in
            self?.requestKeyframe()
            self?.onStatus(["session": "running"])
        })
        observers.append(center.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: session,
                                            queue: nil) { [weak self] note in
            let error = note.userInfo?[AVCaptureSessionErrorKey] as? NSError
            self?.onStatus(["session": "interrupted", "reason": "error",
                            "error": error?.localizedDescription ?? "unknown"])
        })
    }

    private static func reasonName(_ raw: Int) -> String {
        guard let reason = AVCaptureSession.InterruptionReason(rawValue: raw) else { return "other" }
        switch reason {
        case .videoDeviceNotAvailableInBackground: return "background"
        case .videoDeviceInUseByAnotherClient: return "camera-in-use"
        case .videoDeviceNotAvailableDueToSystemPressure: return "system-pressure"
        case .audioDeviceInUseByAnotherClient: return "call"
        case .videoDeviceNotAvailableWithMultipleForegroundApps: return "multitasking"
        @unknown default: return "other"
        }
    }
}
