#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace bdr::core
{
    [[nodiscard]] std::string ToUtf8(std::wstring_view text);
    [[nodiscard]] std::filesystem::path PathFromUtf8(std::string_view text);
    [[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);
    [[nodiscard]] std::filesystem::path GetExecutablePath();
    // Reads PE metadata only. No DLL code is executed.
    void ValidateDllFile(const std::filesystem::path& path);
} // namespace bdr::core
