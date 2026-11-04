#include "BonDriverRelay/core/TunerManager.h"
#include "BonDriverRelay/core/Error.h"
#include <algorithm>
#include <bit>
namespace bdr::core
{
    using protocol::Opcode;
    using protocol::Status;
    TunerManager::TunerManager(const ServerConfig& config, Logger* logger) : m_config(config), m_logger(logger)
    {
        std::promise<void> ready;
        auto future = ready.get_future();
        m_worker = std::jthread(
            [this, ready = std::move(ready)](std::stop_token stop) mutable { Worker(stop, std::move(ready)); });
        try {
            future.get();
        }
        catch (...) {
            Stop();
            throw;
        }
    }
    TunerManager::~TunerManager()
    {
        Stop();
    }
    void TunerManager::Stop()
    {
        RequestStop();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }
    void TunerManager::RequestStop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_worker.request_stop();
        m_changed.notify_all();
    }
    void TunerManager::Initialize()
    {
        if (m_config.tuners.empty()) {
            throw RelayError(ErrorCode::Configuration, "Network server requires at least one tuner");
        }
        for (const auto& definition : m_config.tuners) {
            for (const auto& other : m_nodes) {
                if (std::filesystem::equivalent(other.definition.dllPath, definition.dllPath)) {
                    throw RelayError(ErrorCode::Configuration, "A physical BonDriver DLL cannot be registered twice");
                }
            }
            Node node;
            node.definition = definition;
            node.owner = std::make_unique<TunerWorker>(definition, m_config.decoder, m_logger);
            node.catalog = node.owner->GetCatalog();
            (void)protocol::EncodeCatalog(node.catalog);
            for (const auto& other : m_nodes) {
                if (other.definition.group == definition.group && other.catalog != node.catalog) {
                    throw RelayError(ErrorCode::Configuration,
                                     "Tuners in a group must expose identical channel catalogs");
                }
            }
            m_nodes.push_back(std::move(node));
        }
    }
    void TunerManager::Worker(std::stop_token stop, std::promise<void> ready)
    {
        try {
            Initialize();
            RefreshSnapshot();
            ready.set_value();
        }
        catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        auto next_snapshot = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            FinishTunes();
            CollectFailures();
            if (std::chrono::steady_clock::now() >= next_snapshot) {
                RefreshSnapshot();
                next_snapshot = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            }
            std::shared_ptr<Job> job;
            std::set<std::uint64_t> disconnected;
            {
                std::unique_lock lock(m_mutex);
                if (m_jobs.empty()) {
                    m_changed.wait_for(lock, std::chrono::milliseconds(1),
                                       [&] { return !m_jobs.empty() || !m_pendingDisconnects.empty() || m_stopping; });
                }
                disconnected.swap(m_pendingDisconnects);
                if (!m_jobs.empty()) {
                    auto next = m_jobs.begin();
                    if ((*next)->opcode == Opcode::Tune) {
                        for (auto candidate = next; candidate != m_jobs.end() && (*candidate)->opcode == Opcode::Tune;
                             ++candidate) {
                            if ((*candidate)->priority > (*next)->priority) {
                                next = candidate;
                            }
                        }
                    }
                    job = *next;
                    m_jobs.erase(next);
                }
            }
            for (const auto id : disconnected) {
                try {
                    (void)Handle(id, Opcode::Close, {}, protocol::TS_PAYLOAD_LIMIT);
                }
                catch (const std::exception& error) {
                    Log(LogLevel::Error, "-", id, "disconnect", error.what());
                }
            }
            if (job && !job->cancelled.load()) {
                try {
                    if (job->opcode == Opcode::Tune) {
                        BeginTune(job);
                    }
                    else {
                        job->promise.set_value(Handle(job->client, job->opcode, job->body, job->tsLimit));
                    }
                }
                catch (...) {
                    if (job->opcode == Opcode::Tune && m_clients.contains(job->client)) {
                        Detach(job->client);
                        RenewQueue(m_clients.at(job->client));
                    }
                    job->promise.set_exception(std::current_exception());
                }
            }
        }
        for (auto& [id, client] : m_clients) {
            (void)id;
            client.queue->Close(Status::Unavailable);
            m_retiredDropped += client.droppedBase + client.queue->GetDroppedPackets();
        }
        m_clients.clear();
        for (auto& node : m_nodes) {
            for (auto& job : node.waiters) {
                job->promise.set_exception(
                    std::make_exception_ptr(ProtocolFailure(Status::Unavailable, "Server stopped")));
            }
        }
        {
            std::lock_guard lock(m_mutex);
            for (auto& job : m_jobs) {
                job->promise.set_exception(
                    std::make_exception_ptr(ProtocolFailure(Status::Unavailable, "Server stopped")));
            }
            m_jobs.clear();
            m_registeredClients.clear();
            m_pendingDisconnects.clear();
        }
        for (auto& node : m_nodes) {
            node.owner->RequestStop();
        }
        // Fulfil waiting requests before native shutdown joins.
        for (auto& node : m_nodes) {
            node.owner->Join();
        }
        RefreshSnapshot();
    }
    ManagerSnapshot TunerManager::GetSnapshot() const
    {
        std::lock_guard lock(m_snapshotMutex);
        return m_snapshot;
    }
    void TunerManager::RefreshSnapshot()
    {
        ManagerSnapshot snapshot;
        snapshot.droppedPackets = m_retiredDropped;
        for (const auto& node : m_nodes) {
            snapshot.tuners.push_back(node.owner->GetSnapshot());
        }
        for (const auto& [id, client] : m_clients) {
            (void)id;
            const auto queued = client.queue->GetBytes();
            const auto dropped = client.droppedBase + client.queue->GetDroppedPackets();
            snapshot.queuedBytes += queued;
            snapshot.droppedPackets += dropped;
            snapshot.clients.push_back({id, queued, dropped, client.group,
                                        client.tuner ? m_nodes[*client.tuner].definition.name : "", client.exclusive,
                                        client.pending != nullptr});
            if (client.tuner) {
                ++snapshot.subscriptions;
                auto& tuner = snapshot.tuners[*client.tuner];
                ++tuner.subscribers;
                tuner.queuedBytes += queued;
                tuner.droppedPackets += dropped;
            }
        }
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(snapshot);
    }
    ManagerReply TunerManager::Request(std::uint64_t id, Opcode opcode, const protocol::Bytes& body,
                                       std::uint32_t ts_limit, int priority)
    {
        auto job = std::make_shared<Job>();
        job->client = id;
        job->opcode = opcode;
        job->body = body;
        job->tsLimit = ts_limit;
        job->priority = priority;
        auto future = job->promise.get_future();
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                throw ProtocolFailure(Status::Unavailable, "Server is stopping");
            }
            std::erase_if(m_jobs, [](const auto& pending) { return pending->cancelled.load(); });
            if (m_jobs.size() >= m_config.network.maxClients * 2) {
                throw ProtocolFailure(Status::QueueSaturated, "Control queue is full");
            }
            m_jobs.push_back(job);
        }
        m_changed.notify_one();
        if (future.wait_for(std::chrono::milliseconds(m_config.network.ioTimeoutMs)) != std::future_status::ready) {
            if (opcode != Opcode::Close) {
                job->cancelled.store(true);
            }
            throw ProtocolFailure(Status::Timeout, "Tuner operation deadline exceeded");
        }
        return future.get();
    }
    void TunerManager::Disconnect(std::uint64_t id)
    {
        {
            std::lock_guard lock(m_mutex);
            if (!m_stopping && m_registeredClients.contains(id)) {
                m_pendingDisconnects.insert(id);
            }
        }
        m_changed.notify_one();
    }
    std::size_t TunerManager::CountSubscribers(std::size_t tuner) const
    {
        return static_cast<std::size_t>(std::count_if(m_clients.begin(), m_clients.end(),
                                                      [&](const auto& pair) { return pair.second.tuner == tuner; }));
    }
    void TunerManager::CloseNode(Node& node)
    {
        node.owner->CloseStream();
        node.channel.reset();
        node.exclusive = false;
    }
    void TunerManager::Detach(std::uint64_t id)
    {
        auto& client = m_clients.at(id);
        client.pending.reset();
        const auto old = client.tuner;
        client.tuner.reset();
        if (old) {
            m_nodes[*old].owner->Unsubscribe(id);
            if (!m_nodes[*old].tuning && CountSubscribers(*old) == 0) {
                CloseNode(m_nodes[*old]);
            }
        }
        // An in-flight producer may still own a snapshot: close this generation.
        client.queue->Close(Status::Unavailable);
    }
    void TunerManager::RenewQueue(Subscriber& client)
    {
        client.droppedBase += client.queue->GetDroppedPackets();
        client.queue = std::make_shared<TsQueue>(m_config.distribution.queueBytesPerClient,
                                                 m_config.distribution.overflowPolicy == "drop_oldest");
    }
    void TunerManager::Bind(std::uint64_t id, std::size_t tuner)
    {
        auto& client = m_clients.at(id);
        client.tuner = tuner;
        client.pending.reset();
        m_nodes[tuner].owner->Subscribe(id, client.queue);
    }
    void TunerManager::BeginTune(const std::shared_ptr<Job>& job)
    {
        const auto id = job->client;
        if (!m_clients.contains(id)) {
            throw ProtocolFailure(Status::InvalidState, "Open is required");
        }
        auto& client = m_clients.at(id);
        if (client.pending && !client.pending->cancelled.load()) {
            throw ProtocolFailure(Status::InvalidState, "Tune already pending");
        }
        protocol::Reader in(job->body);
        const auto legacy = in.U32(), space = in.U32(), channel = in.U32();
        in.End();
        if (legacy > 1 || (legacy && channel > 255)) {
            throw ProtocolFailure(Status::InvalidFrame, "Invalid channel selection");
        }
        const bon_driver::ChannelSelection selection{space, channel, legacy != 0};
        if (client.tuner && m_nodes[*client.tuner].channel == selection) {
            const auto same = *client.tuner;
            m_nodes[same].owner->Unsubscribe(id);
            client.queue->Close(Status::Unavailable);
            RenewQueue(client);
            Bind(id, same);
            job->promise.set_value({});
            return;
        }
        std::optional<std::size_t> target;
        for (std::size_t i = 0; i < m_nodes.size(); ++i) {
            const auto& node = m_nodes[i];
            if (node.definition.group == client.group && node.channel == selection && !node.exclusive &&
                !client.exclusive) {
                target = i;
                break;
            }
        }
        if (!target) {
            for (std::size_t i = 0; i < m_nodes.size(); ++i) {
                if (m_nodes[i].definition.group == client.group && !m_nodes[i].channel && !m_nodes[i].tuning) {
                    target = i;
                    break;
                }
            }
        }
        if (!target) {
            if (client.tuner && CountSubscribers(*client.tuner) == 1 && !m_nodes[*client.tuner].tuning) {
                target = client.tuner;
            }
        }
        if (!target) {
            throw ProtocolFailure(Status::TunerBusy, "No idle tuner; existing subscriptions are protected");
        }
        auto& node = m_nodes[*target];
        if (!selection.legacy && (!node.catalog.extended || space >= node.catalog.channels.size() ||
                                  channel >= node.catalog.channels[space].size())) {
            throw ProtocolFailure(Status::Unavailable, "Unknown space/channel");
        }
        Detach(id);
        RenewQueue(client);
        if (node.tuning) {
            node.waiters.push_back(job);
            client.pending = job;
            return;
        }
        if (node.channel == selection) {
            Bind(id, *target);
            job->promise.set_value({});
            return;
        }
        node.tuning.emplace(node.owner->Tune(selection));
        node.channel = selection;
        node.exclusive = client.exclusive;
        node.waiters.push_back(job);
        client.pending = job;
    }
    void TunerManager::FinishTunes()
    {
        for (std::size_t index = 0; index < m_nodes.size(); ++index) {
            auto& node = m_nodes[index];
            if (!node.tuning) {
                continue;
            }
            std::erase_if(node.waiters, [&](const auto& job) {
                const bool stale = job->cancelled.load() || !m_clients.contains(job->client) ||
                                   m_clients.at(job->client).pending != job;
                if (stale) {
                    job->promise.set_exception(
                        std::make_exception_ptr(ProtocolFailure(Status::Unavailable, "Tune subscription cancelled")));
                }
                return stale;
            });
            if (node.tuning->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                continue;
            }
            std::exception_ptr failure;
            try {
                node.tuning->get();
            }
            catch (...) {
                failure = std::current_exception();
            }
            node.tuning.reset();
            for (auto& job : node.waiters) {
                if (job->cancelled.load() || !m_clients.contains(job->client) ||
                    m_clients.at(job->client).pending != job) {
                    job->promise.set_exception(
                        std::make_exception_ptr(ProtocolFailure(Status::Unavailable, "Tune cancelled")));
                    continue;
                }
                auto& client = m_clients.at(job->client);
                client.pending.reset();
                if (failure) {
                    job->promise.set_exception(failure);
                }
                else {
                    Bind(job->client, index);
                    job->promise.set_value({});
                }
            }
            node.waiters.clear();
            if (failure || CountSubscribers(index) == 0) {
                CloseNode(node);
            }
        }
    }
    void TunerManager::CollectFailures()
    {
        std::vector<std::uint64_t> failed;
        for (const auto& [id, client] : m_clients) {
            if (client.tuner && client.queue->GetError() != Status::Ok) {
                failed.push_back(id);
            }
        }
        for (const auto id : failed) {
            auto& client = m_clients.at(id);
            Log(LogLevel::Warn, m_nodes[*client.tuner].definition.name, id, "distribute",
                "subscription detached; dropped_packets=" +
                    std::to_string(client.droppedBase + client.queue->GetDroppedPackets()));
            Detach(id);
        }
    }
    ManagerReply TunerManager::Handle(std::uint64_t id, Opcode opcode, const protocol::Bytes& body,
                                      std::uint32_t ts_limit)
    {
        protocol::Reader in(body);
        ManagerReply result;
        protocol::Writer out;
        if (opcode == Opcode::Open) {
            if (m_clients.contains(id)) {
                throw ProtocolFailure(Status::InvalidState, "Client is already open");
            }
            const auto group = in.Text();
            const auto priority = std::bit_cast<std::int32_t>(in.U32());
            const auto exclusive = in.U32();
            in.End();
            if (priority < -128 || priority > 127 || exclusive > 1) {
                throw ProtocolFailure(Status::InvalidFrame, "Invalid open options");
            }
            if (m_clients.size() >= m_config.network.maxClients) {
                throw ProtocolFailure(Status::Unavailable, "Client limit reached");
            }
            for (const auto& node : m_nodes) {
                if (node.definition.group == group) {
                    Subscriber client;
                    client.group = group;
                    client.exclusive = exclusive != 0;
                    client.queue = std::make_shared<TsQueue>(m_config.distribution.queueBytesPerClient,
                                                             m_config.distribution.overflowPolicy == "drop_oldest");
                    m_clients.emplace(id, std::move(client));
                    result.body = protocol::EncodeCatalog(node.catalog);
                    {
                        std::lock_guard lock(m_mutex);
                        m_registeredClients.insert(id);
                    }
                    return result;
                }
            }
            throw ProtocolFailure(Status::Unavailable, "Unknown tuner group");
        }
        if (opcode == Opcode::Close) {
            in.End();
            if (m_clients.contains(id)) {
                Detach(id);
                m_retiredDropped += m_clients.at(id).droppedBase + m_clients.at(id).queue->GetDroppedPackets();
                m_clients.erase(id);
            }
            {
                std::lock_guard lock(m_mutex);
                m_registeredClients.erase(id);
            }
            return result;
        }
        if (!m_clients.contains(id)) {
            throw ProtocolFailure(Status::InvalidState, "Open is required");
        }
        auto& client = m_clients.at(id);
        if (opcode == Opcode::Purge) {
            in.End();
            if (client.queue->GetError() != Status::Ok) {
                throw ProtocolFailure(client.queue->GetError(), "Failed subscription requires retuning or reopening");
            }
            const auto tuner = client.tuner;
            if (tuner) {
                m_nodes[*tuner].owner->Unsubscribe(id);
            }
            client.queue->Close(Status::Unavailable);
            RenewQueue(client);
            if (tuner) {
                m_nodes[*tuner].owner->Subscribe(id, client.queue);
            }
            return result;
        }
        if (opcode == Opcode::PollTs) {
            in.End();
            const auto error = client.queue->GetError();
            if (error != Status::Ok) {
                throw ProtocolFailure(error,
                                      "Subscription failed (queue/stream); dropped_packets=" +
                                          std::to_string(client.droppedBase + client.queue->GetDroppedPackets()));
            }
            result.ts = client.queue->Pop(ts_limit);
            out.U32(client.tuner ? m_nodes[*client.tuner].owner->GetDecoderState() : 0);
            out.U32(std::bit_cast<std::uint32_t>(client.tuner ? m_nodes[*client.tuner].owner->GetSignal() : 0.0f));
            out.U64(client.droppedBase + client.queue->GetDroppedPackets());
            result.body = std::move(out.data);
            return result;
        }
        throw ProtocolFailure(Status::UnsupportedCommand, "Unknown opcode");
    }
    void TunerManager::Log(LogLevel level, const std::string& tuner, std::uint64_t client, const std::string& operation,
                           const std::string& message) noexcept
    {
        try {
            if (m_logger) {
                m_logger->Write(level, tuner, operation, message, 0, std::to_string(client));
            }
        }
        catch (...) {
            OutputDebugStringW(L"BonDriverRelay: diagnostic logging failed.\n");
        }
    }
} // namespace bdr::core
