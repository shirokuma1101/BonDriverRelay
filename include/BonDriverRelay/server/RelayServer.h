#pragma once
#include "BonDriverRelay/transport/Tcp.h"
#include "BonDriverRelay/core/TunerManager.h"
#include <atomic>

namespace bdr::server
{
    class RelayServer
    {
      public:
        explicit RelayServer(const core::ServerConfig& config, core::Logger* logger = nullptr);
        ~RelayServer();
        RelayServer(const RelayServer&) = delete;
        RelayServer& operator=(const RelayServer&) = delete;
        void Start();
        void Stop();
        std::uint16_t GetPort() const;
        core::ServerSnapshot GetSnapshot() const;
        void LogDiagnostics() const;

      private:
        struct Connection
        {
            std::shared_ptr<transport::Socket> socket;
            std::atomic<bool> done = false;
            std::jthread worker;
        };
        void Accept(std::stop_token stop);
        void Serve(Connection& connection, std::uint64_t id);
        const core::ServerConfig m_config;
        core::Logger* m_logger;
        std::atomic<std::shared_ptr<core::TunerManager>> m_manager;
        mutable std::mutex m_snapshotMutex;
        core::ManagerSnapshot m_lastSnapshot;
        std::atomic<std::uint64_t> m_activeConnections = 0, m_acceptedConnections = 0, m_rejectedConnections = 0;
        std::atomic<std::uint64_t> m_requests = 0, m_requestErrors = 0, m_sentTsBytes = 0;
        std::shared_ptr<transport::Socket> m_listener;
        std::vector<std::unique_ptr<Connection>> m_connections;
        std::jthread m_accept;
    };
} // namespace bdr::server
