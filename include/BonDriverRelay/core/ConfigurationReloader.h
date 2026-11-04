#pragma once
#include "BonDriverRelay/core/Logger.h"
#include <chrono>
#include <mutex>
#include <optional>
namespace bdr::core
{
    struct ReloadSnapshot
    {
        bool enabled = false;
        std::uint64_t applied = 0, rejected = 0;
        std::uint32_t statusIntervalMs = 30000;
        std::string level = "info", lastError;
    };
    class ConfigurationReloader
    {
      public:
        ConfigurationReloader(const ServerConfig& config, Logger& logger);
        void Poll();
        bool ReloadNow();
        ReloadSnapshot GetSnapshot() const;

      private:
        ServerConfig m_active;
        Logger& m_logger;
        mutable std::mutex m_mutex;
        ReloadSnapshot m_snapshot;
        std::optional<std::pair<std::filesystem::file_time_type, std::uintmax_t>> m_stamp;
        std::chrono::steady_clock::time_point m_nextPoll{};
        bool m_fileError = false;
    };
} // namespace bdr::core
