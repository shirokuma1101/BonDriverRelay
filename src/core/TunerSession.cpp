#include "BonDriverRelay/core/TunerSession.h"
#include "BonDriverRelay/core/Error.h"

#include <algorithm>
#include <thread>

namespace bdr::core
{
    void TunerSession::Load(const std::filesystem::path& path)
    {
        if (m_state != TunerState::Unloaded && m_state != TunerState::Closed) {
            throw RelayError(ErrorCode::InvalidState, "Session is already loaded");
        }
        m_driver.Load(path);
        m_state = TunerState::Loaded;
        m_receivedBytes = 0;
    }

    void TunerSession::Open()
    {
        if (m_state != TunerState::Loaded) {
            throw RelayError(ErrorCode::InvalidState, "Session must be loaded before Open");
        }
        try {
            m_driver.Open();
            m_state = TunerState::Open;
        }
        catch (...) {
            m_state = TunerState::Failed;
            throw;
        }
    }

    void TunerSession::Tune(const bon_driver::ChannelSelection& channel)
    {
        if (m_state != TunerState::Open && m_state != TunerState::Tuned && m_state != TunerState::Streaming) {
            throw RelayError(ErrorCode::InvalidState, "Session must be open before Tune");
        }
        m_channel.reset();
        m_partialPacket.clear();
        m_state = TunerState::Open;
        m_driver.Tune(channel);
        m_channel = channel;
        m_state = TunerState::Tuned;
    }

    TsReadResult TunerSession::Receive(std::chrono::steady_clock::time_point deadline, std::stop_token stop)
    {
        if (m_state != TunerState::Tuned && m_state != TunerState::Streaming) {
            throw RelayError(ErrorCode::InvalidState, "Session must be tuned before Receive");
        }
        try {
            for (;;) {
                if (stop.stop_requested()) {
                    return {ReceiveStatus::Cancelled, {}};
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    return {ReceiveStatus::Timeout, {}};
                }
                auto data = m_driver.ReadTs();
                if (!data.empty()) {
                    if (!m_partialPacket.empty()) {
                        data.insert(data.begin(), m_partialPacket.begin(), m_partialPacket.end());
                        m_partialPacket.clear();
                    }
                    const auto complete = data.size() / 188 * 188;
                    for (std::size_t offset = 0; offset < complete; offset += 188) {
                        if (data[offset] != 0x47) {
                            throw RelayError(ErrorCode::InvalidStream,
                                             "TS sync byte is invalid; stream is not silently resynchronized");
                        }
                    }
                    m_partialPacket.assign(data.begin() + static_cast<std::ptrdiff_t>(complete), data.end());
                    data.resize(complete);
                    if (!data.empty()) {
                        m_receivedBytes += data.size();
                        m_state = TunerState::Streaming;
                        return {ReceiveStatus::Data, std::move(data)};
                    }
                }
                std::this_thread::sleep_until(
                    std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
            }
        }
        catch (...) {
            m_state = TunerState::Failed;
            m_channel.reset();
            m_partialPacket.clear();
            throw;
        }
    }

    void TunerSession::Purge()
    {
        if (m_state != TunerState::Open && m_state != TunerState::Tuned && m_state != TunerState::Streaming) {
            throw RelayError(ErrorCode::InvalidState, "Session must be open before Purge");
        }
        m_driver.Purge();
        m_partialPacket.clear();
        if (m_channel) {
            m_state = TunerState::Tuned;
        }
    }

    void TunerSession::Close()
    {
        if (m_state == TunerState::Closed) {
            return;
        }
        m_state = TunerState::Stopping;
        m_channel.reset();
        const auto trailing_bytes = m_partialPacket.size();
        m_partialPacket.clear();
        try {
            m_driver.Unload();
            m_state = TunerState::Closed;
        }
        catch (...) {
            m_state = TunerState::Failed;
            throw;
        }
        if (trailing_bytes != 0) {
            throw RelayError(ErrorCode::InvalidStream, "Stream ended with an incomplete TS packet (" +
                                                           std::to_string(trailing_bytes) + " bytes)");
        }
    }

    float TunerSession::GetSignalLevel()
    {
        return m_driver.GetSignalLevel();
    }
    bool TunerSession::HasExtendedInterface() const noexcept
    {
        return m_driver.HasExtendedInterface();
    }
    std::wstring TunerSession::GetTunerName()
    {
        return m_driver.GetTunerName();
    }
    std::optional<std::wstring> TunerSession::GetSpaceName(std::uint32_t space)
    {
        return m_driver.GetSpaceName(space);
    }
    std::optional<std::wstring> TunerSession::GetChannelName(std::uint32_t space, std::uint32_t channel)
    {
        return m_driver.GetChannelName(space, channel);
    }
    TunerState TunerSession::GetState() const noexcept
    {
        return m_state;
    }
    std::optional<bon_driver::ChannelSelection> TunerSession::GetChannel() const noexcept
    {
        return m_channel;
    }
    std::uint64_t TunerSession::GetReceivedBytes() const noexcept
    {
        return m_receivedBytes;
    }
} // namespace bdr::core
