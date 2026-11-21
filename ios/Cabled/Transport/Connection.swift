import Darwin
import Foundation

/// One PC session on its own thread. R1 scope: handshake, ping/pong, throughput probes, logs.
final class Connection {
    private let fd: Int32
    private let context: ConnectionContext
    private var pingCount = 0

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
    func close() {
        shutdown(fd, SHUT_RDWR)
    }

    // MARK: Session

    private func run() {
        var reason = "PC disconnected"
        defer {
            Darwin.close(fd)
            context.emit(.disconnected(reason: reason))
        }
        context.emit(.connected)

        guard send(.hello, json: context.hello) else { return }
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
        case .bye:
            return "PC ended the session"
        case .error:
            log("PC error: \(message.text)")
        case .pong, .log, .status:
            break
        }
        return nil
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
        bytes.withUnsafeBytes { raw -> Bool in
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
