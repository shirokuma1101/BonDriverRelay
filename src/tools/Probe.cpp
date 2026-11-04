#include "BonDriverRelay/bon_driver/BonDriverAbi.h"
#include "BonDriverRelay/client/Diagnostics.h"
#include "BonDriverRelay/core/Paths.h"
#include <bit>
#include <chrono>
#include <iostream>
namespace
{
    struct LoadedDriver
    {
        HMODULE module = nullptr;
        IBonDriver* driver = nullptr;
        ~LoadedDriver()
        {
            if (driver) {
                driver->Release();
            }
            if (module) {
                FreeLibrary(module);
            }
        }
    };
    DWORD Number(const wchar_t* text, DWORD maximum)
    {
        std::size_t used = 0;
        const auto number = std::stoul(text, &used, 10);
        if (used != std::wstring_view(text).size() || number > maximum || text[0] == L'-') {
            throw std::runtime_error("Invalid numeric argument");
        }
        return number;
    }
} // namespace
int wmain(int argc, wchar_t* argv[])
{
    if (argc != 2 && argc != 4 && argc != 5) {
        std::cerr << "Usage: BonDriverRelayProbe ABS_CLIENT_DLL [SPACE CHANNEL [DURATION_MS]]\n";
        return 2;
    }
    try {
        const std::filesystem::path path(argv[1]);
        bdr::core::ValidateDllFile(path);
        const DWORD space = argc >= 4 ? Number(argv[2], 63) : 0, channel = argc >= 4 ? Number(argv[3], 4095) : 0;
        const DWORD duration = argc == 5 ? Number(argv[4], 3'600'000) : 2000;
        if (!duration) {
            throw std::runtime_error("Duration must be positive");
        }
        LoadedDriver loaded;
        loaded.module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loaded.module) {
            throw std::runtime_error("Client LoadLibrary failed");
        }
        const auto symbol = GetProcAddress(loaded.module, "CreateBonDriver");
        if (!symbol) {
            throw std::runtime_error("CreateBonDriver missing");
        }
        loaded.driver = std::bit_cast<bdr::bon_driver::CreateBonDriverFunction>(symbol)();
        if (!loaded.driver) {
            throw std::runtime_error("CreateBonDriver failed; check same-name TOML");
        }
        auto* driver = dynamic_cast<IBonDriver2*>(loaded.driver);
        if (!driver || !driver->OpenTuner() || !driver->SetChannel(space, channel)) {
            throw std::runtime_error("Remote open/tune failed");
        }
        std::uint64_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
        while (std::chrono::steady_clock::now() < deadline) {
            const DWORD wait = driver->WaitTsStream(100);
            if (wait == WAIT_TIMEOUT) {
                continue;
            }
            if (wait != WAIT_OBJECT_0) {
                throw std::runtime_error("TS stream failed or disconnected");
            }
            BYTE* data = nullptr;
            DWORD size = 0, remaining = 0;
            if (!driver->GetTsStream(&data, &size, &remaining)) {
                continue;
            }
            if (!data || !size || size > 188 * 256 || size % 188) {
                throw std::runtime_error("Invalid TS size");
            }
            for (DWORD offset = 0; offset < size; offset += 188) {
                if (data[offset] != 0x47) {
                    throw std::runtime_error("Invalid TS synchronization");
                }
            }
            received += size;
        }
        bdr::client::RelayStatus status;
        const auto diagnostics = GetProcAddress(loaded.module, "GetRelayStatus");
        if (!diagnostics || !std::bit_cast<BOOL(__cdecl*)(IBonDriver*, bdr::client::RelayStatus*)>(diagnostics)(
                                loaded.driver, &status)) {
            throw std::runtime_error("Relay diagnostics unavailable");
        }
        std::cout << "client_bits=" << sizeof(void*) * 8 << " received_bytes=" << received
                  << " decoder_state=" << status.decoderState << " dropped_packets=" << status.droppedPackets
                  << " last_error=" << status.lastError << '\n';
        driver->CloseTuner();
        return received > 0 && status.lastError == 0 && status.droppedPackets == 0 ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << "; win32=" << GetLastError() << '\n';
        return 1;
    }
}
