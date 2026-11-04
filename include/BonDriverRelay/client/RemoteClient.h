#pragma once
#include "BonDriverRelay/transport/Tcp.h"
#include "BonDriverRelay/core/Configuration.h"
#include <atomic>
#include <mutex>
#include <optional>

namespace bdr::client
{
    struct Reply
    {
        protocol::Bytes body;
        protocol::Bytes ts;
    };
    class RemoteClient
    {
      public:
        explicit RemoteClient(const core::ClientConfig& config);
        protocol::Catalog Open();
        Reply Call(protocol::Opcode opcode, const protocol::Bytes& body = {});
        void Shutdown() noexcept;
        bool IsAlive() const noexcept;

      private:
        protocol::Catalog OpenSingle(transport::Deadline deadline);
        Reply CallSingle(protocol::Opcode opcode, const protocol::Bytes& body, transport::Deadline deadline);
        Reply SelectAlternative(const protocol::Bytes& body, transport::Deadline deadline);
        core::ClientConfig EndpointConfig(std::size_t index) const;
        const core::ClientConfig m_config;
        std::vector<core::ClientEndpoint> m_endpoints;
        std::size_t m_selected = 0;
        std::optional<protocol::Catalog> m_catalog;
        std::shared_ptr<transport::Socket> m_socket;
        std::mutex m_mutex;
        std::uint32_t m_nextId = 1;
        std::atomic<bool> m_alive = false;
    };
} // namespace bdr::client
