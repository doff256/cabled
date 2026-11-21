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
    }

    struct Applied {
        var width: Int32
        var height: Int32
        var fps: Double
        var lowLatencyRateControl: Bool
    }

    struct PipelineError: Error, CustomStringConvertible {
        let description: String
    }

    typealias EncodedHandler = (_ avcc: [UInt8], _ keyframe: Bool, _ ptsNs: UInt64) -> Void
    /// STATUS bodies when the camera is interrupted or comes back.
    typealias StatusHandler = (_ status: [String: Any]) -> Void

    private let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "cabled.camera", qos: .userInteractive)
    private var encoder: H264Encoder?
    private let onEncoded: EncodedHandler
    private let onStatus: StatusHandler
    private var observers: [NSObjectProtocol] = []

    init(onEncoded: @escaping EncodedHandler, onStatus: @escaping StatusHandler) {
        self.onEncoded = onEncoded
        self.onStatus = onStatus
    }

    deinit { stop() }

    /// Blocking: configures and starts the camera. Call from a background thread.
    func start(_ settings: Settings) throws -> Applied {
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            throw PipelineError(description: "Camera access is off. Allow it in Settings → Cabled → Camera.")
        }
        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else {
            throw PipelineError(description: "No rear camera found")
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
        session.commitConfiguration()

        let encoder = try H264Encoder(width: width, height: height, fps: settings.fps, bitrate: settings.bitrate) {
            [weak self] avcc, keyframe, ptsNs in
            self?.onEncoded(avcc, keyframe, ptsNs)
        }
        queue.sync { self.encoder = encoder }
        observeInterruptions()
        session.startRunning()
        return Applied(width: width, height: height, fps: settings.fps,
                       lowLatencyRateControl: encoder.lowLatencyRateControl)
    }

    func stop() {
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
