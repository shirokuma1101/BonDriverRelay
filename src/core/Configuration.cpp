#include "BonDriverRelay/core/Configuration.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/core/Paths.h"

#include <toml++/toml.hpp>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string_view>

namespace bdr::core
{
    namespace
    {
        void CheckKeys(const toml::table& table, std::initializer_list<std::string_view> allowed,
                       std::string_view context)
        {
            for (const auto& [key, value] : table) {
                (void)value;
                if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
                    throw RelayError(ErrorCode::Configuration,
                                     "Unknown key in " + std::string(context) + ": " + std::string(key.str()));
                }
            }
        }

        const toml::table& GetTable(const toml::table& root, std::string_view key)
        {
            static const toml::table EMPTY_TABLE;
            const auto* node = root.get(key);
            if (!node) {
                return EMPTY_TABLE;
            }
            const auto* table = node->as_table();
            if (!table) {
                throw RelayError(ErrorCode::Configuration, std::string(key) + " must be a table");
            }
            return *table;
        }

        std::uint32_t ReadNumber(const toml::table& table, std::string_view key, std::uint32_t fallback,
                                 std::uint32_t minimum, std::uint32_t maximum)
        {
            const auto* node = table.get(key);
            if (!node) {
                return fallback;
            }
            const auto value = node->value<std::int64_t>();
            if (!node->is_integer() || !value || *value < minimum || *value > maximum) {
                throw RelayError(ErrorCode::Configuration, std::string(key) + " must be an integer in range " +
                                                               std::to_string(minimum) + ".." +
                                                               std::to_string(maximum));
            }
            return static_cast<std::uint32_t>(*value);
        }

        std::string ReadString(const toml::table& table, std::string_view key, std::string fallback)
        {
            const auto* node = table.get(key);
            if (!node) {
                return fallback;
            }
            const auto value = node->value<std::string>();
            if (!value) {
                throw RelayError(ErrorCode::Configuration, std::string(key) + " must be a string");
            }
            if (value->find('\0') != std::string::npos) {
                throw RelayError(ErrorCode::Configuration, std::string(key) + " contains NUL");
            }
            return *value;
        }

        bool ReadBool(const toml::table& table, std::string_view key, bool fallback)
        {
            const auto* node = table.get(key);
            if (!node) {
                return fallback;
            }
            const auto value = node->value<bool>();
            if (!value) {
                throw RelayError(ErrorCode::Configuration, std::string(key) + " must be a boolean");
            }
            return *value;
        }

        bool IsIpv4(std::string_view address)
        {
            for (int part = 0; part < 4; ++part) {
                const auto separator = address.find('.');
                const auto text = address.substr(0, separator);
                unsigned value = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                    value > 255) {
                    return false;
                }
                if (part == 3) {
                    return separator == std::string_view::npos;
                }
                if (separator == std::string_view::npos) {
                    return false;
                }
                address.remove_prefix(separator + 1);
            }
            return false;
        }

        bool IsTunerName(std::string_view name)
        {
            return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                       ch == '-' || ch == '.';
            });
        }
    } // namespace

    ServerConfig LoadServerConfig(const std::filesystem::path& path)
    {
        if (!path.is_absolute()) {
            throw RelayError(ErrorCode::Configuration, "Config path must be absolute");
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            throw RelayError(ErrorCode::Configuration, "Config file cannot be opened: " + PathToUtf8(path));
        }
        const auto size = input.tellg();
        if (size < 0 || size > 1'048'576) {
            throw RelayError(ErrorCode::Configuration, "Config file exceeds 1 MiB or cannot be read");
        }
        input.seekg(0);
        toml::table root;
        try {
            root = toml::parse(input, PathToUtf8(path));
        }
        catch (const toml::parse_error& error) {
            throw RelayError(ErrorCode::Configuration,
                             "TOML parse error at line " + std::to_string(error.source().begin.line));
        }
        CheckKeys(root,
                  {"schema_version", "network", "distribution", "decoder", "logging", "tuners", "monitoring", "reload"},
                  "root");
        if (!root.contains("schema_version")) {
            throw RelayError(ErrorCode::Configuration, "schema_version is required");
        }
        (void)ReadNumber(root, "schema_version", 1, 1, 1);
        ServerConfig config;
        config.configPath = path.lexically_normal();
        const auto& network = GetTable(root, "network");
        CheckKeys(network,
                  {"bind_address", "port", "max_clients", "connect_timeout_ms", "io_timeout_ms", "shutdown_timeout_ms",
                   "startup_timeout_ms"},
                  "network");
        config.network.bindAddress = ReadString(network, "bind_address", "127.0.0.1");
        if (!IsIpv4(config.network.bindAddress)) {
            throw RelayError(ErrorCode::Configuration, "bind_address must be an IPv4 literal");
        }
        config.network.port = static_cast<std::uint16_t>(ReadNumber(network, "port", 57230, 1, 65535));
        config.network.maxClients = ReadNumber(network, "max_clients", 8, 1, 64);
        config.network.connectTimeoutMs = ReadNumber(network, "connect_timeout_ms", 5000, 100, 120000);
        config.network.ioTimeoutMs = ReadNumber(network, "io_timeout_ms", 5000, 100, 120000);
        config.network.shutdownTimeoutMs = ReadNumber(network, "shutdown_timeout_ms", 10000, 100, 120000);
        config.network.startupTimeoutMs = ReadNumber(network, "startup_timeout_ms", 60000, 100, 600000);

        const auto& distribution = GetTable(root, "distribution");
        CheckKeys(distribution, {"queue_bytes_per_client", "overflow_policy"}, "distribution");
        config.distribution.queueBytesPerClient =
            ReadNumber(distribution, "queue_bytes_per_client", 4'194'280, 188, 67'108'856);
        if (config.distribution.queueBytesPerClient % 188 != 0) {
            throw RelayError(ErrorCode::Configuration, "queue_bytes_per_client must be a multiple of 188");
        }
        config.distribution.overflowPolicy = ReadString(distribution, "overflow_policy", "disconnect");
        if (config.distribution.overflowPolicy != "disconnect" && config.distribution.overflowPolicy != "drop_oldest") {
            throw RelayError(ErrorCode::Configuration, "Only overflow_policy=disconnect is supported");
        }

        const auto& decoder = GetTable(root, "decoder");
        CheckKeys(decoder, {"enabled", "dll_path", "failure_policy", "multi2_round", "simd_mode"}, "decoder");
        config.decoder.enabled = ReadBool(decoder, "enabled", false);
        config.decoder.dllPath = PathFromUtf8(ReadString(decoder, "dll_path", ""));
        config.decoder.failurePolicy = ReadString(decoder, "failure_policy", "stop_stream");
        if (config.decoder.failurePolicy != "stop_stream" && config.decoder.failurePolicy != "passthrough") {
            throw RelayError(ErrorCode::Configuration, "Only decoder failure_policy=stop_stream is supported");
        }
        config.decoder.multi2Round = ReadNumber(decoder, "multi2_round", 4, 1, 16);
        config.decoder.simdMode = ReadNumber(decoder, "simd_mode", 0, 0, 3);
        if (config.decoder.enabled && !config.decoder.dllPath.is_absolute()) {
            throw RelayError(ErrorCode::Configuration, "Enabled decoder requires an absolute dll_path");
        }
        if (!config.decoder.dllPath.empty()) {
            if (config.decoder.failurePolicy != "passthrough" || std::filesystem::exists(config.decoder.dllPath)) {
                ValidateDllFile(config.decoder.dllPath);
            }
        }

        const auto& logging = GetTable(root, "logging");
        CheckKeys(logging, {"level", "directory", "rotate_bytes", "rotate_files", "status_interval_ms"}, "logging");
        config.logging.level = ReadString(logging, "level", "info");
        if (config.logging.level != "debug" && config.logging.level != "info" && config.logging.level != "warn" &&
            config.logging.level != "error") {
            throw RelayError(ErrorCode::Configuration, "logging.level must be debug, info, warn or error");
        }
        config.logging.directory = PathFromUtf8(ReadString(logging, "directory", "logs"));
        if (config.logging.directory.empty()) {
            throw RelayError(ErrorCode::Configuration, "logging.directory must not be empty");
        }
        if (config.logging.directory.is_relative()) {
            config.logging.directory = path.parent_path() / config.logging.directory;
        }
        config.logging.directory = config.logging.directory.lexically_normal();
        config.logging.rotateBytes = ReadNumber(logging, "rotate_bytes", 10'485'760, 1024, 1'073'741'824);
        config.logging.rotateFiles = ReadNumber(logging, "rotate_files", 3, 1, 10);
        config.logging.statusIntervalMs = ReadNumber(logging, "status_interval_ms", 30000, 1000, 3600000);

        if (const auto* node = root.get("tuners")) {
            const auto* array = node->as_array();
            if (!array || array->size() > 64) {
                throw RelayError(ErrorCode::Configuration, "tuners must be an array of at most 64 tables");
            }
            std::set<std::string> names;
            for (const auto& entry : *array) {
                const auto* table = entry.as_table();
                if (!table) {
                    throw RelayError(ErrorCode::Configuration, "Each tuner must be a table");
                }
                CheckKeys(*table, {"name", "dll_path", "group"}, "tuners");
                TunerDefinition tuner{ReadString(*table, "name", ""), PathFromUtf8(ReadString(*table, "dll_path", ""))};
                tuner.group = ReadString(*table, "group", "default");
                if (!IsTunerName(tuner.group)) {
                    throw RelayError(ErrorCode::Configuration, "Invalid tuner group");
                }
                if (!IsTunerName(tuner.name)) {
                    throw RelayError(ErrorCode::Configuration, "Tuner name must be 1..64 ASCII letters/digits/._-");
                }
                if (!names.insert(tuner.name).second) {
                    throw RelayError(ErrorCode::Configuration, "Duplicate tuner name: " + tuner.name);
                }
                ValidateDllFile(tuner.dllPath);
                for (const auto& existing : config.tuners) {
                    if (std::filesystem::equivalent(existing.dllPath, tuner.dllPath)) {
                        throw RelayError(ErrorCode::Configuration,
                                         "Each physical tuner must have a distinct DLL file path");
                    }
                }
                config.tuners.push_back(std::move(tuner));
            }
        }
        const auto& monitoring = GetTable(root, "monitoring");
        CheckKeys(monitoring,
                  {"enabled", "name", "bind_address", "port", "peer_interval_ms", "peer_timeout_ms", "peers"},
                  "monitoring");
        config.monitoring.enabled = ReadBool(monitoring, "enabled", false);
        config.monitoring.name = ReadString(monitoring, "name", "local");
        config.monitoring.bindAddress = ReadString(monitoring, "bind_address", "127.0.0.1");
        if (!IsTunerName(config.monitoring.name) || !IsIpv4(config.monitoring.bindAddress)) {
            throw RelayError(ErrorCode::Configuration, "Invalid monitoring name/bind_address");
        }
        config.monitoring.port = static_cast<std::uint16_t>(ReadNumber(monitoring, "port", 57231, 1, 65535));
        config.monitoring.peerIntervalMs = ReadNumber(monitoring, "peer_interval_ms", 5000, 1000, 3600000);
        config.monitoring.peerTimeoutMs = ReadNumber(monitoring, "peer_timeout_ms", 500, 100, 2000);
        if (config.monitoring.enabled && config.monitoring.port == config.network.port) {
            throw RelayError(ErrorCode::Configuration, "Monitoring and TS ports must differ");
        }
        if (const auto* node = monitoring.get("peers")) {
            const auto* peers = node->as_array();
            if (!peers || peers->size() > 8) {
                throw RelayError(ErrorCode::Configuration, "monitoring.peers must contain at most 8 tables");
            }
            std::set<std::string> names;
            std::set<std::pair<std::string, std::uint16_t>> addresses;
            for (const auto& entry : *peers) {
                const auto* peer = entry.as_table();
                if (!peer) {
                    throw RelayError(ErrorCode::Configuration, "Invalid monitoring peer");
                }
                CheckKeys(*peer, {"name", "host", "port"}, "monitoring.peers");
                MonitorPeer value{ReadString(*peer, "name", ""), ReadString(*peer, "host", "127.0.0.1"),
                                  static_cast<std::uint16_t>(ReadNumber(*peer, "port", 57231, 1, 65535))};
                if (!IsTunerName(value.name) || value.name == config.monitoring.name || !IsIpv4(value.host) ||
                    !names.insert(value.name).second || !addresses.emplace(value.host, value.port).second ||
                    (value.host == config.monitoring.bindAddress && value.port == config.monitoring.port)) {
                    throw RelayError(ErrorCode::Configuration, "Invalid/duplicate/self monitoring peer");
                }
                config.monitoring.peers.push_back(std::move(value));
            }
        }
        const auto& reload = GetTable(root, "reload");
        CheckKeys(reload, {"enabled", "interval_ms"}, "reload");
        config.reload.enabled = ReadBool(reload, "enabled", false);
        config.reload.intervalMs = ReadNumber(reload, "interval_ms", 1000, 250, 60000);
        return config;
    }

    const TunerDefinition& SelectTuner(const ServerConfig& config, const std::string& name)
    {
        if (name.empty() && config.tuners.size() == 1) {
            return config.tuners.front();
        }
        for (const auto& tuner : config.tuners) {
            if (tuner.name == name) {
                return tuner;
            }
        }
        throw RelayError(ErrorCode::Configuration,
                         name.empty() ? "Select a configured tuner using --tuner" : "Unknown tuner name: " + name);
    }
    ClientConfig LoadClientConfig(const std::filesystem::path& path)
    {
        if (!path.is_absolute()) {
            throw RelayError(ErrorCode::Configuration, "Client config path must be absolute");
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() < 0 || input.tellg() > 1'048'576) {
            throw RelayError(ErrorCode::Configuration, "Client config is missing or too large");
        }
        input.seekg(0);
        toml::table root;
        try {
            root = toml::parse(input, PathToUtf8(path));
        }
        catch (const toml::parse_error&) {
            throw RelayError(ErrorCode::Configuration, "Invalid client TOML");
        }
        CheckKeys(root, {"schema_version", "server", "client", "fallback_servers"}, "client root");
        if (!root.contains("schema_version")) {
            throw RelayError(ErrorCode::Configuration, "schema_version is required");
        }
        (void)ReadNumber(root, "schema_version", 1, 1, 1);
        ClientConfig config;
        const auto& server = GetTable(root, "server");
        const auto& client = GetTable(root, "client");
        CheckKeys(server, {"host", "port", "connect_timeout_ms", "request_timeout_ms"}, "server");
        CheckKeys(client,
                  {"tuner_group", "receive_queue_bytes", "reconnect", "priority", "exclusive", "selection_timeout_ms"},
                  "client");
        config.host = ReadString(server, "host", "127.0.0.1");
        if (!IsIpv4(config.host)) {
            throw RelayError(ErrorCode::Configuration, "Client host must be an IPv4 literal");
        }
        config.port = static_cast<std::uint16_t>(ReadNumber(server, "port", 57230, 1, 65535));
        config.connectTimeoutMs = ReadNumber(server, "connect_timeout_ms", 5000, 100, 120000);
        config.requestTimeoutMs = ReadNumber(server, "request_timeout_ms", 5000, 100, 120000);
        config.selectionTimeoutMs = ReadNumber(client, "selection_timeout_ms", 15000, 100, 120000);
        if (const auto* node = root.get("fallback_servers")) {
            const auto* servers = node->as_array();
            if (!servers || servers->size() > 7) {
                throw RelayError(ErrorCode::Configuration, "fallback_servers must contain at most 7 tables");
            }
            std::set<std::pair<std::string, std::uint16_t>> addresses{{config.host, config.port}};
            for (const auto& entry : *servers) {
                const auto* server_entry = entry.as_table();
                if (!server_entry) {
                    throw RelayError(ErrorCode::Configuration, "Invalid fallback server");
                }
                CheckKeys(*server_entry, {"host", "port"}, "fallback_servers");
                ClientEndpoint endpoint{ReadString(*server_entry, "host", "127.0.0.1"),
                                        static_cast<std::uint16_t>(ReadNumber(*server_entry, "port", 57230, 1, 65535))};
                if (!IsIpv4(endpoint.host) || !addresses.emplace(endpoint.host, endpoint.port).second) {
                    throw RelayError(ErrorCode::Configuration, "Invalid/duplicate fallback server");
                }
                config.fallbackServers.push_back(std::move(endpoint));
            }
        }
        config.queueBytes = ReadNumber(client, "receive_queue_bytes", 4'194'280, 188, 67'108'856);
        if (config.queueBytes % 188 != 0) {
            throw RelayError(ErrorCode::Configuration, "Client queue must be a multiple of 188");
        }
        config.group = ReadString(client, "tuner_group", "default");
        if (!IsTunerName(config.group)) {
            throw RelayError(ErrorCode::Configuration, "Invalid tuner_group");
        }
        if (ReadBool(client, "reconnect", false)) {
            throw RelayError(ErrorCode::NotImplemented,
                             "Automatic replay/reconnect is not supported; explicitly reopen the tuner");
        }
        config.exclusive = ReadBool(client, "exclusive", false);
        if (const auto* value = client.get("priority")) {
            const auto priority = value->value<std::int64_t>();
            if (!value->is_integer() || !priority || *priority < -128 || *priority > 127) {
                throw RelayError(ErrorCode::Configuration, "Priority must be -128..127");
            }
            config.priority = static_cast<int>(*priority);
        }
        return config;
    }
} // namespace bdr::core
