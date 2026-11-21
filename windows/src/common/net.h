#pragma once
// Minimal RAII wrapper over Winsock TCP sockets (IPv4 only; everything we talk to is on 127.0.0.1).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace cabled::net {

// Initialises Winsock for the lifetime of the object. Nesting is fine.
class WinsockSession {
public:
    WinsockSession();
    ~WinsockSession();
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

class Socket {
public:
    using Handle = std::uintptr_t;  // SOCKET
    static constexpr Handle kInvalid = ~Handle(0);

    Socket() = default;
    explicit Socket(Handle h) : h_(h) {}
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    static std::optional<Socket> connectTcp(const std::string& ipv4, uint16_t port, std::string* error);
    // port 0 picks a free port; read it back with localPort().
    static std::optional<Socket> listenTcp(const std::string& ipv4, uint16_t port, std::string* error);

    std::optional<Socket> accept(std::string* error);

    bool valid() const { return h_ != kInvalid; }
    Handle handle() const { return h_; }
    uint16_t localPort() const;

    bool sendAll(const void* data, size_t size);
    bool recvExact(void* data, size_t size);

    void setRecvTimeout(uint32_t timeoutMs);  // 0 = block forever
    void setNoDelay(bool on);
    void setBufferSizes(int bytes);
    void shutdownBoth();
    void close();
    // Thread-safe: makes a recv/send blocked in another thread fail right away (closes the handle;
    // the owner's later close() is then a no-op).
    void abort();

private:
    Handle h_ = kInvalid;
    std::atomic<bool> aborted_{false};
};

// Text for the last Winsock error on this thread.
std::string lastErrorString();

}  // namespace cabled::net
