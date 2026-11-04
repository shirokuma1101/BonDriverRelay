#pragma once

#include "BonDriverRelay/bon_driver/BonDriverAdapter.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <vector>

namespace bdr::core
{
    enum class TunerState
    {
        Unloaded,
        Loaded,
        Open,
        Tuned,
        Streaming,
        Stopping,
        Closed,
        Failed
    };
    enum class ReceiveStatus
    {
        Data,
        Timeout,
        Cancelled
    };

    struct TsReadResult
    {
        ReceiveStatus status = ReceiveStatus::Timeout;
        std::vector<std::uint8_t> data;
    };

    class TunerSession
    {
      public:
        TunerSession() = default;
        ~TunerSession() = default;
        TunerSession(const TunerSession&) = delete;
        TunerSession& operator=(const TunerSession&) = delete;
        TunerSession(TunerSession&&) = delete;
        TunerSession& operator=(TunerSession&&) = delete;

        void Load(const std::filesystem::path& path);
        void Open();
        void Tune(const bon_driver::ChannelSelection& channel);
        [[nodiscard]] TsReadResult Receive(std::chrono::steady_clock::time_point deadline, std::stop_token stop = {});
        void Purge();
        void Close();
        [[nodiscard]] float GetSignalLevel();
        [[nodiscard]] bool HasExtendedInterface() const noexcept;
        [[nodiscard]] std::wstring GetTunerName();
        [[nodiscard]] std::optional<std::wstring> GetSpaceName(std::uint32_t space);
        [[nodiscard]] std::optional<std::wstring> GetChannelName(std::uint32_t space, std::uint32_t channel);
        [[nodiscard]] TunerState GetState() const noexcept;
        [[nodiscard]] std::optional<bon_driver::ChannelSelection> GetChannel() const noexcept;
        [[nodiscard]] std::uint64_t GetReceivedBytes() const noexcept;

      private:
        bon_driver::BonDriverAdapter m_driver;
        TunerState m_state = TunerState::Unloaded;
        std::optional<bon_driver::ChannelSelection> m_channel;
        std::vector<std::uint8_t> m_partialPacket;
        std::uint64_t m_receivedBytes = 0;
    };
} // namespace bdr::core
