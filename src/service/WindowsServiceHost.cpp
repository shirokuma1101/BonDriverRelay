#include "BonDriverRelay/service/WindowsServiceHost.h"
#include "BonDriverRelay/service/HostRuntime.h"
#include "BonDriverRelay/core/Error.h"
#include <atomic>
#include <iostream>
#include <mutex>

namespace bdr::service
{
    namespace
    {
        constexpr wchar_t SERVICE_NAME[] = L"BonDriverRelay";
        class WindowsServiceHost
        {
          public:
            explicit WindowsServiceHost(const std::filesystem::path& path) : m_configPath(path) {}
            int Run();
            static void WINAPI ServiceMain(DWORD, LPWSTR*);

          private:
            static DWORD WINAPI Control(DWORD control, DWORD, LPVOID, LPVOID context);
            void Main() noexcept;
            void Report(const HostStatus& status);
            std::filesystem::path m_configPath;
            std::atomic<bool> m_stopRequested = false;
            SERVICE_STATUS_HANDLE m_handle = nullptr;
            std::mutex m_statusMutex;
            SERVICE_STATUS m_status{};
            int m_exitCode = 0;
        };
        WindowsServiceHost* active_host = nullptr;
        int WindowsServiceHost::Run()
        {
            active_host = this;
            SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(SERVICE_NAME), ServiceMain}, {nullptr, nullptr}};
            const BOOL dispatched = StartServiceCtrlDispatcherW(table);
            const auto error = dispatched ? ERROR_SUCCESS : GetLastError();
            active_host = nullptr;
            if (!dispatched) {
                throw core::RelayError(core::ErrorCode::InvalidState,
                                       "SCM dispatcher failed; start --service through SCM", error);
            }
            return m_exitCode;
        }
        void WINAPI WindowsServiceHost::ServiceMain(DWORD, LPWSTR*)
        {
            active_host->Main();
        }
        void WindowsServiceHost::Report(const HostStatus& status)
        {
            std::lock_guard lock(m_statusMutex);
            if (m_status.dwCurrentState == SERVICE_STOPPED) {
                return; // STOPPED closes SCM's RPC context; never publish it twice.
            }
            m_status = MakeServiceStatus(status);
            if (!SetServiceStatus(m_handle, &m_status)) {
                throw core::RelayError(core::ErrorCode::InvalidState, "SetServiceStatus failed", GetLastError());
            }
        }
        DWORD WINAPI WindowsServiceHost::Control(DWORD control, DWORD, LPVOID, LPVOID context)
        {
            auto& host = *static_cast<WindowsServiceHost*>(context);
            if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
                host.m_stopRequested.store(true);
                return NO_ERROR;
            }
            if (control == SERVICE_CONTROL_INTERROGATE) {
                std::lock_guard lock(host.m_statusMutex);
                if (host.m_status.dwCurrentState == SERVICE_STOPPED) {
                    return NO_ERROR;
                }
                return SetServiceStatus(host.m_handle, &host.m_status) ? NO_ERROR : GetLastError();
            }
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
        void WindowsServiceHost::Main() noexcept
        {
            std::unique_ptr<core::Logger> logger;
            m_status = MakeServiceStatus({HostState::StartPending, 0, 10000, 0});
            m_handle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, Control, this);
            if (!m_handle) {
                m_exitCode = 5;
                return;
            }
            try {
                Report({HostState::StartPending, 1, 10000, 0});
                const auto config = core::LoadServerConfig(m_configPath);
                logger = std::make_unique<core::Logger>(config.logging);
                RunServerHost(
                    config, *logger, [&] { return m_stopRequested.load(); },
                    [&](const HostStatus& status) {
                        // Finish logger cleanup before the one final SCM notification.
                        if (status.state == HostState::Stopped) {
                            return;
                        }
                        auto current = status;
                        // Configuration and logging initialization completed: one real startup milestone.
                        if (current.state == HostState::StartPending) {
                            ++current.checkpoint;
                        }
                        Report(current);
                    });
                logger.reset();
                Report({HostState::Stopped, 0, 0, 0});
            }
            catch (const std::exception& error) {
                m_exitCode = 5;
                const auto* relay_error = dynamic_cast<const core::RelayError*>(&error);
                const auto native_code = relay_error ? relay_error->GetNativeCode() : 0;
                try {
                    if (logger) {
                        logger->Write(core::LogLevel::Error, "-", "service_failure", error.what(), native_code);
                        logger->Flush();
                    }
                    else {
                        std::cerr << "ERROR service_failure: " << error.what() << " win32=" << native_code << '\n';
                    }
                }
                catch (...) {
                    OutputDebugStringW(L"BonDriverRelay: service failure diagnostic unavailable.\n");
                }
                logger.reset();
                try {
                    bool stopped;
                    {
                        std::lock_guard lock(m_statusMutex);
                        stopped = m_status.dwCurrentState == SERVICE_STOPPED;
                    }
                    if (!stopped) {
                        Report({HostState::Stopped, 0, 0, 5});
                    }
                }
                catch (...) {
                    OutputDebugStringW(L"BonDriverRelay: final SCM status unavailable.\n");
                }
            }
        }
    } // namespace
    int RunWindowsService(const std::filesystem::path& config_path)
    {
        WindowsServiceHost host(config_path);
        return host.Run();
    }
} // namespace bdr::service
