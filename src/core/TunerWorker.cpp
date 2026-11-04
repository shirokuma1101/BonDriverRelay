#include "BonDriverRelay/core/TunerWorker.h"
#include "BonDriverRelay/core/TunerManager.h"
#include "BonDriverRelay/core/Paths.h"
#include "BonDriverRelay/core/Error.h"
#include <utility>
namespace bdr::core
{
    namespace
    {
        protocol::Catalog Catalog(TunerSession& session)
        {
            protocol::Catalog catalog;
            catalog.extended = session.HasExtendedInterface();
            catalog.tunerName = "BonDriverRelay";
            if (!catalog.extended) {
                return catalog;
            }
            for (std::uint32_t space = 0; space <= 64; ++space) {
                const auto name = session.GetSpaceName(space);
                if (!name) {
                    (void)protocol::EncodeCatalog(catalog);
                    return catalog;
                }
                if (space == 64) {
                    throw RelayError(ErrorCode::Configuration, "Space catalog exceeds limit");
                }
                catalog.spaces.push_back(ToUtf8(*name));
                auto& channels = catalog.channels.emplace_back();
                bool ended = false;
                for (std::uint32_t channel = 0; channel <= 4096; ++channel) {
                    const auto channel_name = session.GetChannelName(space, channel);
                    if (!channel_name) {
                        ended = true;
                        break;
                    }
                    if (channel == 4096) {
                        break;
                    }
                    channels.push_back(ToUtf8(*channel_name));
                }
                if (!ended) {
                    throw RelayError(ErrorCode::Configuration, "Channel catalog exceeds limit");
                }
            }
            throw RelayError(ErrorCode::Configuration, "Space catalog exceeds limit");
        }
    } // namespace
    TunerWorker::TunerWorker(const TunerDefinition& definition, const DecoderConfig& decoder_config, Logger* logger)
        : m_definition(definition), m_decoderConfig(decoder_config), m_logger(logger)
    {
        std::promise<void> ready;
        auto future = ready.get_future();
        m_thread = std::jthread(
            [this, ready = std::move(ready)](std::stop_token stop) mutable { Run(stop, std::move(ready)); });
        try {
            future.get();
        }
        catch (...) {
            RequestStop();
            Join();
            throw;
        }
    }
    TunerWorker::~TunerWorker()
    {
        RequestStop();
        Join();
    }
    const protocol::Catalog& TunerWorker::GetCatalog() const noexcept
    {
        return m_catalog;
    }
    std::future<void> TunerWorker::Tune(const bon_driver::ChannelSelection& channel)
    {
        Task task;
        task.channel = channel;
        auto result = task.completion.get_future();
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                throw ProtocolFailure(protocol::Status::Unavailable, "Tuner is stopping");
            }
            if (m_task) {
                throw ProtocolFailure(protocol::Status::TunerBusy, "Tuner command is already pending");
            }
            m_task.emplace(std::move(task));
        }
        m_changed.notify_one();
        return result;
    }
    void TunerWorker::CloseStream()
    {
        {
            std::lock_guard lock(m_mutex);
            m_closeRequested = true;
        }
        m_changed.notify_one();
    }
    void TunerWorker::Subscribe(std::uint64_t id, std::shared_ptr<TsQueue> queue)
    {
        std::lock_guard lock(m_subMutex);
        m_subscribers[id] = queue;
        const auto error = m_fault.load();
        if (error != protocol::Status::Ok) {
            queue->Close(error);
        }
    }
    void TunerWorker::Unsubscribe(std::uint64_t id)
    {
        std::lock_guard lock(m_subMutex);
        m_subscribers.erase(id);
    }
    float TunerWorker::GetSignal() const noexcept
    {
        return m_signal.load();
    }
    std::uint32_t TunerWorker::GetDecoderState() const noexcept
    {
        return m_decoderState.load();
    }
    TunerSnapshot TunerWorker::GetSnapshot() const
    {
        TunerSnapshot snapshot;
        snapshot.name = m_definition.name;
        snapshot.phase = m_phase.load();
        snapshot.decoderState = m_decoderState.load();
        snapshot.receivedBytes = m_receivedBytes.load();
        snapshot.outputBytes = m_outputBytes.load();
        snapshot.decoderFailures = m_decoderFailures.load();
        snapshot.tuneFailures = m_tuneFailures.load();
        snapshot.cleanupFailures = m_cleanupFailures.load();
        return snapshot;
    }
    void TunerWorker::AccountDecoder(const decoder::B25DecoderAdapter& decoder)
    {
        const auto failures = decoder.GetFailureCount();
        m_decoderFailures.fetch_add(failures - m_observedDecoderFailures);
        m_observedDecoderFailures = failures;
        m_decoderState.store(static_cast<std::uint32_t>(decoder.GetState()));
    }
    void TunerWorker::RequestStop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_thread.request_stop();
        m_changed.notify_all();
    }
    void TunerWorker::Join()
    {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    void TunerWorker::Log(LogLevel level, const std::string& operation, const std::string& message) noexcept
    {
        try {
            if (m_logger) {
                m_logger->Write(level, m_definition.name, operation, message);
            }
        }
        catch (...) {
            OutputDebugStringW(L"BonDriverRelay: tuner diagnostic failed.\n");
        }
    }
    void TunerWorker::Close(std::unique_ptr<TunerSession>& session,
                            std::unique_ptr<decoder::B25DecoderAdapter>& decoder) noexcept
    {
        if (decoder) {
            AccountDecoder(*decoder);
            try {
                decoder->Close();
            }
            catch (const std::exception& error) {
                ++m_cleanupFailures;
                Log(LogLevel::Error, "decoder_close", error.what());
            }
            decoder.reset();
        }
        if (session) {
            try {
                session->Close();
            }
            catch (const std::exception& error) {
                ++m_cleanupFailures;
                Log(LogLevel::Error, "tuner_close", error.what());
            }
            session.reset();
        }
        m_signal.store(0);
        m_decoderState.store(0);
        if (m_phase.load() != TunerPhase::Fault) {
            m_phase.store(TunerPhase::Idle);
        }
    }
    void TunerWorker::Fail(protocol::Status status)
    {
        m_fault.store(status);
        std::lock_guard lock(m_subMutex);
        for (const auto& [id, queue] : m_subscribers) {
            (void)id;
            queue->Close(status);
        }
    }
    void TunerWorker::Read(TunerSession& session, decoder::B25DecoderAdapter& decoder)
    {
        auto received = session.Receive(std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
        m_signal.store(session.GetSignalLevel());
        if (received.status != ReceiveStatus::Data) {
            return;
        }
        const auto previous = decoder.GetState();
        m_receivedBytes.fetch_add(received.data.size());
        auto decoded = decoder.Decode(received.data);
        AccountDecoder(decoder);
        m_outputBytes.fetch_add(decoded.size());
        if (decoder.GetState() != previous) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= m_nextDecoderLog) {
                const auto level =
                    decoder.GetState() == decoder::DecoderState::ActiveUnverified ? LogLevel::Info : LogLevel::Warn;
                Log(level, "decode",
                    "decoder_state=" + std::to_string(m_decoderState.load()) +
                        " failures=" + std::to_string(m_decoderFailures.load()) +
                        " suppressed_transitions=" + std::to_string(m_suppressedDecoderLogs));
                m_suppressedDecoderLogs = 0;
                m_nextDecoderLog = now + std::chrono::seconds(5);
            }
            else {
                ++m_suppressedDecoderLogs;
            }
        }
        if (decoded.empty()) {
            return;
        }
        const auto data = std::make_shared<const protocol::Bytes>(std::move(decoded));
        std::vector<std::shared_ptr<TsQueue>> queues;
        {
            std::lock_guard lock(m_subMutex);
            for (const auto& [id, queue] : m_subscribers) {
                (void)id;
                queues.push_back(queue);
            }
        }
        for (const auto& queue : queues) {
            (void)queue->Push(data);
        }
    }
    void TunerWorker::Run(std::stop_token stop, std::promise<void> ready)
    {
        try {
            TunerSession probe;
            probe.Load(m_definition.dllPath);
            probe.Open();
            m_catalog = Catalog(probe);
            probe.Close();
            ready.set_value();
        }
        catch (...) {
            ready.set_exception(std::current_exception());
            return;
        }
        std::unique_ptr<TunerSession> session;
        std::unique_ptr<decoder::B25DecoderAdapter> decoder;
        while (!stop.stop_requested()) {
            std::optional<Task> task;
            bool close = false;
            {
                std::unique_lock lock(m_mutex);
                if (!session && !m_task && !m_closeRequested && !m_stopping) {
                    m_changed.wait(lock, [&] { return m_task || m_closeRequested || m_stopping; });
                }
                if (m_stopping) {
                    break;
                }
                task.swap(m_task);
                close = std::exchange(m_closeRequested, false);
            }
            if (close || task) {
                Close(session, decoder);
            }
            if (task) {
                m_phase.store(TunerPhase::Tuning);
                try {
                    m_fault.store(protocol::Status::Ok);
                    session = std::make_unique<TunerSession>();
                    session->Load(m_definition.dllPath);
                    session->Open();
                    Log(LogLevel::Info, "tune_begin",
                        "space=" + std::to_string(task->channel.space) +
                            " channel=" + std::to_string(task->channel.channel));
                    session->Tune(task->channel);
                    decoder = std::make_unique<decoder::B25DecoderAdapter>();
                    m_observedDecoderFailures = 0;
                    m_nextDecoderLog = {};
                    m_suppressedDecoderLogs = 0;
                    try {
                        decoder->Open(m_decoderConfig);
                    }
                    catch (const std::exception& error) {
                        throw ProtocolFailure(protocol::Status::DecoderFailure, error.what());
                    }
                    m_decoderState.store(static_cast<std::uint32_t>(decoder->GetState()));
                    AccountDecoder(*decoder);
                    m_phase.store(TunerPhase::Streaming);
                    Log(LogLevel::Info, "tune", "stream opened decoder_state=" + std::to_string(m_decoderState.load()));
                    task->completion.set_value();
                }
                catch (...) {
                    ++m_tuneFailures;
                    m_phase.store(TunerPhase::Fault);
                    Close(session, decoder);
                    task->completion.set_exception(std::current_exception());
                }
            }
            if (session && decoder) {
                try {
                    Read(*session, *decoder);
                }
                catch (const std::exception& error) {
                    m_phase.store(TunerPhase::Fault);
                    AccountDecoder(*decoder);
                    Log(LogLevel::Error, "receive", error.what());
                    const auto fault = decoder->GetState() == decoder::DecoderState::DecodeFailed
                                           ? protocol::Status::DecoderFailure
                                           : protocol::Status::Unavailable;
                    Fail(fault);
                    Close(session, decoder);
                }
            }
        }
        Close(session, decoder);
        m_phase.store(TunerPhase::Stopped);
        Fail(protocol::Status::Unavailable);
        {
            std::lock_guard lock(m_mutex);
            if (m_task) {
                m_task->completion.set_exception(
                    std::make_exception_ptr(ProtocolFailure(protocol::Status::Unavailable, "Tuner stopped")));
                m_task.reset();
            }
        }
    }
} // namespace bdr::core
