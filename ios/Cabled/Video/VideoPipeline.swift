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
    private var lastStateJSON = ""

    // Rotation (R7): iOS tells us the angle that keeps the picture level; the capture hardware
    // rotates. When the frame flips between landscape and portrait the encoder is recreated.
    private var videoConnection: AVCaptureConnection?
    private var rotationCoordinator: AVCaptureDevice.RotationCoordinator?
    private var rotationObservation: NSKeyValueObservation?
    private var rotationAngle: CGFloat = 0  // camera queue
    private var autoRotationAngle: CGFloat = 0  // what iOS says keeps the picture level (camera queue)
    private var rotationOverride: CGFloat?      // R9: fixed by the user (camera queue)
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

        // SDR BT.709 end to end (Q11, video-pipeline.md "Colour"): iOS would otherwise switch the
        // camera to Display P3 wherever the format allows, and the encoder only *labels* frames
        // BT.709, it doesn't convert, so P3 colours would reach the PC mislabelled (dull reds/skin).
        session.automaticallyConfiguresCaptureDeviceForWideColor = false
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
            Self.useRec709(device)
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

    /// Fast path (R8): same camera, new size / frame rate / codec. Changing the format of the running
    /// session takes a few frames; tearing it down and building a new one takes a second or more.
    /// Nil when it can't be done this way (the caller then restarts the whole pipeline).
    func reconfigure(_ settings: Settings) -> Applied? {
        guard session.isRunning, let device = queue.sync(execute: { self.device }),
              settings.cameraId == nil || settings.cameraId == device.uniqueID,
              let format = Self.pickFormat(device, width: settings.width, height: settings.height, fps: settings.fps)
        else { return nil }
        session.beginConfiguration()
        do {
            try device.lockForConfiguration()
            if device.activeFormat != format { device.activeFormat = format }
            Self.useRec709(device)
            let frameDuration = CMTime(value: 1, timescale: CMTimeScale(settings.fps.rounded()))
            device.activeVideoMinFrameDuration = frameDuration
            device.activeVideoMaxFrameDuration = frameDuration
            // A new format resets zoom; put back what the PC remembered for this camera.
            let wanted = settings.zoom > 0 ? settings.zoom : CameraCatalog.defaultZoom(device)
            device.videoZoomFactor = CGFloat(clampZoom(wanted, device))
            device.unlockForConfiguration()
        } catch {
            session.commitConfiguration()
            return nil
        }
        session.commitConfiguration()
        guard let encoder = try? makeEncoder(hevc: settings.hevc, width: Int(settings.width),
                                             height: Int(settings.height), fps: settings.fps,
                                             bitrate: settings.bitrate) else { return nil }
        queue.sync {
            self.encoder?.invalidate()
            self.encoder = encoder  // captureOutput swaps it again if the frames are rotated
            self.encoderSize = (Int(settings.width), Int(settings.height))
            self.encoderHevc = encoder.hevc
            self.encoderFps = settings.fps
            self.targetBitrate = settings.bitrate
            self.currentBitrate = settings.bitrate
        }
        return Applied(width: settings.width, height: settings.height, fps: settings.fps, hevc: encoder.hevc,
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
            self?.queue.async {
                guard let self else { return }
                self.autoRotationAngle = angle
                if self.rotationOverride == nil { self.applyRotation(angle) }
            }
        }
    }

    /// R9: CONTROL_SET "rotation": 0 / 90 / 180 / 270 fixes the picture's rotation (a phone lying
    /// flat or on an odd mount confuses auto-rotation); "auto" hands it back. Camera queue.
    private func setRotationOverride(_ value: Any) {
        if let degrees = (value as? NSNumber)?.intValue, [0, 90, 180, 270].contains(degrees) {
            rotationOverride = CGFloat(degrees)
            applyRotation(CGFloat(degrees))
        } else {
            rotationOverride = nil
            applyRotation(autoRotationAngle)
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

    /// Reports the camera's state to the PC whenever it changes (zoom ramps, auto exposure...).
    private func startControlTimer() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 0.1, repeating: 0.2)
        timer.setEventHandler { [weak self] in
            guard let self, let device = self.device else { return }
            let state = Self.controlState(device)
            guard JSONSerialization.isValidJSONObject(state),
                  let data = try? JSONSerialization.data(withJSONObject: state, options: [.sortedKeys]),
                  let json = String(data: data, encoding: .utf8), json != self.lastStateJSON else { return }
            self.lastStateJSON = json
            self.onControlState(state)
        }
        timer.resume()
        controlTimer = timer
    }

    /// Rounded, and never NaN/infinity: JSONSerialization raises an uncatchable exception on those.
    private static func r(_ v: Double, _ places: Double = 100) -> Double {
        guard v.isFinite else { return 0 }
        let x = (v * places).rounded() / places
        return x.isFinite ? x : 0
    }

    private static func seconds(_ t: CMTime) -> Double {
        let s = CMTimeGetSeconds(t)
        return t.isValid && s.isFinite ? s : 0
    }

    static func controlState(_ d: AVCaptureDevice) -> [String: Any] {
        let format = d.activeFormat
        let shutter = seconds(d.exposureDuration)
        let frame = seconds(d.activeVideoMaxFrameDuration)
        let maxShutter = frame > 0 ? min(seconds(format.maxExposureDuration), frame) : seconds(format.maxExposureDuration)
        // temperatureAndTintValues(for:) raises an Objective-C exception (crash) if any gain is
        // outside 1...maxWhiteBalanceGain, and the live gains can be, e.g. right after a switch.
        let wb = d.temperatureAndTintValues(for: Self.clampedGains(d.deviceWhiteBalanceGains, max: d.maxWhiteBalanceGain))
        return [
            "camera": d.uniqueID,
            "zoom": r(Double(d.videoZoomFactor)),
            "lens": CameraCatalog.lensName(d),
            "thermal": DeviceInfo.thermalStateName(ProcessInfo.processInfo.thermalState),
            "colorSpace": colorSpaceName(d.activeColorSpace),  // R12: should always read rec709
            "focus": [
                "mode": d.focusMode == .locked ? "manual" : "auto",
                "position": r(Double(d.lensPosition)),
                "manualSupported": d.isLockingFocusWithCustomLensPositionSupported,
            ],
            "exposure": [
                "mode": d.exposureMode == .custom ? "manual" : "auto",
                "iso": r(Double(d.iso), 1),
                "shutter": r(shutter, 100_000),
                "bias": r(Double(d.exposureTargetBias)),
                "isoRange": [r(Double(format.minISO), 1), r(Double(format.maxISO), 1)],
                "shutterRange": [r(seconds(format.minExposureDuration), 1_000_000), r(maxShutter, 1_000_000)],
                "biasRange": [r(Double(d.minExposureTargetBias)), r(Double(d.maxExposureTargetBias))],
            ],
            "whiteBalance": [
                "mode": d.whiteBalanceMode == .continuousAutoWhiteBalance ? "auto" : "manual",
                "temperature": r(Double(wb.temperature), 0.01),
                "tint": r(Double(wb.tint), 1),
            ],
        ]
    }

    // MARK: Focus / exposure / white balance (R8)

    /// CONTROL_SET body: {focus?, exposure?, whiteBalance?}. Points are in the sent frame (0..1).
    func applyControls(_ c: [String: Any]) {
        queue.async {
            if let rotation = c["rotation"] { self.setRotationOverride(rotation) }
            guard let device = self.device, (try? device.lockForConfiguration()) != nil else { return }
            defer { device.unlockForConfiguration() }
            let angle = self.rotationAngle

            // A clicked point only sticks if face-driven focus/exposure is off: with a face in view
            // iOS otherwise keeps steering both to the face and ignores the point (build 9 test).
            // Choosing "auto" again hands control back to iOS (faces included, centre of frame).
            if let focus = c["focus"] as? [String: Any] {
                if let p = focus["point"] as? [Double], p.count == 2, device.isFocusPointOfInterestSupported {
                    device.automaticallyAdjustsFaceDrivenAutoFocusEnabled = false
                    device.isFaceDrivenAutoFocusEnabled = false
                    device.focusPointOfInterest = Self.unrotate(CGPoint(x: p[0], y: p[1]), by: angle)
                    // Setting the mode is what starts focusing on the new point.
                    if device.isFocusModeSupported(.autoFocus) { device.focusMode = .autoFocus }
                    if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
                }
                switch focus["mode"] as? String {
                case "manual":
                    if device.isLockingFocusWithCustomLensPositionSupported, let pos = focus["position"] as? Double {
                        device.setFocusModeLocked(lensPosition: Float(min(max(pos, 0), 1)), completionHandler: nil)
                    }
                case "auto":
                    device.automaticallyAdjustsFaceDrivenAutoFocusEnabled = true
                    if device.isFocusPointOfInterestSupported { device.focusPointOfInterest = CGPoint(x: 0.5, y: 0.5) }
                    if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
                default: break
                }
            }

            if let exposure = c["exposure"] as? [String: Any] {
                if let p = exposure["point"] as? [Double], p.count == 2, device.isExposurePointOfInterestSupported {
                    device.automaticallyAdjustsFaceDrivenAutoExposureEnabled = false
                    device.isFaceDrivenAutoExposureEnabled = false
                    device.exposurePointOfInterest = Self.unrotate(CGPoint(x: p[0], y: p[1]), by: angle)
                    if device.isExposureModeSupported(.continuousAutoExposure) { device.exposureMode = .continuousAutoExposure }
                }
                switch exposure["mode"] as? String {
                case "auto":
                    if exposure["point"] == nil && exposure["bias"] == nil {  // a brightness tweak keeps the point
                        device.automaticallyAdjustsFaceDrivenAutoExposureEnabled = true
                        if device.isExposurePointOfInterestSupported {
                            device.exposurePointOfInterest = CGPoint(x: 0.5, y: 0.5)
                        }
                    }
                    if device.isExposureModeSupported(.continuousAutoExposure) { device.exposureMode = .continuousAutoExposure }
                    if let bias = exposure["bias"] as? Double {
                        let b = min(max(Float(bias), device.minExposureTargetBias), device.maxExposureTargetBias)
                        device.setExposureTargetBias(b, completionHandler: nil)
                    }
                case "manual":
                    guard device.isExposureModeSupported(.custom) else { break }
                    let format = device.activeFormat
                    let iso = min(max(Float(exposure["iso"] as? Double ?? Double(device.iso)), format.minISO), format.maxISO)
                    let longest = min(CMTimeGetSeconds(format.maxExposureDuration),
                                      CMTimeGetSeconds(device.activeVideoMaxFrameDuration))  // ≤ one frame
                    let seconds = min(max(exposure["shutter"] as? Double ?? CMTimeGetSeconds(device.exposureDuration),
                                          CMTimeGetSeconds(format.minExposureDuration)), longest)
                    device.setExposureModeCustom(duration: CMTime(seconds: seconds, preferredTimescale: 1_000_000_000),
                                                 iso: iso, completionHandler: nil)
                default: break
                }
            }

            if let wb = c["whiteBalance"] as? [String: Any] {
                switch wb["mode"] as? String {
                case "auto":
                    if device.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
                        device.whiteBalanceMode = .continuousAutoWhiteBalance
                    }
                case "locked":
                    if device.isWhiteBalanceModeSupported(.locked) { device.whiteBalanceMode = .locked }
                case "manual":
                    guard device.isWhiteBalanceModeSupported(.locked) else { break }
                    let values = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(
                        temperature: Float(wb["temperature"] as? Double ?? 5000), tint: Float(wb["tint"] as? Double ?? 0))
                    let gains = Self.clampedGains(device.deviceWhiteBalanceGains(for: values),
                                                  max: device.maxWhiteBalanceGain)
                    device.setWhiteBalanceModeLocked(with: gains, completionHandler: nil)
                default: break
                }
            }
        }
    }

    /// White balance gains AVFoundation accepts: each in 1...maxGain, never NaN.
    static func clampedGains(_ g: AVCaptureDevice.WhiteBalanceGains, max maxGain: Float) -> AVCaptureDevice.WhiteBalanceGains {
        func c(_ v: Float) -> Float { v.isFinite ? Swift.min(Swift.max(v, 1), maxGain) : 1 }
        return AVCaptureDevice.WhiteBalanceGains(redGain: c(g.redGain), greenGain: c(g.greenGain), blueGain: c(g.blueGain))
    }

    /// Inverse of rotate(): a point in the sent frame -> sensor (point-of-interest) coordinates.
    static func unrotate(_ p: CGPoint, by angle: CGFloat) -> CGPoint {
        switch Int(angle.rounded()) % 360 {
        case 90: return CGPoint(x: p.y, y: 1 - p.x)
        case 180: return CGPoint(x: 1 - p.x, y: 1 - p.y)
        case 270: return CGPoint(x: 1 - p.y, y: p.x)
        default: return p
        }
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

    /// Device locked by the caller. sRGB is AVFoundation's name for the BT.709-primaries video space.
    static func useRec709(_ device: AVCaptureDevice) {
        if device.activeFormat.supportedColorSpaces.contains(.sRGB), device.activeColorSpace != .sRGB {
            device.activeColorSpace = .sRGB
        }
    }

    static func colorSpaceName(_ space: AVCaptureColorSpace) -> String {
        switch space {
        case .sRGB: return "rec709"
        case .P3_D65: return "p3"
        case .HLG_BT2020: return "hlg"
        case .appleLog: return "appleLog"
        default: return "other"
        }
    }

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
