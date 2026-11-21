import AVFoundation
import Foundation

/// Camera -> H.264. R3 scope: rear main camera (D-26), fixed size/fps from CONFIGURE, 420v,
/// stabilisation off, landscape buffers (sensor orientation; auto-rotation comes in R6/R7).
final class VideoPipeline: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
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

    typealias EncodedHandler = (_ avcc: [UInt8], _ keyframe: Bool, _ ptsNs: UInt64) -> Void
    /// STATUS bodies when the camera is interrupted or comes back.
    typealias StatusHandler = (_ status: [String: Any]) -> Void
    /// CONTROL_STATE bodies: {camera, zoom, lens} whenever they change.
    typealias ControlStateHandler = (_ state: [String: Any]) -> Void

    private let session = AVCaptureSession()
    private var device: AVCaptureDevice?
    private let onControlState: ControlStateHandler
    private var controlTimer: DispatchSourceTimer?
    private var lastReported: (zoom: Double, lens: String) = (0, "")
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
        if let connection = output.connection(with: .video), connection.isVideoStabilizationSupported {
            connection.preferredVideoStabilizationMode = .off  // adds latency and crops
        }
        // Starting zoom: what the PC remembered for this camera, else the main lens.
        if (try? device.lockForConfiguration()) != nil {
            let wanted = settings.zoom > 0 ? settings.zoom : CameraCatalog.defaultZoom(device)
            device.videoZoomFactor = CGFloat(clampZoom(wanted, device))
            device.unlockForConfiguration()
        }
        session.commitConfiguration()

        let deliver: VideoEncoder.Output = { [weak self] data, keyframe, ptsNs in self?.onEncoded(data, keyframe, ptsNs) }
        let encoder: VideoEncoder
        if settings.hevc, let hevcEncoder = try? VideoEncoder(hevc: true, width: width, height: height,
                                                               fps: settings.fps, bitrate: settings.bitrate,
                                                               output: deliver) {
            encoder = hevcEncoder
        } else {  // H.264 asked for, or HEVC wouldn't start
            encoder = try VideoEncoder(hevc: false, width: width, height: height, fps: settings.fps,
                                       bitrate: settings.bitrate, output: deliver)
        }
        queue.sync {
            self.encoder = encoder
            self.device = device
            self.targetBitrate = settings.bitrate
            self.currentBitrate = settings.bitrate
            self.lastReported = (0, "")
        }
        startRecoveryTimer()
        startControlTimer()
        observeInterruptions()
        session.startRunning()
        return Applied(width: width, height: height, fps: settings.fps, hevc: encoder.hevc,
                       lowLatencyRateControl: encoder.lowLatencyRateControl, cameraId: device.uniqueID)
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
        guard let encoder, let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
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
