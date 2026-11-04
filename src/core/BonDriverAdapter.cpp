#include "BonDriverRelay/bon_driver/BonDriverAdapter.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/core/Paths.h"

#include <bit>
#include <cmath>
#include <utility>

namespace bdr::bon_driver
{
    using core::ErrorCode;
    using core::RelayError;

    namespace
    {
        template <class Function> decltype(auto) CallDriver(Function&& function)
        {
            try {
                return std::forward<Function>(function)();
            }
            catch (...) {
                throw RelayError(ErrorCode::DriverFailure, "BonDriver raised a C++ exception");
            }
        }

        std::wstring CopyDriverString(const wchar_t* text)
        {
            if (!text) {
                return {};
            }
            constexpr std::size_t MAX_NAME_LENGTH = 4096;
            std::size_t length = 0;
            while (length < MAX_NAME_LENGTH && text[length] != L'\0') {
                ++length;
            }
            if (length == MAX_NAME_LENGTH) {
                throw RelayError(ErrorCode::DriverFailure, "BonDriver string exceeds 4095 characters");
            }
            return std::wstring(text, length);
        }
    } // namespace

    BonDriverAdapter::~BonDriverAdapter()
    {
        (void)Cleanup();
    }

    void BonDriverAdapter::RequireOwner() const
    {
        if (m_module && m_owner != std::this_thread::get_id()) {
            throw RelayError(ErrorCode::InvalidState, "BonDriver must be used on its owning thread");
        }
    }

    void BonDriverAdapter::RequireOpen() const
    {
        RequireOwner();
        if (!m_driver || !m_isOpen) {
            throw RelayError(ErrorCode::InvalidState, "Tuner is not open");
        }
    }

    void BonDriverAdapter::Load(const std::filesystem::path& path)
    {
        if (m_module || m_cleanupFailed) {
            throw RelayError(ErrorCode::InvalidState, "BonDriver is already loaded");
        }
        core::ValidateDllFile(path);
        auto normalized = path.lexically_normal();
        normalized.make_preferred();
        m_module = LoadLibraryExW(normalized.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m_module) {
            throw RelayError(ErrorCode::LoadFailure, "LoadLibraryExW failed", GetLastError());
        }
        m_owner = std::this_thread::get_id();
        try {
            const auto symbol = GetProcAddress(m_module, "CreateBonDriver");
            if (!symbol) {
                throw RelayError(ErrorCode::MissingExport, "CreateBonDriver export is missing", GetLastError());
            }
            const auto create = std::bit_cast<CreateBonDriverFunction>(symbol);
            m_driver = CallDriver([&] { return create(); });
            if (!m_driver) {
                throw RelayError(ErrorCode::FactoryFailure, "CreateBonDriver returned nullptr");
            }
            m_driver2 = dynamic_cast<IBonDriver2*>(m_driver);
        }
        catch (...) {
            (void)Cleanup();
            throw;
        }
    }

    void BonDriverAdapter::Open()
    {
        RequireOwner();
        if (!m_driver || m_isOpen) {
            throw RelayError(ErrorCode::InvalidState, "Tuner is not loaded or is already open");
        }
        // A failed/throwing Open may have partially acquired resources. Cleanup
        // must still attempt CloseTuner before Release.
        m_isOpen = true;
        const BOOL opened = CallDriver([&] { return m_driver->OpenTuner(); });
        if (opened == FALSE || (m_driver2 && CallDriver([&] { return m_driver2->IsTunerOpening(); }) == FALSE)) {
            m_isOpen = false;
            CallDriver([&] { m_driver->CloseTuner(); });
            throw RelayError(ErrorCode::DriverFailure, "OpenTuner failed");
        }
    }

    void BonDriverAdapter::Tune(const ChannelSelection& channel)
    {
        RequireOpen();
        BOOL tuned = FALSE;
        if (channel.legacy) {
            if (channel.channel > 255) {
                throw RelayError(ErrorCode::InvalidArgument, "Legacy channel must be 0..255");
            }
            tuned = CallDriver([&] { return m_driver->SetChannel(static_cast<BYTE>(channel.channel)); });
        }
        else {
            if (!m_driver2) {
                throw RelayError(ErrorCode::UnsupportedInterface, "Extended tuning requires IBonDriver2");
            }
            tuned = CallDriver([&] { return m_driver2->SetChannel(channel.space, channel.channel); });
        }
        if (tuned == FALSE) {
            throw RelayError(ErrorCode::DriverFailure, "SetChannel failed");
        }
        Purge();
    }

    void BonDriverAdapter::Purge()
    {
        RequireOpen();
        CallDriver([&] { m_driver->PurgeTsStream(); });
    }

    std::vector<std::uint8_t> BonDriverAdapter::ReadTs()
    {
        RequireOpen();
        BYTE* data = nullptr;
        DWORD size = 0;
        DWORD remaining = 0;
        const BOOL result = CallDriver([&] { return m_driver->GetTsStream(&data, &size, &remaining); });
        if (result == FALSE) {
            if (size != 0) {
                throw RelayError(ErrorCode::DriverFailure, "GetTsStream failed with a nonempty output");
            }
            return {};
        }
        if (size > MAX_TS_BLOCK_BYTES || (size > 0 && !data)) {
            throw RelayError(ErrorCode::InvalidStream, "GetTsStream returned an invalid pointer or oversized block");
        }
        if (size == 0) {
            return {};
        }
        return std::vector<std::uint8_t>(data, data + size);
    }

    float BonDriverAdapter::GetSignalLevel()
    {
        RequireOpen();
        const float level = CallDriver([&] { return m_driver->GetSignalLevel(); });
        if (!std::isfinite(level)) {
            throw RelayError(ErrorCode::DriverFailure, "Signal level is not finite");
        }
        return level;
    }

    bool BonDriverAdapter::HasExtendedInterface() const noexcept
    {
        return m_driver2 != nullptr;
    }

    std::wstring BonDriverAdapter::GetTunerName()
    {
        RequireOpen();
        if (!m_driver2) {
            throw RelayError(ErrorCode::UnsupportedInterface, "Tuner name requires IBonDriver2");
        }
        return CopyDriverString(CallDriver([&] { return m_driver2->GetTunerName(); }));
    }

    std::optional<std::wstring> BonDriverAdapter::GetSpaceName(std::uint32_t space)
    {
        RequireOpen();
        if (!m_driver2) {
            throw RelayError(ErrorCode::UnsupportedInterface, "Space enumeration requires IBonDriver2");
        }
        const auto* name = CallDriver([&] { return m_driver2->EnumTuningSpace(space); });
        if (!name) {
            return std::nullopt;
        }
        return CopyDriverString(name);
    }

    std::optional<std::wstring> BonDriverAdapter::GetChannelName(std::uint32_t space, std::uint32_t channel)
    {
        RequireOpen();
        if (!m_driver2) {
            throw RelayError(ErrorCode::UnsupportedInterface, "Channel enumeration requires IBonDriver2");
        }
        const auto* name = CallDriver([&] { return m_driver2->EnumChannelName(space, channel); });
        if (!name) {
            return std::nullopt;
        }
        return CopyDriverString(name);
    }

    bool BonDriverAdapter::Cleanup() noexcept
    {
        bool success = !m_cleanupFailed;
        bool released = true;
        if (m_driver && std::exchange(m_isOpen, false)) {
            try {
                m_driver->CloseTuner();
            }
            catch (...) {
                success = false;
            }
        }
        if (auto* driver = std::exchange(m_driver, nullptr)) {
            try {
                driver->Release();
            }
            catch (...) {
                released = false;
                success = false;
            }
        }
        m_driver2 = nullptr;
        if (!released) {
            m_cleanupFailed = true;
        }
        if (auto module = std::exchange(m_module, nullptr)) {
            if (released) {
                if (!FreeLibrary(module)) {
                    success = false;
                }
            }
            else {
                OutputDebugStringW(L"BonDriverRelay: Release threw; keeping its DLL loaded for safety.\n");
            }
        }
        return success;
    }

    void BonDriverAdapter::Unload()
    {
        RequireOwner();
        if (!Cleanup()) {
            throw RelayError(ErrorCode::DriverFailure, "BonDriver cleanup failed; a throwing Release pins its DLL");
        }
    }
} // namespace bdr::bon_driver
