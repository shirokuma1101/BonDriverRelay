#pragma once
#include "BonDriverRelay/core/TunerWorker.h"
#include <future>
#include <map>
#include <set>
namespace bdr::core
{
    class ProtocolFailure : public std::runtime_error
    {
      public:
        ProtocolFailure(protocol::Status status, const std::string& message)
            : std::runtime_error(message), m_status(status)
        {}
        protocol::Status GetStatus() const noexcept
        {
            return m_status;
        }

      private:
        protocol::Status m_status;
    };
    struct ManagerReply
    {
        protocol::Bytes body;
        protocol::Bytes ts;
    };
    class TunerManager
    {
      public:
        explicit TunerManager(const ServerConfig& config, Logger* logger = nullptr);
        ~TunerManager();
        TunerManager(const TunerManager&) = delete;
        TunerManager& operator=(const TunerManager&) = delete;
        ManagerReply Request(std::uint64_t client, protocol::Opcode opcode, const protocol::Bytes& body,
                             std::uint32_t ts_limit, int priority = 0);
        void Disconnect(std::uint64_t client);
        void RequestStop();
        void Stop();
        ManagerSnapshot GetSnapshot() const;

      private:
        struct Job
        {
            std::uint64_t client = 0;
            protocol::Opcode opcode = protocol::Opcode::PollTs;
            protocol::Bytes body;
            std::uint32_t tsLimit = protocol::TS_PAYLOAD_LIMIT;
            int priority = 0;
            std::promise<ManagerReply> promise;
            std::atomic<bool> cancelled = false;
        };
        struct Subscriber
        {
            std::string group;
            bool exclusive = false;
            std::optional<std::size_t> tuner;
            std::shared_ptr<Job> pending;
            std::shared_ptr<TsQueue> queue;
            std::uint64_t droppedBase = 0;
        };
        struct Node
        {
            TunerDefinition definition;
            protocol::Catalog catalog;
            std::unique_ptr<TunerWorker> owner;
            std::optional<bon_driver::ChannelSelection> channel;
            bool exclusive = false;
            std::optional<std::future<void>> tuning;
            std::vector<std::shared_ptr<Job>> waiters;
        };
        void Worker(std::stop_token stop, std::promise<void> ready);
        void Initialize();
        ManagerReply Handle(std::uint64_t id, protocol::Opcode opcode, const protocol::Bytes& body,
                            std::uint32_t ts_limit);
        void BeginTune(const std::shared_ptr<Job>& job);
        void FinishTunes();
        void CollectFailures();
        void RefreshSnapshot();
        void Detach(std::uint64_t id);
        void RenewQueue(Subscriber& client);
        void Bind(std::uint64_t id, std::size_t tuner);
        void CloseNode(Node& node);
        std::size_t CountSubscribers(std::size_t tuner) const;
        void Log(LogLevel level, const std::string& tuner, std::uint64_t client, const std::string& operation,
                 const std::string& message) noexcept;
        const ServerConfig m_config;
        Logger* m_logger;
        std::vector<Node> m_nodes;
        std::map<std::uint64_t, Subscriber> m_clients;
        std::mutex m_mutex;
        std::condition_variable m_changed;
        std::deque<std::shared_ptr<Job>> m_jobs;
        std::set<std::uint64_t> m_registeredClients, m_pendingDisconnects;
        bool m_stopping = false;
        mutable std::mutex m_snapshotMutex;
        ManagerSnapshot m_snapshot;
        std::uint64_t m_retiredDropped = 0;
        std::jthread m_worker;
    };
} // namespace bdr::core
