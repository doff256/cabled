import Darwin
import Foundation

/// One PC session on its own thread. R1 scope: handshake, ping/pong, throughput probes, logs.
final class Connection {
    private let fd: Int32
    private let context: ConnectionContext
    private var pingCount = 0
    // Guards `fd` against reuse: once this connection has closed its socket, the number may belong
    // to the next connection, so close() must not touch it any more (R1 field bug: the next PC
    // connection was shut down the instant it arrived).
    private let fdLock = NSLock()
    private var fdClosed = false
    // Writes come from the connection thread, the video sender and camera notifications.
    private let writeLock = NSLock()
    private let videoLock = NSLock()
    private var pipeline: VideoPipeline?
    private var videoOut: VideoOutQueue?

    // State of a PC -> phone ("up") probe while it runs.
    private var upActive = false
    private var upBytes: UInt64 = 0
    private var upFirstNs: UInt64 = 0

    init(fd: Int32, context: ConnectionContext) {
        self.fd = fd
        self.context = context
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, Int32(IPPROTO_TCP), TCP_NODELAY, &yes, socklen_t(MemoryLayout<Int32>.size))
        var bufferSize: Int32 = 4 << 20
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, socklen_t(MemoryLayout<Int32>.size))
    }

    func start() {
        let thread = Thread { [self] in self.run() }
        thread.name = "cabled.connection"
        thread.qualityOfService = .userInitiated
        thread.start()
    }

    /// Wakes the blocked read; the connection thread then closes the socket itself.
    /// Does nothing if this connection already ended.
    func close() {
        fdLock.lock()
        defer { fdLock.unlock() }
        if !fdClosed { shutdown(fd, SHUT_RDWR) }
    }

    // MARK: Session

    private func run() {
        var reason = "PC disconnected"
        defer {
            stopVideo()
            fdLock.lock()
            fdClosed = true
            Darwin.close(fd)
            fdLock.unlock()
            context.emit(.disconnected(reason: reason))
        }
        context.emit(.connected)

        guard send(.hello, json: context.hello) else { return }
        _ = send(.caps, json: CameraCatalog.capabilities())
        _ = send(.status, json: context.status())
        log("Companion ready (protocol \(WireProtocol.versionString))")

        while true {
            guard let message = readMessage() else { return }
            if let end = handle(message) {
                reason = end
                return
            }
        }
    }

    /// Returns a reason string when the session should end.
    private func handle(_ message: WireMessage) -> String? {
        guard let type = message.type else {
            log("Ignoring unknown message type 0x\(String(message.rawType, radix: 16))")
            return nil
        }
        switch type {
        case .hello:
            let hello = WireCodec.parseJSON(message.payload) ?? [:]
            context.emit(.peerHello(hello))
            let version = hello["protocol"] as? String ?? "?"
            if !WireProtocol.isCompatible(version) {
                _ = send(.error, text: "Incompatible protocol: PC speaks \(version), phone speaks \(WireProtocol.versionString)")
                return "Incompatible PC version \(version)"
            }
        case .ping:
            if let pcTime = WireCodec.decodePing(message.payload) {
                _ = send(.pong, payload: WireCodec.encodePong(pcTimeNs: pcTime, phoneTimeNs: monotonicNanos()))
                pingCount += 1
                if pingCount % 10 == 0 { context.emit(.pingCount(pingCount)) }
            }
        case .probeRequest:
            handleProbeRequest(WireCodec.parseJSON(message.payload) ?? [:])
        case .probeData:
            if upActive {
                if upFirstNs == 0 { upFirstNs = monotonicNanos() }
                upBytes += UInt64(message.payload.count + WireProtocol.headerSize)
            }
        case .probeEnd:
            if upActive { finishUpProbe() }
        case .probeResult:
            let result = WireCodec.parseJSON(message.payload) ?? [:]
            if let mbps = result["mbps"] as? Double {
                context.emit(.probeDown(mbps: mbps))
                log(String(format: "PC measured %.1f Mbit/s from the phone", mbps))
            }
        case .configure:
            configure(WireCodec.parseJSON(message.payload) ?? [:])
        case .keyframeRequest:
            videoLock.lock()
            let p = pipeline
            videoLock.unlock()
            p?.requestKeyframe()
        case .bye:
            return "PC ended the session"
        case .error:
            log("PC error: \(message.text)")
        case .controlSet:
            let request = WireCodec.parseJSON(message.payload) ?? [:]
            videoLock.lock()
            let p = pipeline
            videoLock.unlock()
            if let zoom = request["zoom"] as? Double {
                p?.setZoom(zoom, rampRate: request["ramp"] as? Double ?? 0)
            }
            if request["focus"] != nil || request["exposure"] != nil || request["whiteBalance"] != nil
                || request["rotation"] != nil {
                p?.applyControls(request)
            }
        case .pong, .log, .status, .configured, .video, .caps, .controlState:
            break
        }
        return nil
    }

    // MARK: Video

    private func configure(_ request: [String: Any]) {
        if (request["enabled"] as? Bool) != false, reconfigureRunning(request) { return }
        stopVideo()
        if (request["enabled"] as? Bool) == false {
            send(.configured, json: ["ok": true, "enabled": false])
            context.emit(.streaming(nil))
            log("Video stopped by the PC")
            return
        }
        let settings = Self.settings(from: request)
        startVideo(settings)
    }

    private static func settings(from request: [String: Any]) -> VideoPipeline.Settings {
        var settings = VideoPipeline.Settings()
        if let w = request["width"] as? Int { settings.width = Int32(w) }
        if let h = request["height"] as? Int { settings.height = Int32(h) }
        if let fps = request["fps"] as? Double { settings.fps = fps }
        if let bitrate = request["bitrate"] as? Int { settings.bitrate = bitrate }
        settings.hevc = (request["codec"] as? String) == "hevc"
        settings.cameraId = request["camera"] as? String
        settings.zoom = request["zoom"] as? Double ?? 0
        return settings
    }

    /// Same camera, still streaming: change the format in place instead of restarting (R8).
    private func reconfigureRunning(_ request: [String: Any]) -> Bool {
        videoLock.lock()
        let p = pipeline, out = videoOut
        videoLock.unlock()
        guard let p, let out else { return false }
        let settings = Self.settings(from: request)
        guard let applied = p.reconfigure(settings) else { return false }
        out.setCodec(hevc: applied.hevc)
        reportStarted(applied, bitrate: settings.bitrate)
        return true
    }

    private func reportStarted(_ applied: VideoPipeline.Applied, bitrate: Int) {
        send(.configured, json: ["ok": true, "enabled": true, "codec": applied.hevc ? "hevc" : "h264",
                                 "width": Int(applied.width), "height": Int(applied.height),
                                 "fps": applied.fps, "bitrate": bitrate,
                                 "lowLatencyRateControl": applied.lowLatencyRateControl])
        let description = "Streaming \(applied.width)×\(applied.height) @ \(Int(applied.fps))"
            + (applied.hevc ? " HEVC" : "")
        context.emit(.streaming(description))
        log(description + (applied.lowLatencyRateControl ? " (low-latency encoder)" : ""))
    }

    private func startVideo(_ settings: VideoPipeline.Settings) {
        let out = VideoOutQueue(send: { [weak self] bytes in self?.sendAll(bytes) ?? false },
                                onStall: { [weak self] in
                                    guard let self else { return }
                                    self.videoLock.lock()
                                    let p = self.pipeline
                                    self.videoLock.unlock()
                                    p?.linkStalled()  // lower the bitrate and restart from a keyframe
                                })
        out.setCodec(hevc: settings.hevc)
        let pipeline = VideoPipeline(
            onEncoded: { avcc, keyframe, ptsNs, meta in
                out.push(avcc: avcc, keyframe: keyframe, ptsNs: ptsNs, meta: meta)
            },
            onStatus: { [weak self] status in self?.cameraStatusChanged(status) },
            onControlState: { [weak self] state in self?.send(.controlState, json: state) })
        do {
            videoLock.lock()
            self.pipeline = pipeline
            self.videoOut = out
            videoLock.unlock()
            out.start()
            let applied = try pipeline.start(settings)
            out.setCodec(hevc: applied.hevc)
            reportStarted(applied, bitrate: settings.bitrate)
        } catch {
            stopVideo()
            send(.configured, json: ["ok": false, "error": "\(error)"])
            log("Camera failed to start: \(error)")
        }
    }

    private func stopVideo() {
        videoLock.lock()
        let p = pipeline, out = videoOut
        pipeline = nil
        videoOut = nil
        videoLock.unlock()
        out?.stop()
        p?.stop()
    }

    private func cameraStatusChanged(_ status: [String: Any]) {
        var body = status
        body["thermal"] = DeviceInfo.thermalStateName(ProcessInfo.processInfo.thermalState)
        send(.status, json: body)
        let session = status["session"] as? String ?? "?"
        let reason = status["reason"] as? String ?? ""
        log("Camera \(session)\(reason.isEmpty ? "" : " (\(reason))")")
    }

    // MARK: Probes

    private func handleProbeRequest(_ request: [String: Any]) {
        let direction = request["direction"] as? String ?? ""
        let seconds = min(max((request["seconds"] as? Double) ?? 5, 0.1), 60)
        let chunk = min(max((request["chunk"] as? Int) ?? 65536, WireProtocol.minProbeChunk), 4 << 20)
        switch direction {
        case "down":
            log(String(format: "Sending to PC for %.1f s", seconds))
            sendDownProbe(seconds: seconds, chunk: chunk)
        case "up":
            upActive = true
            upBytes = 0
            upFirstNs = 0
            log("Receiving from PC")
        default:
            log("Unknown probe direction '\(direction)'")
        }
    }

    private func sendDownProbe(seconds: Double, chunk: Int) {
        var buffer = [UInt8](repeating: 0, count: WireProtocol.headerSize + chunk)
        WireCodec.writeHeader(into: &buffer, type: .probeData, payloadCount: chunk)
        WireCodec.fillProbePattern(&buffer, payloadOffset: WireProtocol.headerSize)

        let start = monotonicNanos()
        let end = start + UInt64(seconds * 1e9)
        var seq: UInt64 = 0
        var bytes: UInt64 = 0
        while monotonicNanos() < end {
            WireCodec.writeProbeSequence(&buffer, payloadOffset: WireProtocol.headerSize, seq: seq)
            guard sendAll(buffer) else { return }
            seq += 1
            bytes += UInt64(buffer.count)
        }
        let elapsed = Double(monotonicNanos() - start) / 1e9
        _ = send(.probeEnd, json: ["direction": "down", "bytes": bytes, "seconds": elapsed, "chunks": seq])
        log(String(format: "Sent %.1f MB in %.2f s", Double(bytes) / 1e6, elapsed))
    }

    private func finishUpProbe() {
        upActive = false
        let seconds = upFirstNs > 0 ? Double(monotonicNanos() - upFirstNs) / 1e9 : 0
        let mbps = seconds > 0 ? Double(upBytes) * 8 / seconds / 1e6 : 0
        _ = send(.probeResult, json: ["direction": "up", "bytes": upBytes, "seconds": seconds, "mbps": mbps])
        context.emit(.probeUp(mbps: mbps))
        log(String(format: "Received %.1f Mbit/s from the PC", mbps))
    }

    // MARK: I/O

    private func log(_ line: String) {
        context.emit(.log(line))
        _ = send(.log, text: line)
    }

    @discardableResult
    private func send(_ type: MessageType, json: [String: Any]) -> Bool {
        send(type, payload: WireCodec.json(json))
    }

    @discardableResult
    private func send(_ type: MessageType, text: String) -> Bool {
        send(type, payload: [UInt8](text.utf8))
    }

    @discardableResult
    private func send(_ type: MessageType, payload: [UInt8]) -> Bool {
        sendAll(WireCodec.encode(type, payload: payload))
    }

    private func sendAll(_ bytes: [UInt8]) -> Bool {
        writeLock.lock()
        defer { writeLock.unlock() }
        return bytes.withUnsafeBytes { raw -> Bool in
            guard var pointer = raw.baseAddress else { return true }
            var remaining = raw.count
            while remaining > 0 {
                let sent = Darwin.send(fd, pointer, remaining, 0)
                if sent < 0 {
                    if errno == EINTR { continue }
                    return false
                }
                if sent == 0 { return false }
                pointer += sent
                remaining -= sent
            }
            return true
        }
    }

    private func recvExact(_ count: Int) -> [UInt8]? {
        var buffer = [UInt8](repeating: 0, count: count)
        let ok = buffer.withUnsafeMutableBytes { raw -> Bool in
            guard var pointer = raw.baseAddress else { return true }
            var remaining = count
            while remaining > 0 {
                let got = recv(fd, pointer, remaining, 0)
                if got < 0 {
                    if errno == EINTR { continue }
                    return false
                }
                if got == 0 { return false }
                pointer += got
                remaining -= got
            }
            return true
        }
        return ok ? buffer : nil
    }

    private func readMessage() -> WireMessage? {
        guard let lengthBytes = recvExact(4) else { return nil }
        let length = WireCodec.decodeLength(lengthBytes)
        guard length >= 4, length <= WireProtocol.maxBodyLength else {
            context.emit(.log("Bad message length \(length); dropping the connection"))
            return nil
        }
        guard let body = recvExact(Int(length)) else { return nil }
        return WireCodec.decodeBody(body)
    }
}
