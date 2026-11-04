#include "BonDriverRelay/decoder/B25DecoderAdapter.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/core/Paths.h"
#include "IB25Decoder.h"
#include <bit>
#include <TlHelp32.h>
#include <utility>
#include <mutex>

namespace bdr::decoder
{
    namespace
    {
        constexpr DWORD MAX_OUTPUT_BYTES = 188 * 5'577;
        // Serialize only factory/release/module lifetime; decoding and card
        // initialization remain independent between tuner owners.
        std::mutex decoder_lifetime_mutex;
        std::string ModulePath(HMODULE module) noexcept
        {
            try {
                std::vector<wchar_t> buffer(32768);
                const DWORD count = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (!count || count >= buffer.size()) {
                    return "unavailable";
                }
                return core::ToUtf8(std::wstring_view(buffer.data(), count));
            }
            catch (...) {
                return "unavailable";
            }
        }
        std::string LoadedWinSCardModules() noexcept
        {
            struct LastErrorGuard
            {
                DWORD value = GetLastError();
                ~LastErrorGuard()
                {
                    SetLastError(value);
                }
            } last_error;
            try {
                struct Snapshot
                {
                    HANDLE handle = INVALID_HANDLE_VALUE;
                    ~Snapshot()
                    {
                        if (handle != INVALID_HANDLE_VALUE) {
                            CloseHandle(handle);
                        }
                    }
                } snapshot;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    snapshot.handle = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
                    if (snapshot.handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) {
                        break;
                    }
                }
                if (snapshot.handle == INVALID_HANDLE_VALUE) {
                    return "snapshot_unavailable";
                }
                MODULEENTRY32W entry{};
                entry.dwSize = sizeof(entry);
                std::string result = "[";
                bool found = Module32FirstW(snapshot.handle, &entry) != FALSE;
                if (!found && GetLastError() != ERROR_NO_MORE_FILES) {
                    return "snapshot_unavailable";
                }
                unsigned matches = 0;
                while (found) {
                    if (_wcsicmp(entry.szModule, L"winscard.dll") == 0) {
                        if (matches++) {
                            result += ",";
                        }
                        result += "\"" + ModulePath(entry.hModule) + "\"";
                    }
                    found = Module32NextW(snapshot.handle, &entry) != FALSE;
                }
                if (GetLastError() != ERROR_NO_MORE_FILES) {
                    return "snapshot_unavailable";
                }
                return result + "]";
            }
            catch (...) {
                return "unavailable";
            }
        }
        [[noreturn]] void RethrowForeign()
        {
            try {
                throw;
            }
            catch (const core::RelayError&) {
                throw;
            }
            catch (...) {
                throw core::RelayError(core::ErrorCode::DriverFailure, "B25 DLL raised an exception");
            }
        }
        void Validate(std::span<const std::uint8_t> data)
        {
            if (data.size() > MAX_OUTPUT_BYTES || data.size() % 188 != 0) {
                throw core::RelayError(core::ErrorCode::InvalidStream, "B25 output has invalid size");
            }
            for (std::size_t offset = 0; offset < data.size(); offset += 188) {
                if (data[offset] != 0x47) {
                    throw core::RelayError(core::ErrorCode::InvalidStream, "B25 output has invalid synchronization");
                }
            }
        }
    } // namespace
    B25DecoderAdapter::~B25DecoderAdapter()
    {
        (void)Cleanup();
    }
    bool B25DecoderAdapter::Cleanup() noexcept
    {
        bool released = true;
        if (auto* decoder = std::exchange(m_decoder, nullptr)) {
            try {
                std::lock_guard lock(decoder_lifetime_mutex);
                decoder->Release();
            }
            catch (...) {
                released = false;
                m_poisoned = true;
            }
        }
        if (auto module = std::exchange(m_module, nullptr)) {
            if (released) {
                std::lock_guard lock(decoder_lifetime_mutex);
                if (!FreeLibrary(module)) {
                    return false;
                }
            }
            else {
                OutputDebugStringW(L"BonDriverRelay: B25 Release threw; its module is pinned.\n");
            }
        }
        return released && !m_poisoned;
    }
    void B25DecoderAdapter::Close()
    {
        if (!Cleanup()) {
            throw core::RelayError(core::ErrorCode::DriverFailure, "B25 cleanup failed");
        }
    }
    void B25DecoderAdapter::Open(const core::DecoderConfig& config)
    {
        Close();
        m_passthrough = config.failurePolicy == "passthrough";
        m_state = DecoderState::Disabled;
        if (!config.enabled) {
            return;
        }
        m_state = DecoderState::Unavailable;
        try {
            core::ValidateDllFile(config.dllPath);
            auto path = config.dllPath;
            path.make_preferred();
            DWORD load_error = ERROR_SUCCESS;
            {
                std::lock_guard lock(decoder_lifetime_mutex);
                m_module = LoadLibraryExW(path.c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!m_module) {
                    load_error = GetLastError();
                }
            }
            if (!m_module) {
                throw core::RelayError(core::ErrorCode::LoadFailure, "Cannot load B25 DLL", load_error);
            }
            const auto symbol = GetProcAddress(m_module, "CreateB25Decoder2");
            if (!symbol) {
                throw core::RelayError(core::ErrorCode::MissingExport, "CreateB25Decoder2 is missing", GetLastError());
            }
            using Factory = IB25Decoder2*(__cdecl*)();
            {
                std::lock_guard lock(decoder_lifetime_mutex);
                m_decoder = std::bit_cast<Factory>(symbol)();
            }
            if (!m_decoder) {
                throw core::RelayError(core::ErrorCode::FactoryFailure, "B25 factory returned nullptr");
            }
            m_state = DecoderState::InitializationFailed;
            const auto winscard_before = LoadedWinSCardModules();
            if (m_decoder->Initialize(config.multi2Round) == FALSE) {
                throw core::RelayError(
                    core::ErrorCode::DriverFailure,
                    "B25 Initialize failed (card/init failure; ABI exposes no reliable cause); b25_module=\"" +
                        ModulePath(m_module) + "\" winscard_before=" + winscard_before +
                        " winscard_after=" + LoadedWinSCardModules());
            }
            m_decoder->DiscardNullPacket(false);
            m_decoder->DiscardScramblePacket(false);
            m_decoder->EnableEmmProcess(false);
            m_decoder->SetMulti2Round(static_cast<std::int32_t>(config.multi2Round));
            m_decoder->SetSimdMode(static_cast<std::int32_t>(config.simdMode));
            if (m_decoder->Reset() == FALSE) {
                throw core::RelayError(core::ErrorCode::DriverFailure, "B25 initial Reset failed");
            }
            m_state = DecoderState::ActiveUnverified;
        }
        catch (...) {
            ++m_failures;
            (void)Cleanup();
            if (!m_passthrough || m_poisoned) {
                RethrowForeign();
            }
        }
    }
    std::vector<std::uint8_t> B25DecoderAdapter::CopyOutput(BYTE* data, DWORD length)
    {
        if (length > MAX_OUTPUT_BYTES || (length != 0 && !data)) {
            throw core::RelayError(core::ErrorCode::InvalidStream, "B25 returned an invalid borrowed buffer");
        }
        if (length == 0) {
            return {};
        }
        std::vector<std::uint8_t> output(data, data + length);
        Validate(output);
        return output;
    }
    void B25DecoderAdapter::Reset()
    {
        if (!m_decoder) {
            if (m_state != DecoderState::Disabled && !m_passthrough) {
                throw core::RelayError(core::ErrorCode::InvalidState, "B25 is unavailable");
            }
            return;
        }
        try {
            if (m_decoder->Reset() == FALSE) {
                throw core::RelayError(core::ErrorCode::DriverFailure, "B25 Reset failed");
            }
            m_state = DecoderState::ActiveUnverified;
        }
        catch (...) {
            ++m_failures;
            m_state = DecoderState::DecodeFailed;
            RethrowForeign();
        }
    }
    std::vector<std::uint8_t> B25DecoderAdapter::Decode(std::span<const std::uint8_t> source)
    {
        Validate(source);
        if (m_state == DecoderState::Disabled || !m_decoder || m_state == DecoderState::DecodeFailed) {
            if (m_state != DecoderState::Disabled && !m_passthrough) {
                throw core::RelayError(core::ErrorCode::DriverFailure, "B25 is not usable");
            }
            return {source.begin(), source.end()};
        }
        try {
            std::vector<std::uint8_t> writable(source.begin(), source.end());
            BYTE* data = nullptr;
            DWORD length = 0;
            if (m_decoder->Decode(writable.data(), static_cast<DWORD>(writable.size()), &data, &length) == FALSE) {
                throw core::RelayError(core::ErrorCode::DriverFailure, "B25 Decode failed");
            }
            auto result = CopyOutput(data, length);
            m_state = DecoderState::ActiveUnverified;
            for (std::size_t i = 0; i < result.size(); i += 188) {
                if ((result[i + 3] & 0xc0) != 0) {
                    m_state = DecoderState::ScrambledOutput;
                    break;
                }
            }
            return result;
        }
        catch (...) {
            ++m_failures;
            m_state = DecoderState::DecodeFailed;
            if (!m_passthrough) {
                RethrowForeign();
            }
            return {source.begin(), source.end()};
        }
    }
    std::vector<std::uint8_t> B25DecoderAdapter::Flush()
    {
        if (!m_decoder || m_state == DecoderState::DecodeFailed) {
            return {};
        }
        try {
            BYTE* data = nullptr;
            DWORD size = 0;
            if (m_decoder->Flush(&data, &size) == FALSE) {
                throw core::RelayError(core::ErrorCode::DriverFailure, "B25 Flush failed");
            }
            return CopyOutput(data, size);
        }
        catch (...) {
            ++m_failures;
            m_state = DecoderState::DecodeFailed;
            RethrowForeign();
        }
    }
    DecoderState B25DecoderAdapter::GetState() const noexcept
    {
        return m_state;
    }
    std::uint64_t B25DecoderAdapter::GetFailureCount() const noexcept
    {
        return m_failures;
    }
} // namespace bdr::decoder
