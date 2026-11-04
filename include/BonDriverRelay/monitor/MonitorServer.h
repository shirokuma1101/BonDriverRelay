#pragma once
#include "BonDriverRelay/monitor/Http.h"
#include "BonDriverRelay/monitor/MonitorView.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
namespace bdr::monitor
{
    class MonitorServer
    {
      public:
        using SnapshotProvider = std::function<core::ServerSnapshot()>;
        using ReloadProvider = std::function<core::ReloadSnapshot()>;
        MonitorServer(const core::MonitoringConfig& config, SnapshotProvider snapshot, ReloadProvider reload);
        ~MonitorServer();
        void Start();
        void Stop();
        std::uint16_t GetPort() const;
        std::vector<PeerSnapshot> GetPeers() const;

      private:
        void Accept(std::stop_token stop);
        void Worker(std::stop_token stop, std::size_t index);
        void Serve(const transport::Socket& socket);
        void PollPeers(std::stop_token stop);
        const core::MonitoringConfig m_config;
        SnapshotProvider m_snapshot;
        ReloadProvider m_reload;
        std::shared_ptr<transport::Socket> m_listener;
        std::mutex m_mutex;
        std::condition_variable_any m_changed;
        std::condition_variable_any m_peerChanged;
        std::deque<std::shared_ptr<transport::Socket>> m_queue;
        std::array<std::atomic<std::shared_ptr<transport::Socket>>, 4> m_active;
        std::vector<std::jthread> m_workers;
        std::jthread m_accept, m_peerWorker;
        mutable std::mutex m_peerMutex;
        std::vector<PeerSnapshot> m_peers;
        std::vector<std::chrono::steady_clock::time_point> m_peerChecked;
    };
} // namespace bdr::monitor
