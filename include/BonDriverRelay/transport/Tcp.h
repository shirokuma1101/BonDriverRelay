#pragma once
#include <WinSock2.h>
#include "BonDriverRelay/protocol/Control.h"
#include <chrono>
#include <memory>
#include <string>
#include <atomic>

namespace bdr::transport
{
    using Deadline = std::chrono::steady_clock::time_point;
    struct Frame
    {
        protocol::Header header;
        protocol::Bytes payload;
    };
    class Socket
    {
      public:
        Socket();
        ~Socket();
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;
        void Connect(const std::string& host, std::uint16_t port, Deadline deadline);
        void Listen(const std::string& address, std::uint16_t port);
        std::shared_ptr<Socket> Accept(Deadline deadline);
        std::uint16_t GetPort() const;
        void Shutdown() noexcept;
        void Send(std::span<const std::uint8_t> data, Deadline deadline) const;
        void Receive(std::span<std::uint8_t> data, Deadline deadline) const;
        std::size_t ReceiveSome(std::span<std::uint8_t> data, Deadline deadline) const;
        void SendFrame(protocol::MessageType type, std::uint32_t id, std::span<const std::uint8_t> data,
                       Deadline deadline, protocol::Status status = protocol::Status::Ok) const;
        Frame ReceiveFrame(Deadline deadline) const;

      private:
        bool Ready(bool write, Deadline deadline) const;
        void SetNonblocking();
        SOCKET m_socket = INVALID_SOCKET;
        bool m_winsock = false;
        std::atomic<bool> m_shutdown = false;
    };
    Deadline After(std::uint32_t milliseconds);
} // namespace bdr::transport
