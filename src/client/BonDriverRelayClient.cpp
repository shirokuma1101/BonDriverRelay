#include "BonDriverRelay/client/BonDriverRelayClient.h"
#include "BonDriverRelay/core/TunerManager.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/core/Paths.h"
#include <bit>
#include <cmath>

namespace bdr::client
{
    BonDriverRelayClient::BonDriverRelayClient(const core::ClientConfig& config)
        : m_config(config), m_queue(config.queueBytes)
    {
        m_queue.Close(protocol::Status::Unavailable);
    }
    BonDriverRelayClient::~BonDriverRelayClient()
    {
        CloseTuner();
    }
    void BonDriverRelayClient::Error(const std::exception& error) noexcept
    {
        DWORD code = ERROR_GEN_FAILURE;
        if (const auto* protocol_error = dynamic_cast<const core::ProtocolFailure*>(&error)) {
            switch (protocol_error->GetStatus()) {
            case protocol::Status::TunerBusy:
                code = ERROR_BUSY;
                break;
            case protocol::Status::QueueSaturated:
                code = ERROR_BUFFER_OVERFLOW;
                break;
            case protocol::Status::Timeout:
                code = ERROR_TIMEOUT;
                break;
            case protocol::Status::DecoderFailure:
                code = ERROR_INVALID_DATA;
                m_decoderState.store(4);
                break;
            default:
                code = ERROR_NOT_READY;
                break;
            }
        }
        else if (const auto* relay = dynamic_cast<const core::RelayError*>(&error)) {
            code = relay->GetNativeCode()
                       ? relay->GetNativeCode()
                       : (relay->GetCode() == core::ErrorCode::ReceiveTimeout ? ERROR_TIMEOUT : ERROR_INVALID_DATA);
        }
        m_lastError.store(code);
        SetLastError(code);
        OutputDebugStringA("BonDriverRelay: operation failed; inspect GetRelayStatus and server diagnostics.\n");
    }
    const BOOL BonDriverRelayClient::OpenTuner()
    {
        std::lock_guard lock(m_apiMutex);
        if (m_open.load()) {
            return TRUE;
        }
        try {
            Close();
            auto remote = std::make_unique<RemoteClient>(m_config);
            auto catalog = remote->Open();
            if (m_hasCatalog && catalog != m_catalog) {
                throw core::RelayError(core::ErrorCode::InvalidStream,
                                       "Catalog changed; recreate the BonDriver instance");
            }
            if (!m_hasCatalog) {
                m_spaces.clear();
                m_channels.clear();
                for (const auto& name : catalog.spaces) {
                    m_spaces.push_back(core::PathFromUtf8(name).native());
                }
                for (const auto& names : catalog.channels) {
                    auto& channels = m_channels.emplace_back();
                    for (const auto& name : names) {
                        channels.push_back(core::PathFromUtf8(name).native());
                    }
                }
                m_catalog = std::move(catalog);
                m_hasCatalog = true;
            }
            m_remote = std::move(remote);
            m_queue.Reset();
            m_open.store(true);
            m_lastError.store(ERROR_SUCCESS);
            m_reconnects.fetch_add(1);
            StartPump();
            return TRUE;
        }
        catch (const std::exception& error) {
            Error(error);
            Close();
            return FALSE;
        }
    }
    void BonDriverRelayClient::StopPump()
    {
        m_pump.request_stop();
        if (m_pump.joinable()) {
            m_pump.join();
        }
    }
    void BonDriverRelayClient::StartPump()
    {
        m_pump = std::jthread([this](std::stop_token stop) {
            try {
                while (!stop.stop_requested()) {
                    auto reply = m_remote->Call(protocol::Opcode::PollTs);
                    protocol::Reader in(reply.body);
                    const auto decoder = in.U32();
                    const float signal = std::bit_cast<float>(in.U32());
                    const auto drops = in.U64();
                    in.End();
                    if (!std::isfinite(signal) || decoder > 5) {
                        throw core::RelayError(core::ErrorCode::InvalidStream, "Invalid stream status");
                    }
                    const auto previous = m_decoderState.exchange(decoder);
                    if (decoder != previous) {
                        OutputDebugStringA(
                            "BonDriverRelay: decoder state changed; GetRelayStatus reports the new state.\n");
                    }
                    m_signal.store(signal);
                    m_remoteDrops.store(drops);
                    if (!reply.ts.empty()) {
                        m_receivedBytes.fetch_add(reply.ts.size());
                        if (!m_queue.Push(std::make_shared<const protocol::Bytes>(std::move(reply.ts)))) {
                            throw core::ProtocolFailure(protocol::Status::QueueSaturated,
                                                        "Client receive queue exceeded");
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            catch (const std::exception& error) {
                Error(error);
                m_open.store(false);
                m_queue.Close(protocol::Status::Unavailable);
                m_remote->Shutdown();
            }
        });
    }
    void BonDriverRelayClient::Close()
    {
        StopPump();
        m_open.store(false);
        m_queue.Close(protocol::Status::Unavailable);
        if (m_remote) {
            try {
                if (m_remote->IsAlive()) {
                    (void)m_remote->Call(protocol::Opcode::Close);
                }
            }
            catch (...) {
            }
            m_remote->Shutdown();
            m_remote.reset();
        }
        m_returnedTs.clear();
        m_space.store(0xffffffff);
        m_channel.store(0xffffffff);
        m_signal.store(0);
    }
    void BonDriverRelayClient::CloseTuner()
    {
        try {
            std::lock_guard lock(m_apiMutex);
            Close();
        }
        catch (...) {
            SetLastError(ERROR_GEN_FAILURE);
        }
    }
    BOOL BonDriverRelayClient::Tune(bool legacy, DWORD space, DWORD channel)
    {
        std::lock_guard lock(m_apiMutex);
        if (!m_open.load()) {
            SetLastError(ERROR_NOT_READY);
            return FALSE;
        }
        StopPump();
        m_queue.Reset();
        m_returnedTs.clear();
        m_space.store(0xffffffff);
        m_channel.store(0xffffffff);
        try {
            protocol::Writer out;
            out.U32(legacy ? 1u : 0u);
            out.U32(space);
            out.U32(channel);
            const auto result = m_remote->Call(protocol::Opcode::Tune, out.data);
            protocol::Reader(result.body).End();
            m_space.store(space);
            m_channel.store(channel);
            m_lastError.store(ERROR_SUCCESS);
            StartPump();
            return TRUE;
        }
        catch (const std::exception& error) {
            Error(error);
            if (m_remote && m_remote->IsAlive()) {
                StartPump();
            }
            return FALSE;
        }
    }
    const BOOL BonDriverRelayClient::SetChannel(const BYTE channel)
    {
        return Tune(true, 0, channel);
    }
    const BOOL BonDriverRelayClient::SetChannel(const DWORD space, const DWORD channel)
    {
        return Tune(false, space, channel);
    }
    const float BonDriverRelayClient::GetSignalLevel()
    {
        return m_signal.load();
    }
    const DWORD BonDriverRelayClient::WaitTsStream(const DWORD timeout)
    {
        try {
            return m_queue.Wait(timeout);
        }
        catch (...) {
            return WAIT_FAILED;
        }
    }
    const DWORD BonDriverRelayClient::GetReadyCount()
    {
        return m_queue.GetReadyCount();
    }
    BOOL BonDriverRelayClient::ReadStream(BYTE* copy, BYTE** borrowed, DWORD* size, DWORD* remaining)
    {
        if (size) {
            *size = 0;
        }
        if (remaining) {
            *remaining = 0;
        }
        if (borrowed) {
            *borrowed = nullptr;
        }
        if (!size || !remaining || (!copy && !borrowed)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        try {
            std::lock_guard lock(m_apiMutex);
            m_returnedTs = m_queue.Pop(188 * 256);
            if (m_returnedTs.empty()) {
                return FALSE;
            }
            *size = static_cast<DWORD>(m_returnedTs.size());
            *remaining = m_queue.GetReadyCount();
            if (borrowed) {
                *borrowed = m_returnedTs.data();
            }
            else {
                std::copy(m_returnedTs.begin(), m_returnedTs.end(), copy);
            }
            return TRUE;
        }
        catch (const std::exception& error) {
            Error(error);
            return FALSE;
        }
    }
    const BOOL BonDriverRelayClient::GetTsStream(BYTE* destination, DWORD* size, DWORD* remaining)
    {
        return ReadStream(destination, nullptr, size, remaining);
    }
    const BOOL BonDriverRelayClient::GetTsStream(BYTE** destination, DWORD* size, DWORD* remaining)
    {
        return ReadStream(nullptr, destination, size, remaining);
    }
    void BonDriverRelayClient::PurgeTsStream()
    {
        try {
            std::lock_guard lock(m_apiMutex);
            StopPump();
            m_queue.Reset();
            m_returnedTs.clear();
            if (m_open.load()) {
                const auto reply = m_remote->Call(protocol::Opcode::Purge);
                protocol::Reader(reply.body).End();
                StartPump();
            }
            else {
                m_queue.Close(protocol::Status::Unavailable);
            }
        }
        catch (const std::exception& error) {
            Error(error);
            m_open.store(false);
            m_queue.Close(protocol::Status::Unavailable);
        }
    }
    void BonDriverRelayClient::Release()
    {
        delete this;
    }
    LPCTSTR BonDriverRelayClient::GetTunerName()
    {
        return L"BonDriverRelay";
    }
    const BOOL BonDriverRelayClient::IsTunerOpening()
    {
        return m_open.load() ? TRUE : FALSE;
    }
    LPCTSTR BonDriverRelayClient::EnumTuningSpace(const DWORD space)
    {
        std::lock_guard lock(m_apiMutex);
        return space < m_spaces.size() ? m_spaces[space].c_str() : nullptr;
    }
    LPCTSTR BonDriverRelayClient::EnumChannelName(const DWORD space, const DWORD channel)
    {
        std::lock_guard lock(m_apiMutex);
        return space < m_channels.size() && channel < m_channels[space].size() ? m_channels[space][channel].c_str()
                                                                               : nullptr;
    }
    const DWORD BonDriverRelayClient::GetCurSpace()
    {
        return m_space.load();
    }
    const DWORD BonDriverRelayClient::GetCurChannel()
    {
        return m_channel.load();
    }
    RelayStatus BonDriverRelayClient::GetDiagnostics() const
    {
        RelayStatus status;
        status.lastError = m_lastError.load();
        status.decoderState = m_decoderState.load();
        status.readyBlocks = m_queue.GetReadyCount();
        status.receivedBytes = m_receivedBytes.load();
        status.droppedPackets = m_remoteDrops.load() + m_queue.GetDroppedPackets();
        const auto opens = m_reconnects.load();
        status.reconnects = opens == 0 ? 0 : opens - 1;
        return status;
    }
} // namespace bdr::client
