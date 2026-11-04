#include "BonDriverRelay/core/Paths.h"
#include "BonDriverRelay/core/Error.h"

#include <Windows.h>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

namespace bdr::core
{
    std::string ToUtf8(std::wstring_view text)
    {
        if (text.empty()) {
            return {};
        }
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw RelayError(ErrorCode::InvalidArgument, "UTF-16 string is too long");
        }
        const int length = static_cast<int>(text.size());
        const int required =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
        if (required == 0) {
            throw RelayError(ErrorCode::InvalidArgument, "Invalid UTF-16 string", GetLastError());
        }
        std::string result(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), required, nullptr,
                                nullptr) == 0) {
            throw RelayError(ErrorCode::InvalidArgument, "UTF-16 conversion failed", GetLastError());
        }
        return result;
    }

    std::filesystem::path PathFromUtf8(std::string_view text)
    {
        if (text.empty()) {
            return {};
        }
        if (text.size() > 131'072 || text.find('\0') != std::string_view::npos) {
            throw RelayError(ErrorCode::Configuration, "Invalid path length or embedded NUL");
        }
        const int length = static_cast<int>(text.size());
        const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
        if (required == 0) {
            throw RelayError(ErrorCode::Configuration, "Invalid UTF-8 path", GetLastError());
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), required) == 0) {
            throw RelayError(ErrorCode::Configuration, "UTF-8 path conversion failed", GetLastError());
        }
        return std::filesystem::path(result);
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        return ToUtf8(path.native());
    }

    std::filesystem::path GetExecutablePath()
    {
        std::vector<wchar_t> buffer(512);
        for (;;) {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                throw RelayError(ErrorCode::FileFailure, "GetModuleFileNameW failed", GetLastError());
            }
            if (length < buffer.size()) {
                return std::filesystem::path(std::wstring(buffer.data(), length));
            }
            if (buffer.size() >= 32'768) {
                throw RelayError(ErrorCode::FileFailure, "Executable path exceeds limit");
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    void ValidateDllFile(const std::filesystem::path& path)
    {
        if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos) {
            throw RelayError(ErrorCode::InvalidDll, "DLL path must be an explicit absolute path");
        }
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw RelayError(ErrorCode::InvalidDll, "DLL file cannot be opened: " + PathToUtf8(path));
        }
        const auto size = file.tellg();
        IMAGE_DOS_HEADER dos{};
        file.seekg(0);
        file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
        if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos))) {
            throw RelayError(ErrorCode::InvalidDll, "DLL has an invalid DOS header");
        }
        constexpr auto PREFIX_SIZE = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        const auto offset = static_cast<std::streamoff>(dos.e_lfanew);
        if (offset > size || size - offset < static_cast<std::streamoff>(PREFIX_SIZE)) {
            throw RelayError(ErrorCode::InvalidDll, "DLL has a truncated PE header");
        }
        DWORD signature = 0;
        IMAGE_FILE_HEADER header{};
        file.seekg(offset);
        file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || signature != IMAGE_NT_SIGNATURE || (header.Characteristics & IMAGE_FILE_DLL) == 0 ||
            header.SizeOfOptionalHeader < sizeof(WORD) ||
            size - offset - static_cast<std::streamoff>(PREFIX_SIZE) < header.SizeOfOptionalHeader) {
            throw RelayError(ErrorCode::InvalidDll, "DLL has invalid PE metadata");
        }
        WORD magic = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
#if defined(_M_X64)
        constexpr WORD EXPECTED_MACHINE = IMAGE_FILE_MACHINE_AMD64;
        constexpr WORD EXPECTED_MAGIC = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#elif defined(_M_IX86)
        constexpr WORD EXPECTED_MACHINE = IMAGE_FILE_MACHINE_I386;
        constexpr WORD EXPECTED_MAGIC = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#else
#error Unsupported architecture
#endif
        if (header.Machine != EXPECTED_MACHINE || magic != EXPECTED_MAGIC) {
            throw RelayError(ErrorCode::ArchitectureMismatch, "DLL architecture does not match this process");
        }
    }
} // namespace bdr::core
