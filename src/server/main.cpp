#include "BonDriverRelay/Version.h"
#include "BonDriverRelay/server/RelayServer.h"
#include "BonDriverRelay/core/Configuration.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/core/Logger.h"
#include "BonDriverRelay/core/Paths.h"
#include "BonDriverRelay/core/TunerSession.h"
#include "BonDriverRelay/service/HostRuntime.h"
#include "BonDriverRelay/service/WindowsServiceHost.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace
{
    using bdr::core::ErrorCode;
    using bdr::core::LogLevel;
    using bdr::core::RelayError;

    std::atomic<bool> stop_requested = false;

    BOOL WINAPI ConsoleHandler(DWORD event)
    {
        if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
            event == CTRL_SHUTDOWN_EVENT) {
            stop_requested.store(true, std::memory_order_relaxed);
            return TRUE;
        }
        return FALSE;
    }

    class ConsoleControl
    {
      public:
        ConsoleControl()
        {
            stop_requested.store(false, std::memory_order_relaxed);
            if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
                throw RelayError(ErrorCode::FileFailure, "Cannot register console handler", GetLastError());
            }
        }
        ~ConsoleControl()
        {
            SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        }
        ConsoleControl(const ConsoleControl&) = delete;
        ConsoleControl& operator=(const ConsoleControl&) = delete;
    };

    class TsFileWriter
    {
      public:
        explicit TsFileWriter(const std::filesystem::path& path)
        {
            if (!path.is_absolute()) {
                throw RelayError(ErrorCode::InvalidArgument, "--output must be absolute");
            }
            m_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_file == INVALID_HANDLE_VALUE) {
                throw RelayError(ErrorCode::FileFailure, "Cannot create output (existing files are never overwritten)",
                                 GetLastError());
            }
        }
        ~TsFileWriter()
        {
            if (m_file != INVALID_HANDLE_VALUE) {
                CloseHandle(m_file);
            }
        }
        TsFileWriter(const TsFileWriter&) = delete;
        TsFileWriter& operator=(const TsFileWriter&) = delete;

        void Write(std::span<const std::uint8_t> data)
        {
            while (!data.empty()) {
                DWORD written = 0;
                const DWORD count = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1'048'576));
                if (!WriteFile(m_file, data.data(), count, &written, nullptr)) {
                    throw RelayError(ErrorCode::FileFailure, "TS write failed", GetLastError());
                }
                if (written == 0) {
                    throw RelayError(ErrorCode::FileFailure, "TS write made no progress", ERROR_WRITE_FAULT);
                }
                data = data.subspan(written);
            }
        }

        void Close()
        {
            if (!FlushFileBuffers(m_file)) {
                throw RelayError(ErrorCode::FileFailure, "TS flush failed", GetLastError());
            }
            const auto file = std::exchange(m_file, INVALID_HANDLE_VALUE);
            if (!CloseHandle(file)) {
                throw RelayError(ErrorCode::FileFailure, "TS close failed", GetLastError());
            }
        }

      private:
        HANDLE m_file = INVALID_HANDLE_VALUE;
    };

    enum class Mode
    {
        Help,
        Version,
        Validate,
        Console,
        Serve,
        Service
    };
    struct Options
    {
        Mode mode = Mode::Help;
        std::filesystem::path configPath;
        std::filesystem::path outputPath;
        std::string tunerName;
        std::optional<std::uint32_t> space;
        std::optional<std::uint32_t> channel;
        std::optional<std::uint32_t> legacyChannel;
        std::uint32_t durationMs = 5000;
        bool listChannels = false;
    };

    std::uint32_t ParseNumber(std::wstring_view value, std::uint32_t maximum)
    {
        const auto text = bdr::core::ToUtf8(value);
        std::uint32_t number = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
        if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || number > maximum) {
            throw RelayError(ErrorCode::InvalidArgument, "Invalid numeric option");
        }
        return number;
    }

    Options ParseOptions(int argc, wchar_t* argv[])
    {
        if (argc < 2) {
            throw RelayError(ErrorCode::InvalidArgument, "Use --help or select an explicit mode");
        }
        Options options;
        const std::wstring_view mode = argv[1];
        if (mode == L"--help") {
            options.mode = Mode::Help;
        }
        else if (mode == L"--version") {
            options.mode = Mode::Version;
        }
        else if (mode == L"--validate-config") {
            options.mode = Mode::Validate;
        }
        else if (mode == L"--console") {
            options.mode = Mode::Console;
        }
        else if (mode == L"--service") {
            options.mode = Mode::Service;
        }
        else if (mode == L"--serve") {
            options.mode = Mode::Serve;
            options.durationMs = 0;
        }
        else {
            throw RelayError(ErrorCode::InvalidArgument, "Unknown mode; use --help");
        }
        if (options.mode == Mode::Help || options.mode == Mode::Version) {
            if (argc != 2) {
                throw RelayError(ErrorCode::InvalidArgument, "This mode takes no options");
            }
            return options;
        }
        std::set<std::wstring_view> seen;
        for (int index = 2; index < argc; ++index) {
            const std::wstring_view key = argv[index];
            if (!seen.insert(key).second) {
                throw RelayError(ErrorCode::InvalidArgument, "Duplicate option");
            }
            if (key == L"--list-channels" && options.mode == Mode::Console) {
                options.listChannels = true;
                continue;
            }
            if (key != L"--config" && options.mode != Mode::Console &&
                !(options.mode == Mode::Serve && key == L"--duration-ms")) {
                throw RelayError(ErrorCode::InvalidArgument, "This mode accepts only --config");
            }
            if (key != L"--config" && key != L"--tuner" && key != L"--space" && key != L"--channel" &&
                key != L"--legacy-channel" && key != L"--duration-ms" && key != L"--output") {
                throw RelayError(ErrorCode::InvalidArgument, "Unknown option");
            }
            if (++index == argc) {
                throw RelayError(ErrorCode::InvalidArgument, "Option value is missing");
            }
            const std::wstring_view value = argv[index];
            if (value.empty()) {
                throw RelayError(ErrorCode::InvalidArgument, "Option value must not be empty");
            }
            if (key == L"--config") {
                options.configPath = value;
            }
            else if (key == L"--tuner") {
                options.tunerName = bdr::core::ToUtf8(value);
            }
            else if (key == L"--space") {
                options.space = ParseNumber(value, UINT32_MAX);
            }
            else if (key == L"--channel") {
                options.channel = ParseNumber(value, UINT32_MAX);
            }
            else if (key == L"--legacy-channel") {
                options.legacyChannel = ParseNumber(value, 255);
            }
            else if (key == L"--duration-ms") {
                options.durationMs = ParseNumber(value, 3'600'000);
                if (options.durationMs == 0) {
                    throw RelayError(ErrorCode::InvalidArgument, "Duration must be positive");
                }
            }
            else if (key == L"--output") {
                options.outputPath = value;
            }
        }
        if (options.configPath.empty()) {
            options.configPath = bdr::core::GetExecutablePath().parent_path() / L"server.toml";
        }
        if (!options.configPath.is_absolute()) {
            throw RelayError(ErrorCode::InvalidArgument, "--config must be absolute");
        }
        if (options.mode == Mode::Console) {
            if (options.listChannels) {
                if (options.space || options.channel || options.legacyChannel || !options.outputPath.empty() ||
                    seen.contains(L"--duration-ms")) {
                    throw RelayError(ErrorCode::InvalidArgument,
                                     "Channel listing cannot be combined with capture options");
                }
            }
            else if (options.legacyChannel ? (options.space || options.channel)
                                           : (!options.space || !options.channel)) {
                throw RelayError(ErrorCode::InvalidArgument, "Specify --space and --channel, or --legacy-channel");
            }
            if (!options.outputPath.empty() && !options.outputPath.is_absolute()) {
                throw RelayError(ErrorCode::InvalidArgument, "--output must be absolute");
            }
        }
        return options;
    }

    void ListChannels(bdr::core::TunerSession& session)
    {
        for (std::uint32_t space = 0; space < 64; ++space) {
            if (stop_requested.load(std::memory_order_relaxed)) {
                return;
            }
            const auto space_name = session.GetSpaceName(space);
            if (!space_name) {
                return;
            }
            std::cout << "space=" << space << " name=" << bdr::core::ToUtf8(*space_name) << '\n';
            bool ended = false;
            for (std::uint32_t channel = 0; channel < 4096; ++channel) {
                if (stop_requested.load(std::memory_order_relaxed)) {
                    return;
                }
                const auto name = session.GetChannelName(space, channel);
                if (!name) {
                    ended = true;
                    break;
                }
                std::cout << "channel=" << channel << " name=" << bdr::core::ToUtf8(*name) << '\n';
            }
            if (!ended) {
                throw RelayError(ErrorCode::DriverFailure, "Channel enumeration exceeds 4096 entries");
            }
        }
        throw RelayError(ErrorCode::DriverFailure, "Tuning space enumeration exceeds 64 entries");
    }

    int RunConsole(const Options& options, const bdr::core::ServerConfig& config, bdr::core::Logger& logger)
    {
        const auto& tuner = bdr::core::SelectTuner(config, options.tunerName);
        ConsoleControl control;
        bdr::core::TunerSession session;
        std::unique_ptr<TsFileWriter> output;
        if (!options.outputPath.empty()) {
            output = std::make_unique<TsFileWriter>(options.outputPath);
        }
        logger.Write(LogLevel::Info, tuner.name, "load", "Loading " + bdr::core::PathToUtf8(tuner.dllPath));
        session.Load(tuner.dllPath);
        session.Open();
        logger.Write(LogLevel::Info, tuner.name, "open", "Tuner opened; network=disabled");
        if (options.listChannels) {
            ListChannels(session);
            session.Close();
            logger.Write(LogLevel::Info, tuner.name, "close", "Tuner released and DLL unloaded");
            return stop_requested.load(std::memory_order_relaxed) ? 130 : 0;
        }
        const bdr::bon_driver::ChannelSelection channel{options.space.value_or(0),
                                                        options.legacyChannel.value_or(options.channel.value_or(0)),
                                                        options.legacyChannel.has_value()};
        session.Tune(channel);
        logger.Write(LogLevel::Info, tuner.name, "tune",
                     "space=" + std::to_string(channel.space) + " channel=" + std::to_string(channel.channel) +
                         " legacy=" + std::to_string(channel.legacy));
        bdr::decoder::B25DecoderAdapter decoder;
        decoder.Open(config.decoder);
        logger.Write(LogLevel::Info, tuner.name, "receive",
                     "decoder_state=" + std::to_string(static_cast<unsigned>(decoder.GetState())));
        std::uint64_t output_bytes = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.durationMs);
        while (!stop_requested.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
            auto result =
                session.Receive(std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
            if (result.status == bdr::core::ReceiveStatus::Data) {
                const auto decoded = decoder.Decode(result.data);
                output_bytes += decoded.size();
                if (output && !decoded.empty()) {
                    output->Write(decoded);
                }
            }
        }
        const auto received = session.GetReceivedBytes();
        const float signal = session.GetSignalLevel();
        const auto tail = decoder.Flush();
        output_bytes += tail.size();
        if (output && !tail.empty()) {
            output->Write(tail);
        }
        const auto decoder_state = decoder.GetState();
        decoder.Close();
        session.Close();
        if (output) {
            output->Close();
        }
        logger.Write(LogLevel::Info, tuner.name, "close",
                     "Tuner released and DLL unloaded; received_bytes=" + std::to_string(received) +
                         " signal=" + std::to_string(signal));
        std::cout << "received_bytes=" << received << " output_bytes=" << output_bytes
                  << " decoder_state=" << static_cast<unsigned>(decoder_state) << '\n';
        logger.Flush();
        if (stop_requested.load(std::memory_order_relaxed)) {
            return 130;
        }
        if (received == 0) {
            throw RelayError(ErrorCode::ReceiveTimeout, "No complete TS packet received before deadline");
        }
        return 0;
    }

    int GetExitCode(ErrorCode code)
    {
        switch (code) {
        case ErrorCode::InvalidArgument:
            return 2;
        case ErrorCode::NotImplemented:
            return 3;
        case ErrorCode::Configuration:
        case ErrorCode::InvalidDll:
        case ErrorCode::ArchitectureMismatch:
            return 4;
        case ErrorCode::FileFailure:
        case ErrorCode::LogFailure:
            return 6;
        case ErrorCode::ReceiveTimeout:
            return 7;
        default:
            return 5;
        }
    }
} // namespace

int wmain(int argc, wchar_t* argv[])
{
    std::unique_ptr<bdr::core::Logger> logger;
    std::string tuner_name = "-";
    try {
        const auto options = ParseOptions(argc, argv);
        tuner_name = options.tunerName.empty() ? "-" : options.tunerName;
        if (options.mode == Mode::Version) {
            std::cout << "BonDriverRelay " BDR_VERSION "\n";
            return 0;
        }
        if (options.mode == Mode::Help) {
            std::cout << "BonDriverRelayServer --version | --help\n"
                         "--validate-config [--config ABSOLUTE_TOML]\n"
                         "--console [--config ABSOLUTE_TOML] [--tuner ID] --space N --channel N\n"
                         "  [--duration-ms 1..3600000] [--output NEW_ABSOLUTE_TS]\n"
                         "  Use --legacy-channel 0..255 instead of --space/--channel for IBonDriver v1.\n"
                         "--console [--config ABSOLUTE_TOML] [--tuner ID] --list-channels\n"
                         "--serve [--config ABSOLUTE_TOML] [--duration-ms N] starts the TCP server.\n"
                         "--service [--config ABSOLUTE_TOML] runs through SCM as BonDriverRelay.\n"
                         "B25 requires an external decoder DLL and card environment.\n";
            return 0;
        }
        if (options.mode == Mode::Service) {
            return bdr::service::RunWindowsService(options.configPath);
        }
        const auto config = bdr::core::LoadServerConfig(options.configPath);
        if (options.mode == Mode::Validate) {
            std::cout << "Configuration valid; tuners=" << config.tuners.size()
                      << "; DLLs were not executed; decoder_configured=" << config.decoder.enabled << '\n';
            return 0;
        }
        if (options.mode == Mode::Serve) {
            logger = std::make_unique<bdr::core::Logger>(config.logging);
            ConsoleControl control;
            std::optional<std::chrono::steady_clock::time_point> running_since;
            bdr::service::RunServerHost(
                config, *logger,
                [&] {
                    return stop_requested.load() || (running_since && options.durationMs != 0 &&
                                                     std::chrono::steady_clock::now() - *running_since >=
                                                         std::chrono::milliseconds(options.durationMs));
                },
                [&](const bdr::service::HostStatus& status) {
                    if (status.state == bdr::service::HostState::Running && !running_since) {
                        running_since = std::chrono::steady_clock::now();
                    }
                });
            return 0;
        }
        tuner_name = bdr::core::SelectTuner(config, options.tunerName).name;
        logger = std::make_unique<bdr::core::Logger>(config.logging);
        return RunConsole(options, config, *logger);
    }
    catch (const RelayError& error) {
        const auto message = std::string(bdr::core::GetErrorCodeName(error.GetCode())) + ": " + error.what();
        if (logger) {
            try {
                logger->Write(LogLevel::Error, tuner_name, "failure", message, error.GetNativeCode());
            }
            catch (...) {
                std::cerr << "ERROR " << message << " win32=" << error.GetNativeCode() << '\n';
            }
        }
        else {
            std::cerr << "ERROR " << message << " win32=" << error.GetNativeCode() << '\n';
        }
        return GetExitCode(error.GetCode());
    }
    catch (const std::exception& error) {
        std::cerr << "ERROR Unexpected: " << error.what() << '\n';
        return 5;
    }
}
