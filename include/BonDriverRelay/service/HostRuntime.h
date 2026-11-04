#pragma once
#include "BonDriverRelay/core/Configuration.h"
#include "BonDriverRelay/core/Logger.h"
#include <functional>
#include <stop_token>
#include <Windows.h>

namespace bdr::service
{
    enum class HostState
    {
        StartPending,
        Running,
        StopPending,
        Stopped
    };
    struct HostStatus
    {
        HostState state = HostState::StartPending;
        DWORD checkpoint = 0;
        DWORD waitHintMs = 0;
        DWORD exitCode = 0;
    };
    using StatusReporter = std::function<void(const HostStatus&)>;
    using HostedWork = std::function<void(std::stop_token, const std::function<void()>&)>;

    // Own-process watchdog: a stuck native DLL cannot be safely interrupted or unloaded.
    // Deadlines terminate the entire host with ERROR_TIMEOUT, never a single native thread.
    void RunHost(const HostedWork& work, const std::function<bool()>& stop_requested, const StatusReporter& report,
                 std::uint32_t startup_timeout_ms, std::uint32_t shutdown_timeout_ms);
    void RunServerHost(const core::ServerConfig& config, core::Logger& logger,
                       const std::function<bool()>& stop_requested, const StatusReporter& report = {});
    SERVICE_STATUS MakeServiceStatus(const HostStatus& status);
} // namespace bdr::service
