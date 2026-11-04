#include "BonDriverRelay/core/ConfigurationReloader.h"
#include "BonDriverRelay/core/Error.h"
#include <Windows.h>
namespace bdr::core
{
    ConfigurationReloader::ConfigurationReloader(const ServerConfig& config, Logger& logger)
        : m_active(config), m_logger(logger)
    {
        m_snapshot.enabled = config.reload.enabled;
        m_snapshot.level = config.logging.level;
        m_snapshot.statusIntervalMs = config.logging.statusIntervalMs;
    }
    ReloadSnapshot ConfigurationReloader::GetSnapshot() const
    {
        std::lock_guard lock(m_mutex);
        return m_snapshot;
    }
    bool ConfigurationReloader::ReloadNow()
    {
        try {
            auto candidate = LoadServerConfig(m_active.configPath);
            if (candidate == m_active) {
                std::lock_guard lock(m_mutex);
                m_snapshot.lastError.clear();
                return true;
            }
            auto comparable = candidate;
            comparable.logging.level = m_active.logging.level;
            comparable.logging.statusIntervalMs = m_active.logging.statusIntervalMs;
            if (comparable != m_active) {
                throw RelayError(
                    ErrorCode::Configuration,
                    "Reload permits only logging.level and logging.status_interval_ms; other changes require restart");
            }
            m_logger.SetLevel(candidate.logging.level);
            m_active = std::move(candidate);
            {
                std::lock_guard lock(m_mutex);
                ++m_snapshot.applied;
                m_snapshot.level = m_active.logging.level;
                m_snapshot.statusIntervalMs = m_active.logging.statusIntervalMs;
                m_snapshot.lastError.clear();
            }
            try {
                m_logger.Write(LogLevel::Info, "-", "reload", "Runtime log settings applied");
            }
            catch (...) {
                OutputDebugStringW(L"BonDriverRelay: runtime log settings applied; diagnostic failed.\n");
            }
            return true;
        }
        catch (const std::exception& error) {
            {
                std::lock_guard lock(m_mutex);
                ++m_snapshot.rejected;
                m_snapshot.lastError = error.what();
            }
            try {
                m_logger.Write(LogLevel::Warn, "-", "reload_rejected", error.what());
            }
            catch (...) {
                OutputDebugStringW(L"BonDriverRelay: reload rejection log failed.\n");
            }
            return false;
        }
    }
    void ConfigurationReloader::Poll()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!m_active.reload.enabled || now < m_nextPoll) {
            return;
        }
        m_nextPoll = now + std::chrono::milliseconds(m_active.reload.intervalMs);
        std::error_code error;
        const auto time = std::filesystem::last_write_time(m_active.configPath, error);
        if (error) {
            if (!m_fileError) {
                m_fileError = true;
                (void)ReloadNow();
            }
            return;
        }
        const auto size = std::filesystem::file_size(m_active.configPath, error);
        if (error) {
            if (!m_fileError) {
                m_fileError = true;
                (void)ReloadNow();
            }
            return;
        }
        if (m_fileError) {
            m_stamp.reset();
            m_fileError = false;
        }
        const auto stamp = std::make_pair(time, size);
        if (!m_stamp || *m_stamp != stamp) {
            m_stamp = stamp;
            (void)ReloadNow();
        }
    }
} // namespace bdr::core
