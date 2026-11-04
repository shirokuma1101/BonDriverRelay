#pragma once
#include <filesystem>
namespace bdr::service
{
    // Connects to SCM only. Registration, account and recovery settings are user-managed.
    int RunWindowsService(const std::filesystem::path& config_path);
} // namespace bdr::service
