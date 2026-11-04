#include "BonDriverRelay/core/Logger.h"
#include "BonDriverRelay/core/Error.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace bdr::core
{
    Logger::Logger(const LoggingConfig& config)
    {
        try {
            std::filesystem::create_directories(config.directory);
            auto console = std::make_shared<spdlog::sinks::stderr_sink_mt>();
            auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                (config.directory / L"BonDriverRelay.log").native(), config.rotateBytes, config.rotateFiles);
            m_logger = std::make_shared<spdlog::logger>("BonDriverRelay", spdlog::sinks_init_list{console, file});
            m_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
            m_logger->set_level(spdlog::level::from_str(config.level));
            m_logger->flush_on(spdlog::level::info);
            m_logger->set_error_handler(
                [](const std::string&) { throw RelayError(ErrorCode::LogFailure, "Log output failed"); });
        }
        catch (const std::exception&) {
            throw RelayError(ErrorCode::LogFailure, "Logger initialization failed");
        }
    }

    void Logger::Write(LogLevel level, std::string_view tuner, std::string_view operation, std::string_view message,
                       std::uint32_t native_code, std::string_view client)
    {
        spdlog::level::level_enum native_level = spdlog::level::info;
        switch (level) {
        case LogLevel::Debug:
            native_level = spdlog::level::debug;
            break;
        case LogLevel::Info:
            native_level = spdlog::level::info;
            break;
        case LogLevel::Warn:
            native_level = spdlog::level::warn;
            break;
        case LogLevel::Error:
            native_level = spdlog::level::err;
            break;
        }
        m_logger->log(native_level, "tuner={} client={} operation={} win32={} {}", tuner, client, operation,
                      native_code, message);
    }

    void Logger::Flush()
    {
        m_logger->flush();
    }
    void Logger::SetLevel(const std::string& level)
    {
        if (level != "debug" && level != "info" && level != "warn" && level != "error") {
            throw RelayError(ErrorCode::Configuration, "Invalid runtime log level");
        }
        m_logger->set_level(spdlog::level::from_str(level));
    }
} // namespace bdr::core
