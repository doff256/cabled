import Darwin
import Foundation

struct SocketError: Error, CustomStringConvertible {
    let call: String
    let code: Int32
    var description: String { "\(call) failed: \(String(cString: strerror(code))) (\(code))" }
}

/// What the UI hears from the transport. Always delivered on a background thread.
enum CompanionEvent {
    case listening(address: String, port: UInt16)
    case listenerStopped
    case connected
    case disconnected(reason: String)
    case peerHello([String: Any])
    case pingCount(Int)
    case probeDown(mbps: Double)
    case probeUp(mbps: Double)
    case streaming(String?)  // description while video flows, nil when it stops
    case log(String)
}

/// Everything a connection needs from the app, gathered on the main thread before it starts.
struct ConnectionContext {
    var hello: [String: Any]
    var status: () -> [String: Any]
    var emit: (CompanionEvent) -> Void
}

/// Listens for the PC. usbmux delivers connections from the PC to the phone's loopback interface,
/// so binding to 127.0.0.1 should be enough and keeps us off Wi-Fi entirely (R-60). The R1 spike
/// also offers "all interfaces" so we can check that assumption on a real device.
final class SocketServer {
    enum BindMode: String, CaseIterable, Identifiable {
        case loopback
        case any
        var id: String { rawValue }
        var address: String { self == .loopback ? "127.0.0.1" : "0.0.0.0" }
    }

    private let port: UInt16
    private let bindMode: BindMode
    private let makeContext: () -> ConnectionContext
    private let emit: (CompanionEvent) -> Void

    private let lock = NSLock()
    private var listenFD: Int32 = -1
    private var stopping = false
    private var current: Connection?

    init(port: UInt16, bindMode: BindMode,
         makeContext: @escaping () -> ConnectionContext,
         emit: @escaping (CompanionEvent) -> Void) {
        self.port = port
        self.bindMode = bindMode
        self.makeContext = makeContext
        self.emit = emit
    }

    func start() throws {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw SocketError(call: "socket", code: errno) }

        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = bindMode == .loopback ? inet_addr("127.0.0.1") : in_addr_t(0)

        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else {
            let code = errno
            close(fd)
            throw SocketError(call: "bind \(bindMode.address):\(port)", code: code)
        }
        guard listen(fd, 1) == 0 else {
            let code = errno
            close(fd)
            throw SocketError(call: "listen", code: code)
        }

        lock.lock()
        listenFD = fd
        stopping = false
        lock.unlock()

        let thread = Thread { [weak self] in self?.acceptLoop(fd: fd) }
        thread.name = "cabled.accept"
        thread.qualityOfService = .userInitiated
        thread.start()
        emit(.listening(address: bindMode.address, port: port))
    }

    func stop() {
        lock.lock()
        stopping = true
        let connection = current
        current = nil
        lock.unlock()
        connection?.close()
        // The accept loop polls the stop flag and closes the listening socket itself.
    }

    private func acceptLoop(fd: Int32) {
        defer {
            close(fd)
            lock.lock()
            if listenFD == fd { listenFD = -1 }
            lock.unlock()
            emit(.listenerStopped)
        }
        var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        while true {
            lock.lock()
            let shouldStop = stopping
            lock.unlock()
            if shouldStop { return }

            pfd.revents = 0
            let ready = poll(&pfd, 1, 250)
            if ready < 0 {
                if errno == EINTR { continue }
                emit(.log("poll failed: \(String(cString: strerror(errno)))"))
                return
            }
            if ready == 0 { continue }

            let clientFD = accept(fd, nil, nil)
            if clientFD < 0 {
                if errno == EINTR || errno == EAGAIN || errno == ECONNABORTED { continue }
                emit(.log("accept failed: \(String(cString: strerror(errno)))"))
                return
            }

            // A new PC connection replaces the old one, so a crashed PC app never leaves us stuck.
            let connection = Connection(fd: clientFD, context: makeContext())
            lock.lock()
            let previous = current
            current = connection
            lock.unlock()
            previous?.close()
            connection.start()
        }
    }
}
