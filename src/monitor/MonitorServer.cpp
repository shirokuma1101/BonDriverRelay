#include "BonDriverRelay/monitor/MonitorServer.h"
#include "BonDriverRelay/core/Error.h"
namespace bdr::monitor
{
    MonitorServer::MonitorServer(const core::MonitoringConfig& config, SnapshotProvider snapshot, ReloadProvider reload)
        : m_config(config), m_snapshot(std::move(snapshot)), m_reload(std::move(reload))
    {
        for (const auto& peer : config.peers) {
            m_peers.push_back({peer.name, peer.host, peer.port, false, 0, ""});
        }
        m_peerChecked.resize(m_peers.size(), std::chrono::steady_clock::now());
    }
    MonitorServer::~MonitorServer()
    {
        Stop();
    }
    void MonitorServer::Start()
    {
        if (!m_config.enabled) {
            return;
        }
        if (m_listener) {
            throw core::RelayError(core::ErrorCode::InvalidState, "Monitor already started");
        }
        auto listener = std::make_shared<transport::Socket>();
        listener->Listen(m_config.bindAddress, m_config.port);
        m_listener = std::move(listener);
        try {
            for (std::size_t index = 0; index < m_active.size(); ++index) {
                m_workers.emplace_back([this, index](std::stop_token stop) { Worker(stop, index); });
            }
            m_accept = std::jthread([this](std::stop_token stop) { Accept(stop); });
            m_peerWorker = std::jthread([this](std::stop_token stop) { PollPeers(stop); });
        }
        catch (...) {
            Stop();
            throw;
        }
    }
    void MonitorServer::Stop()
    {
        m_accept.request_stop();
        if (m_listener) {
            m_listener->Shutdown();
        }
        if (m_accept.joinable()) {
            m_accept.join();
        }
        for (auto& worker : m_workers) {
            worker.request_stop();
        }
        m_changed.notify_all();
        for (auto& active : m_active) {
            const auto socket = active.load();
            if (socket) {
                socket->Shutdown();
            }
        }
        {
            std::lock_guard lock(m_mutex);
            for (const auto& socket : m_queue) {
                socket->Shutdown();
            }
            m_queue.clear();
        }
        m_workers.clear();
        m_peerWorker.request_stop();
        m_peerChanged.notify_all();
        if (m_peerWorker.joinable()) {
            m_peerWorker.join();
        }
        m_listener.reset();
    }
    std::uint16_t MonitorServer::GetPort() const
    {
        return m_listener ? m_listener->GetPort() : 0;
    }
    std::vector<PeerSnapshot> MonitorServer::GetPeers() const
    {
        std::lock_guard lock(m_peerMutex);
        auto peers = m_peers;
        for (std::size_t i = 0; i < peers.size(); ++i) {
            peers[i].ageMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            std::chrono::steady_clock::now() - m_peerChecked[i])
                                                            .count());
        }
        return peers;
    }
    void MonitorServer::Accept(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            try {
                auto socket = m_listener->Accept(transport::After(100));
                if (!socket) {
                    continue;
                }
                std::lock_guard lock(m_mutex);
                if (m_queue.size() >= 16) {
                    socket->Shutdown();
                    continue;
                }
                m_queue.push_back(std::move(socket));
                m_changed.notify_one();
            }
            catch (...) {
                if (!stop.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
    }
    void MonitorServer::Worker(std::stop_token stop, std::size_t index)
    {
        while (!stop.stop_requested()) {
            std::shared_ptr<transport::Socket> socket;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, stop, [&] { return !m_queue.empty(); });
                if (stop.stop_requested()) {
                    return;
                }
                socket = m_queue.front();
                m_queue.pop_front();
                m_active[index].store(socket);
            }
            try {
                Serve(*socket);
            }
            catch (...) { /* Malformed, slow or disconnected monitor requests do not affect TS workers. */
            }
            socket->Shutdown();
            m_active[index].store(nullptr);
        }
    }
    void MonitorServer::Serve(const transport::Socket& socket)
    {
        const auto deadline = transport::After(1000);
        HttpHead request;
        try {
            request = ReadHttpHead(socket, deadline);
        }
        catch (...) {
            SendHttp(socket, 400, "text/plain; charset=utf-8", "Bad request\n", transport::After(100));
            return;
        }
        const auto separator = request.firstLine.find(' ');
        const auto version = request.firstLine.rfind(' ');
        if (separator == std::string::npos || version == separator ||
            (request.firstLine.substr(version + 1) != "HTTP/1.1" &&
             request.firstLine.substr(version + 1) != "HTTP/1.0") ||
            !request.bodyPrefix.empty() || request.headers.contains("transfer-encoding") ||
            (request.firstLine.substr(version + 1) == "HTTP/1.1" &&
             (!request.headers.contains("host") || request.headers.at("host").empty())) ||
            (request.headers.contains("content-length") && request.headers.at("content-length") != "0")) {
            SendHttp(socket, 400, "text/plain", "Bad request\n", deadline);
            return;
        }
        if (request.firstLine.substr(0, separator) != "GET") {
            SendHttp(socket, 405, "text/plain", request.firstLine.substr(0, separator) == "HEAD" ? "" : "Read only\n",
                     deadline);
            return;
        }
        const auto path = request.firstLine.substr(separator + 1, version - separator - 1);
        if (path == "/") {
            SendHttp(socket, 200, "text/html; charset=utf-8", DashboardHtml(), deadline);
        }
        else if (path == "/app.js") {
            SendHttp(socket, 200, "text/javascript; charset=utf-8", DashboardScript(), deadline);
        }
        else if (path == "/style.css") {
            SendHttp(socket, 200, "text/css; charset=utf-8", DashboardStyle(), deadline);
        }
        else if (path == "/api/local" || path == "/api/status" || path == "/metrics") {
            const auto snapshot = m_snapshot();
            const auto reload = m_reload();
            if (path == "/metrics") {
                SendHttp(socket, 200, "text/plain; version=0.0.4; charset=utf-8",
                         Metrics(m_config.name, snapshot, reload, GetPeers()), deadline);
            }
            else {
                auto json = LocalJson(m_config.name, snapshot, reload);
                if (path == "/api/status") {
                    json = StatusJson(json, GetPeers());
                }
                SendHttp(socket, 200, "application/json; charset=utf-8", json, deadline);
            }
        }
        else {
            SendHttp(socket, 404, "text/plain", "Not found\n", deadline);
        }
    }
    void MonitorServer::PollPeers(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            for (std::size_t index = 0; index < m_peers.size() && !stop.stop_requested(); ++index) {
                auto peer = m_peers[index];
                try {
                    peer.statusJson = FetchLocalStatus(peer.host, peer.port, m_config.peerTimeoutMs);
                    peer.up = true;
                }
                catch (...) {
                    peer.up = false;
                    peer.statusJson.clear();
                }
                std::lock_guard lock(m_peerMutex);
                m_peers[index] = std::move(peer);
                m_peerChecked[index] = std::chrono::steady_clock::now();
            }
            std::mutex mutex;
            std::unique_lock lock(mutex);
            m_peerChanged.wait_for(lock, stop, std::chrono::milliseconds(m_config.peerIntervalMs),
                                   [] { return false; });
        }
    }
} // namespace bdr::monitor
