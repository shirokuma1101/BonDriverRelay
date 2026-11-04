#pragma once
#include <Windows.h>
#include "BonDriverRelay/core/Configuration.h"
#include <cstdint>
#include <span>
#include <vector>

class IB25Decoder2;
namespace bdr::decoder
{
    enum class DecoderState : std::uint32_t
    {
        Disabled = 0,
        Unavailable = 1,
        InitializationFailed = 2,
        ActiveUnverified = 3,
        DecodeFailed = 4,
        ScrambledOutput = 5
    };
    class B25DecoderAdapter
    {
      public:
        B25DecoderAdapter() = default;
        ~B25DecoderAdapter();
        B25DecoderAdapter(const B25DecoderAdapter&) = delete;
        B25DecoderAdapter& operator=(const B25DecoderAdapter&) = delete;
        void Open(const core::DecoderConfig& config);
        void Reset();
        std::vector<std::uint8_t> Decode(std::span<const std::uint8_t> source);
        std::vector<std::uint8_t> Flush();
        void Close();
        DecoderState GetState() const noexcept;
        std::uint64_t GetFailureCount() const noexcept;

      private:
        std::vector<std::uint8_t> CopyOutput(BYTE* data, DWORD length);
        bool Cleanup() noexcept;
        HMODULE m_module = nullptr;
        IB25Decoder2* m_decoder = nullptr;
        DecoderState m_state = DecoderState::Disabled;
        bool m_passthrough = false;
        bool m_poisoned = false;
        std::uint64_t m_failures = 0;
    };
} // namespace bdr::decoder
