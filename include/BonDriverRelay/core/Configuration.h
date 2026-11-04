#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bdr::core
{
    struct NetworkConfig
    {
        std::string bindAddress = "127.0.0.1";
        std::uint16_t port = 57230;
        std::uint32_t maxClients = 8;
        std::uint32_t connectTimeoutMs = 5000;
        std::uint32_t ioTimeoutMs = 5000;
        std::uint32_t shutdownTimeoutMs = 10000;
        std::uint32_t startupTimeoutMs = 60000;
        bool operator==(const NetworkConfig&) const = default;
    };
    struct DistributionConfig
    {
        std::uint32_t queueBytesPerClient = 4'194'280;
        std::string overflowPolicy = "disconnect";
        bool operator==(const DistributionConfig&) const = default;
    };
    struct DecoderConfig
    {
        bool enabled = false;
        std::filesystem::path dllPath;
        std::string failurePolicy = "stop_stream";
        std::uint32_t multi2Round = 4;
        std::uint32_t simdMode = 0;
        bool operator==(const DecoderConfig&) const = default;
    };
    struct LoggingConfig
    {
        std::string level = "info";
        std::filesystem::path directory;
        std::uint32_t rotateBytes = 10'485'760;
        std::uint32_t rotateFiles = 3;
        std::uint32_t statusIntervalMs = 30000;
        bool operator==(const LoggingConfig&) const = default;
    };
    struct TunerDefinition
    {
        std::string name;
        std::filesystem::path dllPath;
        std::string group = "default";
        bool operator==(const TunerDefinition&) const = default;
    };
    struct MonitorPeer
    {
        std::string name;
        std::string host = "127.0.0.1";
        std::uint16_t port = 57231;
        bool operator==(const MonitorPeer&) const = default;
    };
    struct MonitoringConfig
    {
        bool enabled = false;
        std::string name = "local", bindAddress = "127.0.0.1";
        std::uint16_t port = 57231;
        std::uint32_t peerIntervalMs = 5000, peerTimeoutMs = 500;
        std::vector<MonitorPeer> peers;
        bool operator==(const MonitoringConfig&) const = default;
    };
    struct ReloadConfig
    {
        bool enabled = false;
        std::uint32_t intervalMs = 1000;
        bool operator==(const ReloadConfig&) const = default;
    };
    struct ServerConfig
    {
        std::filesystem::path configPath;
        NetworkConfig network;
        DistributionConfig distribution;
        DecoderConfig decoder;
        LoggingConfig logging;
        std::vector<TunerDefinition> tuners;
        MonitoringConfig monitoring;
        ReloadConfig reload;
        bool operator==(const ServerConfig&) const = default;
    };

    [[nodiscard]] ServerConfig LoadServerConfig(const std::filesystem::path& path);
    [[nodiscard]] const TunerDefinition& SelectTuner(const ServerConfig& config, const std::string& name);
    struct ClientEndpoint
    {
        std::string host = "127.0.0.1";
        std::uint16_t port = 57230;
        bool operator==(const ClientEndpoint&) const = default;
    };
    struct ClientConfig
    {
        std::string host = "127.0.0.1";
        std::uint16_t port = 57230;
        std::uint32_t connectTimeoutMs = 5000;
        std::uint32_t requestTimeoutMs = 5000;
        std::uint32_t queueBytes = 4'194'280;
        std::string group = "default";
        int priority = 0;
        bool exclusive = false;
        std::vector<ClientEndpoint> fallbackServers;
        std::uint32_t selectionTimeoutMs = 15000;
    };
    [[nodiscard]] ClientConfig LoadClientConfig(const std::filesystem::path& path);
} // namespace bdr::core
