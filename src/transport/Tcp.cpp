#include "BonDriverRelay/transport/Tcp.h"
#include "BonDriverRelay/core/Error.h"
#include <WS2tcpip.h>
#include <algorithm>
#include <array>
#include <limits>

namespace bdr::transport
{
    namespace
    {
        [[noreturn]] void Fail(const char* message)
        {
            throw core::RelayError(core::ErrorCode::LoadFailure, message,
                                   static_cast<std::uint32_t>(WSAGetLastError()));
        }
    } // namespace
    Deadline After(std::uint32_t milliseconds)
    {
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    }
    Socket::Socket()
    {
        WSADATA data{};
        const int code = WSAStartup(MAKEWORD(2, 2), &data);
        if (code != 0) {
            throw core::RelayError(core::ErrorCode::LoadFailure, "WSAStartup failed", static_cast<std::uint32_t>(code));
        }
        m_winsock = true;
    }
    Socket::~Socket()
    {
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
        }
        if (m_winsock) {
            WSACleanup();
        }
    }
    void Socket::SetNonblocking()
    {
        u_long enabled = 1;
        if (ioctlsocket(m_socket, FIONBIO, &enabled) == SOCKET_ERROR) {
            Fail("ioctlsocket failed");
        }
    }
    bool Socket::Ready(bool write, Deadline deadline) const
    {
        for (;;) {
            if (m_shutdown.load()) {
                throw core::RelayError(core::ErrorCode::LoadFailure, "Socket stopped", WSAESHUTDOWN);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto us = std::min<std::int64_t>(
                100000, std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count());
            timeval timeout{static_cast<long>(us / 1'000'000), static_cast<long>(us % 1'000'000)};
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(m_socket, &ready);
            const int result = select(0, write ? nullptr : &ready, write ? &ready : nullptr, nullptr, &timeout);
            if (result == SOCKET_ERROR) {
                Fail("select failed");
            }
            if (result > 0) {
                return true;
            }
        }
    }
    void Socket::Connect(const std::string& host, std::uint16_t port, Deadline deadline)
    {
        // Numeric IPv4 keeps DNS resolution outside timeout-sensitive DLL calls.
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            throw core::RelayError(core::ErrorCode::Configuration, "Client host must be an IPv4 literal");
        }
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET) {
            Fail("socket failed");
        }
        SetNonblocking();
        if (connect(m_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                Fail("connect failed");
            }
            if (!Ready(true, deadline)) {
                throw core::RelayError(core::ErrorCode::ReceiveTimeout, "Connect deadline exceeded");
            }
            int error = 0;
            int length = sizeof(error);
            if (getsockopt(m_socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == SOCKET_ERROR ||
                error != 0) {
                if (error != 0) {
                    WSASetLastError(error);
                }
                Fail("connect completion failed");
            }
        }
        const BOOL enabled = TRUE;
        (void)setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    }
    void Socket::Listen(const std::string& host, std::uint16_t port)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            throw core::RelayError(core::ErrorCode::Configuration, "Invalid bind address");
        }
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_socket == INVALID_SOCKET) {
            Fail("socket failed");
        }
        const BOOL exclusive = TRUE;
        if (setsockopt(m_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                       sizeof(exclusive)) == SOCKET_ERROR) {
            Fail("Exclusive bind option failed");
        }
        if (bind(m_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(m_socket, SOMAXCONN) == SOCKET_ERROR) {
            Fail("bind/listen failed");
        }
        SetNonblocking();
    }
    std::shared_ptr<Socket> Socket::Accept(Deadline deadline)
    {
        if (!Ready(false, deadline)) {
            return {};
        }
        const auto accepted = accept(m_socket, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                return {};
            }
            Fail("accept failed");
        }
        std::shared_ptr<Socket> result;
        try {
            result = std::make_shared<Socket>();
        }
        catch (...) {
            closesocket(accepted);
            throw;
        }
        result->m_socket = accepted;
        result->SetNonblocking();
        const BOOL enabled = TRUE;
        (void)setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        return result;
    }
    std::uint16_t Socket::GetPort() const
    {
        sockaddr_in address{};
        int length = sizeof(address);
        if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
            Fail("getsockname failed");
        }
        return ntohs(address.sin_port);
    }
    void Socket::Shutdown() noexcept
    {
        m_shutdown.store(true);
        if (m_socket != INVALID_SOCKET) {
            (void)shutdown(m_socket, SD_BOTH);
        }
    }
    void Socket::Send(std::span<const std::uint8_t> data, Deadline deadline) const
    {
        while (!data.empty()) {
            if (!Ready(true, deadline)) {
                throw core::RelayError(core::ErrorCode::ReceiveTimeout, "Send deadline exceeded");
            }
            const int count = send(m_socket, reinterpret_cast<const char*>(data.data()),
                                   static_cast<int>(std::min<std::size_t>(data.size(), 65536)), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            if (count <= 0) {
                Fail("send disconnected or failed");
            }
            data = data.subspan(static_cast<std::size_t>(count));
        }
    }
    void Socket::Receive(std::span<std::uint8_t> data, Deadline deadline) const
    {
        while (!data.empty()) {
            if (!Ready(false, deadline)) {
                throw core::RelayError(core::ErrorCode::ReceiveTimeout, "Receive deadline exceeded");
            }
            const int count = recv(m_socket, reinterpret_cast<char*>(data.data()),
                                   static_cast<int>(std::min<std::size_t>(data.size(), 65536)), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            if (count <= 0) {
                Fail("recv disconnected or incomplete frame");
            }
            data = data.subspan(static_cast<std::size_t>(count));
        }
    }
    std::size_t Socket::ReceiveSome(std::span<std::uint8_t> data, Deadline deadline) const
    {
        if (data.empty()) {
            return 0;
        }
        for (;;) {
            if (!Ready(false, deadline)) {
                throw core::RelayError(core::ErrorCode::ReceiveTimeout, "HTTP receive deadline exceeded");
            }
            const int count = recv(m_socket, reinterpret_cast<char*>(data.data()),
                                   static_cast<int>(std::min<std::size_t>(data.size(), 65536)), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            if (count < 0) {
                Fail("HTTP recv failed");
            }
            return static_cast<std::size_t>(count);
        }
    }
    void Socket::SendFrame(protocol::MessageType type, std::uint32_t id, std::span<const std::uint8_t> data,
                           Deadline deadline, protocol::Status status) const
    {
        if (data.size() > protocol::TS_PAYLOAD_LIMIT) {
            throw core::RelayError(core::ErrorCode::InvalidStream, "Frame is too large");
        }
        protocol::Header header{1, 0, type, 0, id, static_cast<std::uint32_t>(data.size()), status};
        if (protocol::ValidateHeader(header) != protocol::HeaderError::None) {
            throw core::RelayError(core::ErrorCode::InvalidStream, "Invalid outgoing frame");
        }
        Send(protocol::EncodeHeader(header), deadline);
        Send(data, deadline);
    }
    Frame Socket::ReceiveFrame(Deadline deadline) const
    {
        std::array<std::uint8_t, protocol::HEADER_SIZE> bytes{};
        Receive(bytes, deadline);
        const auto parsed = protocol::DecodeHeader(bytes);
        if (parsed.error != protocol::HeaderError::None) {
            throw core::RelayError(core::ErrorCode::InvalidStream, "Invalid incoming frame header");
        }
        Frame result{parsed.value, protocol::Bytes(parsed.value.payloadLength)};
        Receive(result.payload, deadline);
        return result;
    }
} // namespace bdr::transport
