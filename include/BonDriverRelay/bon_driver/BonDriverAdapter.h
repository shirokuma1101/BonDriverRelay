#pragma once

#include "BonDriverRelay/bon_driver/BonDriverAbi.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bdr::bon_driver
{
    inline constexpr std::size_t MAX_TS_BLOCK_BYTES = 188 * 5'577;

    struct ChannelSelection
    {
        std::uint32_t space = 0;
        std::uint32_t channel = 0;
        bool legacy = false;
        bool operator==(const ChannelSelection&) const = default;
    };

    class BonDriverAdapter
    {
      public:
        BonDriverAdapter() = default;
        ~BonDriverAdapter();
        BonDriverAdapter(const BonDriverAdapter&) = delete;
        BonDriverAdapter& operator=(const BonDriverAdapter&) = delete;
        BonDriverAdapter(BonDriverAdapter&&) = delete;
        BonDriverAdapter& operator=(BonDriverAdapter&&) = delete;

        void Load(const std::filesystem::path& path);
        void Open();
        void Tune(const ChannelSelection& channel);
        void Purge();
        // Copies borrowed data before the next driver call. FALSE with no data is
        // an empty poll, not proof of a permanent error in the BonDriver ABI.
        [[nodiscard]] std::vector<std::uint8_t> ReadTs();
        [[nodiscard]] float GetSignalLevel();
        [[nodiscard]] bool HasExtendedInterface() const noexcept;
        [[nodiscard]] std::wstring GetTunerName();
        [[nodiscard]] std::optional<std::wstring> GetSpaceName(std::uint32_t space);
        [[nodiscard]] std::optional<std::wstring> GetChannelName(std::uint32_t space, std::uint32_t channel);
        void Unload();

      private:
        void RequireOwner() const;
        void RequireOpen() const;
        // On a foreign Release exception, intentionally keep the module loaded:
        // unloading code belonging to a possibly live object is unsafe.
        [[nodiscard]] bool Cleanup() noexcept;

        HMODULE m_module = nullptr;
        IBonDriver* m_driver = nullptr;
        IBonDriver2* m_driver2 = nullptr;
        bool m_isOpen = false;
        bool m_cleanupFailed = false;
        std::thread::id m_owner;
    };
} // namespace bdr::bon_driver
