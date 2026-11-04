#pragma once
#include "BonDriverRelay/core/Configuration.h"
#include "BonDriverRelay/core/Logger.h"
#include "BonDriverRelay/core/TsDistributor.h"
#include "BonDriverRelay/core/TunerSession.h"
#include "BonDriverRelay/core/RuntimeSnapshot.h"
#include "BonDriverRelay/decoder/B25DecoderAdapter.h"
#include <atomic>
#include <future>
#include <map>
#include <thread>
namespace bdr::core
{
    // One stable owner thread for every native operation on one physical tuner.
    class TunerWorker
    {
      public:
        TunerWorker(const TunerDefinition& definition, const DecoderConfig& decoder_config, Logger* logger);
        ~TunerWorker();
        TunerWorker(const TunerWorker&) = delete;
        TunerWorker& operator=(const TunerWorker&) = delete;
        const protocol::Catalog& GetCatalog() const noexcept;
        std::future<void> Tune(const bon_driver::ChannelSelection& channel);
        void CloseStream();
        void Subscribe(std::uint64_t id, std::shared_ptr<TsQueue> queue);
        void Unsubscribe(std::uint64_t id);
        float GetSignal() const noexcept;
        std::uint32_t GetDecoderState() const noexcept;
        TunerSnapshot GetSnapshot() const;
        void RequestStop();
        void Join();

      private:
        struct Task
        {
            bon_driver::ChannelSelection channel;
            std::promise<void> completion;
        };
        void Run(std::stop_token stop, std::promise<void> ready);
        void Read(TunerSession& session, decoder::B25DecoderAdapter& decoder);
        void AccountDecoder(const decoder::B25DecoderAdapter& decoder);
        void Close(std::unique_ptr<TunerSession>& session,
                   std::unique_ptr<decoder::B25DecoderAdapter>& decoder) noexcept;
        void Fail(protocol::Status status);
        void Log(LogLevel level, const std::string& operation, const std::string& message) noexcept;
        const TunerDefinition m_definition;
        const DecoderConfig m_decoderConfig;
        Logger* m_logger;
        protocol::Catalog m_catalog;
        std::mutex m_mutex;
        std::condition_variable m_changed;
        std::optional<Task> m_task;
        bool m_closeRequested = false;
        bool m_stopping = false;
        std::mutex m_subMutex;
        std::map<std::uint64_t, std::shared_ptr<TsQueue>> m_subscribers;
        std::atomic<float> m_signal = 0;
        std::atomic<std::uint32_t> m_decoderState = 0;
        std::atomic<protocol::Status> m_fault = protocol::Status::Ok;
        std::atomic<TunerPhase> m_phase = TunerPhase::Idle;
        std::atomic<std::uint64_t> m_receivedBytes = 0, m_outputBytes = 0, m_decoderFailures = 0;
        std::atomic<std::uint64_t> m_tuneFailures = 0, m_cleanupFailures = 0;
        std::uint64_t m_observedDecoderFailures = 0;
        std::chrono::steady_clock::time_point m_nextDecoderLog{};
        std::uint64_t m_suppressedDecoderLogs = 0;
        std::jthread m_thread;
    };
} // namespace bdr::core
