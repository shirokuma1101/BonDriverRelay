#pragma once

#include "BonDriverRelay/core/Configuration.h"
#include <cstdint>
#include <memory>
#include <string_view>

namespace spdlog
{
    class logger;
}

namespace bdr::core
{
    enum class LogLevel
    {
        Debug,
        Info,
        Warn,
        Error
    };

    class Logger
    {
      public:
        explicit Logger(const LoggingConfig& config);
        void Write(LogLevel level, std::string_view tuner, std::string_view operation, std::string_view message,
                   std::uint32_t native_code = 0, std::string_view client = "-");
        void Flush();
        void SetLevel(const std::string& level);

      private:
        std::shared_ptr<spdlog::logger> m_logger;
    };
} // namespace bdr::core
