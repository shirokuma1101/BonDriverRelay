#include "BonDriverRelay/client/BonDriverRelayClient.h"
#include <vector>

namespace
{
    const int MODULE_ANCHOR = 0;
    std::filesystem::path ConfigPath()
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&MODULE_ANCHOR), &module)) {
            throw std::runtime_error("Cannot locate client DLL");
        }
        std::vector<wchar_t> path(32768);
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            throw std::runtime_error("Client DLL path exceeds limit");
        }
        auto result = std::filesystem::path(std::wstring(path.data(), length));
        result.replace_extension(L".toml");
        return result;
    }
} // namespace

extern "C" IBonDriver* __cdecl CreateBonDriver()
{
    try {
        const auto path = ConfigPath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return nullptr;
        }
        return new bdr::client::BonDriverRelayClient(bdr::core::LoadClientConfig(path));
    }
    catch (...) {
        SetLastError(ERROR_INVALID_DATA);
        return nullptr;
    }
}

extern "C" BOOL __cdecl GetRelayStatus(IBonDriver* driver, bdr::client::RelayStatus* output)
{
    if (!driver || !output || output->size != sizeof(*output)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    try {
        const auto* client = dynamic_cast<bdr::client::BonDriverRelayClient*>(driver);
        if (!client) {
            return FALSE;
        }
        *output = client->GetDiagnostics();
        return TRUE;
    }
    catch (...) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
}
