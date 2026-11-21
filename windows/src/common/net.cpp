#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <system_error>
#include <utility>

namespace cabled::net {
namespace {

SOCKET raw(Socket::Handle h) { return static_cast<SOCKET>(h); }

bool makeAddress(const std::string& ipv4, uint16_t port, sockaddr_in& out, std::string* error) {
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4.c_str(), &out.sin_addr) != 1) {
        if (error) *error = "invalid IPv4 address: " + ipv4;
        return false;
    }
    return true;
}

}  // namespace

WinsockSession::WinsockSession() {
    WSADATA data{};
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

WinsockSession::~WinsockSession() {
    if (ok_) WSACleanup();
}

std::string lastErrorString() {
    const int code = WSAGetLastError();
    return "WSA error " + std::to_string(code) + ": " + std::system_category().message(code);
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : h_(std::exchange(other.h_, kInvalid)), aborted_(other.aborted_.exchange(false)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        h_ = std::exchange(other.h_, kInvalid);
        aborted_ = other.aborted_.exchange(false);
    }
    return *this;
}

void Socket::abort() {
    if (h_ != kInvalid && !aborted_.exchange(true)) closesocket(raw(h_));
}

std::optional<Socket> Socket::connectTcp(const std::string& ipv4, uint16_t port, std::string* error) {
    sockaddr_in addr{};
    if (!makeAddress(ipv4, port, addr, error)) return std::nullopt;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (error) *error = "socket: " + lastErrorString();
        return std::nullopt;
    }
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        if (error) *error = "connect " + ipv4 + ":" + std::to_string(port) + ": " + lastErrorString();
        closesocket(s);
        return std::nullopt;
    }
    return Socket(static_cast<Handle>(s));
}

std::optional<Socket> Socket::listenTcp(const std::string& ipv4, uint16_t port, std::string* error) {
    sockaddr_in addr{};
    if (!makeAddress(ipv4, port, addr, error)) return std::nullopt;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (error) *error = "socket: " + lastErrorString();
        return std::nullopt;
    }
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(s, SOMAXCONN) == SOCKET_ERROR) {
        if (error) *error = "listen " + ipv4 + ":" + std::to_string(port) + ": " + lastErrorString();
        closesocket(s);
        return std::nullopt;
    }
    return Socket(static_cast<Handle>(s));
}

std::optional<Socket> Socket::accept(std::string* error) {
    SOCKET c = ::accept(raw(h_), nullptr, nullptr);
    if (c == INVALID_SOCKET) {
        if (error) *error = "accept: " + lastErrorString();
        return std::nullopt;
    }
    return Socket(static_cast<Handle>(c));
}

uint16_t Socket::localPort() const {
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getsockname(raw(h_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

bool Socket::sendAll(const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const int chunk = static_cast<int>(size > (1u << 30) ? (1u << 30) : size);
        const int sent = ::send(raw(h_), p, chunk, 0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool Socket::recvExact(void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        const int chunk = static_cast<int>(size > (1u << 30) ? (1u << 30) : size);
        const int got = ::recv(raw(h_), p, chunk, 0);
        if (got == SOCKET_ERROR || got == 0) return false;
        p += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

void Socket::setRecvTimeout(uint32_t timeoutMs) {
    const DWORD ms = timeoutMs;
    setsockopt(raw(h_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

void Socket::setNoDelay(bool on) {
    const BOOL value = on ? TRUE : FALSE;
    setsockopt(raw(h_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), sizeof(value));
}

void Socket::setBufferSizes(int bytes) {
    setsockopt(raw(h_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    setsockopt(raw(h_), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

void Socket::shutdownBoth() {
    if (valid()) ::shutdown(raw(h_), SD_BOTH);
}

void Socket::close() {
    if (valid()) {
        if (!aborted_.exchange(false)) closesocket(raw(h_));
        h_ = kInvalid;
    }
}

}  // namespace cabled::net
