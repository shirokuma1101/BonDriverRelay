#include "BonDriverRelay/service/HostRuntime.h"
#include "BonDriverRelay/server/RelayServer.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/monitor/MonitorServer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>

namespace bdr::service
{
    namespace
    {
        [[noreturn]] void DeadlineExceeded(bool startup)
        {
            // Avoid logger/CRT/DLL locks on the watchdog path. OS closes process resources.
            const char* message = startup ? "ERROR host startup deadline exceeded; terminating process (1460)\r\n"
                                          : "ERROR host shutdown deadline exceeded; terminating process (1460)\r\n";
            OutputDebugStringA(message);
            TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
            // TerminateProcess on our own live process does not return on success.
            std::abort();
        }
    } // namespace
    SERVICE_STATUS MakeServiceStatus(const HostStatus& status)
    {
        SERVICE_STATUS native{};
        native.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        switch (status.state) {
        case HostState::StartPending:
            native.dwCurrentState = SERVICE_START_PENDING;
            break;
        case HostState::Running:
            native.dwCurrentState = SERVICE_RUNNING;
            break;
        case HostState::StopPending:
            native.dwCurrentState = SERVICE_STOP_PENDING;
            break;
        case HostState::Stopped:
            native.dwCurrentState = SERVICE_STOPPED;
            break;
        }
        native.dwControlsAccepted =
            status.state == HostState::Running ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
        native.dwCheckPoint = status.checkpoint;
        native.dwWaitHint = status.waitHintMs;
        native.dwWin32ExitCode = status.exitCode ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
        native.dwServiceSpecificExitCode = status.exitCode;
        return native;
    }
    void RunHost(const HostedWork& work, const std::function<bool()>& stop_requested, const StatusReporter& report,
                 std::uint32_t startup_timeout_ms, std::uint32_t shutdown_timeout_ms)
    {
        using Clock = std::chrono::steady_clock;
        std::atomic<bool> ready = false, done = false;
        std::mutex mutex;
        std::condition_variable changed;
        std::exception_ptr work_failure, host_failure;
        const auto started = Clock::now();
        HostStatus status{HostState::StartPending, 1, startup_timeout_ms, 0};
        if (report) {
            report(status);
        }
        std::jthread worker([&](std::stop_token stop) {
            try {
                work(stop, [&] {
                    ready.store(true);
                    changed.notify_all();
                });
            }
            catch (...) {
                work_failure = std::current_exception();
            }
            done.store(true);
            changed.notify_all();
        });
        auto next_report = started + std::chrono::seconds(1);
        auto stop_deadline = Clock::time_point::max();
        while (!done.load()) {
            const auto now = Clock::now();
            bool stopping = host_failure != nullptr;
            if (!stopping) {
                try {
                    stopping = stop_requested();
                }
                catch (...) {
                    host_failure = std::current_exception();
                    stopping = true;
                }
            }
            if (stopping && status.state != HostState::StopPending) {
                status = {HostState::StopPending, 1, shutdown_timeout_ms, 0};
                stop_deadline = now + std::chrono::milliseconds(shutdown_timeout_ms);
                worker.request_stop();
                next_report = now;
            }
            if (status.state == HostState::StartPending && ready.load()) {
                status = {HostState::Running, 0, 0, 0};
                next_report = now;
            }
            if (status.state == HostState::StopPending && now >= stop_deadline) {
                DeadlineExceeded(false);
            }
            if (status.state == HostState::StartPending &&
                now - started >= std::chrono::milliseconds(startup_timeout_ms)) {
                DeadlineExceeded(true);
            }
            if (now >= next_report) {
                // A heartbeat is not native progress. Keep the checkpoint fixed while waiting;
                // the independent deadline bounds this phase even if the DLL never returns.
                if (report && !host_failure) {
                    try {
                        report(status);
                    }
                    catch (...) {
                        host_failure = std::current_exception();
                    }
                }
                next_report = now + std::chrono::seconds(1);
            }
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(25), [&] { return done.load(); });
        }
        worker.join();
        if (report && !host_failure) {
            report({HostState::Stopped, 0, 0, work_failure ? 5u : 0u});
        }
        if (host_failure) {
            std::rethrow_exception(host_failure);
        }
        if (work_failure) {
            std::rethrow_exception(work_failure);
        }
    }
    void RunServerHost(const core::ServerConfig& config, core::Logger& logger,
                       const std::function<bool()>& stop_requested, const StatusReporter& report)
    {
        RunHost(
            [&](std::stop_token stop, const std::function<void()>& ready) {
                server::RelayServer server(config, &logger);
                server.Start();
                core::ConfigurationReloader reloader(config, logger);
                monitor::MonitorServer monitor(
                    config.monitoring, [&] { return server.GetSnapshot(); }, [&] { return reloader.GetSnapshot(); });
                monitor.Start();
                if (config.monitoring.enabled) {
                    logger.Write(core::LogLevel::Info, "-", "monitor_listen",
                                 "HTTP " + config.monitoring.bindAddress + ":" + std::to_string(monitor.GetPort()));
                }
                logger.Write(core::LogLevel::Info, "-", "listen", "TCP port=" + std::to_string(server.GetPort()));
                ready();
                std::mutex mutex;
                std::condition_variable_any changed;
                auto next_status = std::chrono::steady_clock::now();
                auto status_interval = config.logging.statusIntervalMs;
                while (!stop.stop_requested()) {
                    reloader.Poll();
                    const auto interval = reloader.GetSnapshot().statusIntervalMs;
                    if (interval != status_interval) {
                        status_interval = interval;
                        next_status = std::chrono::steady_clock::now();
                    }
                    if (std::chrono::steady_clock::now() >= next_status) {
                        server.LogDiagnostics();
                        next_status = std::chrono::steady_clock::now() + std::chrono::milliseconds(status_interval);
                    }
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, stop, std::chrono::milliseconds(100), [] { return false; });
                }
                logger.Write(core::LogLevel::Info, "-", "stop_begin",
                             "Stopping accept, connections, streams and native owners");
                monitor.Stop();
                server.Stop();
                server.LogDiagnostics();
                const auto final = server.GetSnapshot();
                if (std::any_of(final.manager.tuners.begin(), final.manager.tuners.end(),
                                [](const auto& tuner) { return tuner.cleanupFailures != 0; })) {
                    throw core::RelayError(core::ErrorCode::DriverFailure,
                                           "Native cleanup failed; inspect tuner diagnostics");
                }
                logger.Write(core::LogLevel::Info, "-", "stop",
                             "Server workers stopped; cleanup failures, if any, are logged separately");
                logger.Flush();
            },
            stop_requested, report, config.network.startupTimeoutMs, config.network.shutdownTimeoutMs);
    }
} // namespace bdr::service
