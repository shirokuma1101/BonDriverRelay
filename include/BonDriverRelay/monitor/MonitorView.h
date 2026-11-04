#pragma once
#include "BonDriverRelay/core/RuntimeSnapshot.h"
#include "BonDriverRelay/core/ConfigurationReloader.h"
namespace bdr::monitor
{
    struct PeerSnapshot
    {
        std::string name, host;
        std::uint16_t port = 0;
        bool up = false;
        std::uint64_t ageMs = 0;
        std::string statusJson;
    };
    std::string JsonString(std::string_view text);
    std::string LocalJson(const std::string& name, const core::ServerSnapshot& snapshot,
                          const core::ReloadSnapshot& reload);
    std::string StatusJson(const std::string& local, const std::vector<PeerSnapshot>& peers);
    std::string Metrics(const std::string& name, const core::ServerSnapshot& snapshot,
                        const core::ReloadSnapshot& reload, const std::vector<PeerSnapshot>& peers);
    std::string_view DashboardHtml();
    std::string_view DashboardScript();
    std::string_view DashboardStyle();
} // namespace bdr::monitor
