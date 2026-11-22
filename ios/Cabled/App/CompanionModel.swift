import AVFoundation
import Combine
import Foundation
import UIKit

/// Main-thread state for the status screen. Owns the socket server.
final class CompanionModel: ObservableObject {
    @Published private(set) var isListening = false
    @Published private(set) var listenError: String?
    @Published private(set) var isConnected = false
    @Published private(set) var pcDescription: String?
    @Published private(set) var stream: StreamInfo?
    /// AVCaptureSession interruption reason ("camera-in-use", "system-pressure", "call", ...), nil while running.
    @Published private(set) var interruption: String?
    @Published private(set) var cameraAccess = AVCaptureDevice.authorizationStatus(for: .video)
    @Published private(set) var thermal = ProcessInfo.processInfo.thermalState
    @Published private(set) var lastDownMbps: Double?
    @Published private(set) var lastUpMbps: Double?
    @Published private(set) var pingCount = 0
    @Published private(set) var logLines: [String] = []

    /// usbmux delivers the PC's connection to the phone's loopback interface (verified in R1).
    let bindMode: SocketServer.BindMode = .loopback
    let signingExpiry = SigningInfo.expirationDate()
    let port = WireProtocol.defaultPort
    private var server: SocketServer?
    private var thermalObserver: NSObjectProtocol?

    init() {
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.thermal = ProcessInfo.processInfo.thermalState
        }
    }

    func start() {
        refreshCameraAccess()
        guard server == nil else { return }
        if cameraAccess == .notDetermined {
            AVCaptureDevice.requestAccess(for: .video) { [weak self] _ in  // the PC can't start video without it
                DispatchQueue.main.async { self?.refreshCameraAccess() }
            }
        }
        UIDevice.current.isBatteryMonitoringEnabled = true
        UIApplication.shared.isIdleTimerDisabled = true  // auto-lock would kill the session

        let mode = bindMode
        let hello = helloInfo(bindMode: mode)
        let emit: (CompanionEvent) -> Void = { [weak self] event in
            DispatchQueue.main.async { self?.handle(event) }
        }
        let server = SocketServer(
            port: port,
            bindMode: mode,
            makeContext: { [weak self] in
                ConnectionContext(hello: hello, status: { self?.statusSnapshot() ?? [:] }, emit: emit)
            },
            emit: emit)
        do {
            try server.start()
            self.server = server
            listenError = nil
        } catch {
            listenError = "\(error)"
            append("Listen failed: \(error)")
        }
    }

    func stop() {
        server?.stop()
        server = nil
        isListening = false
        isConnected = false
        stream = nil
        interruption = nil
        UIApplication.shared.isIdleTimerDisabled = false
    }

    func refreshCameraAccess() {
        cameraAccess = AVCaptureDevice.authorizationStatus(for: .video)
        thermal = ProcessInfo.processInfo.thermalState
    }

    // MARK: Events

    private func handle(_ event: CompanionEvent) {
        switch event {
        case .listening(let address, let port):
            isListening = true
            append("Listening on \(address):\(port)")
        case .listenerStopped:
            isListening = false
            append("Listener stopped")
        case .connected:
            isConnected = true
            pingCount = 0
            append("PC connected")
        case .disconnected(let reason):
            isConnected = false
            pcDescription = nil
            stream = nil
            interruption = nil
            append(reason)
        case .peerHello(let hello):
            let app = hello["app"] as? String ?? "?"
            let version = hello["appVersion"] as? String ?? "?"
            pcDescription = "\(app) \(version), protocol \(hello["protocol"] as? String ?? "?")"
        case .pingCount(let count):
            pingCount = count
        case .probeDown(let mbps):
            lastDownMbps = mbps
        case .probeUp(let mbps):
            lastUpMbps = mbps
        case .streaming(let info):
            stream = info
            if info != nil { interruption = nil }
        case .camera(let reason):
            interruption = reason
        case .log(let line):
            append(line)
        }
    }

    private func append(_ line: String) {
        let stamp = Self.timeFormatter.string(from: Date())
        logLines.append("\(stamp)  \(line)")
        if logLines.count > 200 { logLines.removeFirst(logLines.count - 200) }
    }

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    // MARK: Info sent to the PC

    private func helloInfo(bindMode: SocketServer.BindMode) -> [String: Any] {
        [
            "protocol": WireProtocol.versionString,
            "role": "phone",
            "app": DeviceInfo.displayName,
            "appVersion": DeviceInfo.appVersion,
            "build": DeviceInfo.buildNumber,
            "model": DeviceInfo.modelIdentifier,
            "system": DeviceInfo.systemVersion,
            "signingExpiry": SigningInfo.iso8601(signingExpiry),
            "bind": bindMode.rawValue,
        ]
    }

    /// Called from the connection thread; UIDevice must be read on the main thread.
    private func statusSnapshot() -> [String: Any] {
        var battery: Float = -1
        var charging = false
        let read = {
            battery = UIDevice.current.batteryLevel
            let state = UIDevice.current.batteryState
            charging = state == .charging || state == .full
        }
        if Thread.isMainThread { read() } else { DispatchQueue.main.sync(execute: read) }
        return [
            "thermal": DeviceInfo.thermalStateName(ProcessInfo.processInfo.thermalState),
            "battery": battery,
            "charging": charging,
        ]
    }
}
