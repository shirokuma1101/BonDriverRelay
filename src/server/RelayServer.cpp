#include "BonDriverRelay/server/RelayServer.h"
#include "BonDriverRelay/core/Error.h"
#include <algorithm>
#include <bit>
#include <iostream>

namespace bdr::server
{
    using protocol::MessageType;
    using protocol::Status;
    RelayServer::RelayServer(const core::ServerConfig& config, core::Logger* logger)
        : m_config(config), m_logger(logger)
    {}
    RelayServer::~RelayServer()
    {
        Stop();
    }
    void RelayServer::Start()
    {
        if (m_listener) {
            throw core::RelayError(core::ErrorCode::InvalidState, "Server is already started");
        }
        auto manager = std::make_shared<core::TunerManager>(m_config, m_logger);
        auto listener = std::make_shared<transport::Socket>();
        listener->Listen(m_config.network.bindAddress, m_config.network.port);
        m_manager.store(std::move(manager));
        m_listener = std::move(listener);
        m_accept = std::jthread([this](std::stop_token stop) { Accept(stop); });
    }
    std::uint16_t RelayServer::GetPort() const
    {
        return m_listener ? m_listener->GetPort() : 0;
    }
    void RelayServer::Stop()
    {
        m_accept.request_stop();
        if (m_listener) {
            m_listener->Shutdown();
        }
        if (m_accept.joinable()) {
            m_accept.join();
        }
        for (const auto& connection : m_connections) {
            connection->socket->Shutdown();
        }
        const auto manager = m_manager.load();
        if (manager) {
            manager->RequestStop();
        }
        m_connections.clear();
        m_listener.reset();
        if (manager) {
            manager->Stop();
            std::lock_guard lock(m_snapshotMutex);
            m_lastSnapshot = manager->GetSnapshot();
            m_manager.store(nullptr);
        }
    }
    core::ServerSnapshot RelayServer::GetSnapshot() const
    {
        core::ServerSnapshot snapshot;
        {
            std::lock_guard lock(m_snapshotMutex);
            const auto manager = m_manager.load();
            snapshot.manager = manager ? manager->GetSnapshot() : m_lastSnapshot;
        }
        snapshot.activeConnections = m_activeConnections.load();
        snapshot.acceptedConnections = m_acceptedConnections.load();
        snapshot.rejectedConnections = m_rejectedConnections.load();
        snapshot.requests = m_requests.load();
        snapshot.requestErrors = m_requestErrors.load();
        snapshot.sentTsBytes = m_sentTsBytes.load();
        return snapshot;
    }
    void RelayServer::LogDiagnostics() const
    {
        if (!m_logger) {
            return;
        }
        const auto snapshot = GetSnapshot();
        m_logger->Write(core::LogLevel::Info, "-", "status",
                        "connections=" + std::to_string(snapshot.activeConnections) +
                            " accepted=" + std::to_string(snapshot.acceptedConnections) +
                            " rejected=" + std::to_string(snapshot.rejectedConnections) +
                            " requests=" + std::to_string(snapshot.requests) +
                            " request_errors=" + std::to_string(snapshot.requestErrors) +
                            " sent_ts_bytes=" + std::to_string(snapshot.sentTsBytes) +
                            " subscriptions=" + std::to_string(snapshot.manager.subscriptions) +
                            " queued_bytes=" + std::to_string(snapshot.manager.queuedBytes) +
                            " dropped_packets=" + std::to_string(snapshot.manager.droppedPackets));
        for (const auto& tuner : snapshot.manager.tuners) {
            m_logger->Write(core::LogLevel::Info, tuner.name, "status",
                            "phase=" + std::to_string(static_cast<std::uint32_t>(tuner.phase)) +
                                " subscribers=" + std::to_string(tuner.subscribers) +
                                " received_bytes=" + std::to_string(tuner.receivedBytes) +
                                " output_bytes=" + std::to_string(tuner.outputBytes) +
                                " decoder_state=" + std::to_string(tuner.decoderState) +
                                " decoder_failures=" + std::to_string(tuner.decoderFailures) +
                                " tune_failures=" + std::to_string(tuner.tuneFailures) +
                                " cleanup_failures=" + std::to_string(tuner.cleanupFailures) +
                                " queued_bytes=" + std::to_string(tuner.queuedBytes));
        }
    }
    void RelayServer::Accept(std::stop_token stop)
    {
        std::uint64_t next_id = 1;
        while (!stop.stop_requested()) {
            try {
                std::erase_if(m_connections, [](const auto& connection) { return connection->done.load(); });
                auto socket = m_listener->Accept(transport::After(100));
                if (!socket) {
                    continue;
                }
                ++m_acceptedConnections;
                if (m_connections.size() >= m_config.network.maxClients) {
                    ++m_rejectedConnections;
                    socket->Shutdown();
                    continue;
                }
                auto connection = std::make_unique<Connection>();
                connection->socket = std::move(socket);
                auto* owned = connection.get();
                const auto id = next_id++;
                ++m_activeConnections;
                try {
                    connection->worker = std::jthread([this, owned, id] {
                        Serve(*owned, id);
                        --m_activeConnections;
                        owned->done.store(true);
                    });
                }
                catch (...) {
                    --m_activeConnections;
                    throw;
                }
                m_connections.push_back(std::move(connection));
            }
            catch (...) {
                if (!stop.stop_requested()) {
                    std::cerr << "ERROR operation=accept Socket failure\n";
                }
            }
        }
    }
    void RelayServer::Serve(Connection& connection, std::uint64_t id)
    {
        auto& socket = *connection.socket;
        const auto manager = m_manager.load();
        try {
            const auto deadline = transport::After(m_config.network.connectTimeoutMs);
            const auto hello = socket.ReceiveFrame(deadline);
            if (hello.header.type != MessageType::Hello) {
                throw core::RelayError(core::ErrorCode::InvalidStream, "Hello required");
            }
            protocol::Reader input(hello.payload);
            if (input.U32() != 0) {
                throw core::RelayError(core::ErrorCode::InvalidStream, "Unsupported capabilities");
            }
            const auto limit = input.U32();
            input.End();
            if (limit < 188 || limit > protocol::TS_PAYLOAD_LIMIT || limit % 188 != 0) {
                throw core::RelayError(core::ErrorCode::InvalidStream, "Invalid TS limit");
            }
            socket.SendFrame(MessageType::HelloAck, hello.header.requestId, hello.payload, deadline);
            int priority = 0;
            for (;;) {
                const auto request = socket.ReceiveFrame(transport::After(m_config.network.ioTimeoutMs));
                ++m_requests;
                if (request.header.type != MessageType::Request) {
                    throw core::RelayError(core::ErrorCode::InvalidStream, "Request required");
                }
                protocol::Reader reader(request.payload);
                const auto opcode = reader.Code();
                const protocol::Bytes body(request.payload.begin() + 4, request.payload.end());
                try {
                    auto reply = manager->Request(id, opcode, body, limit, priority);
                    if (opcode == protocol::Opcode::Open) {
                        protocol::Reader options(body);
                        (void)options.Text();
                        priority = std::bit_cast<std::int32_t>(options.U32());
                    }
                    protocol::Writer out;
                    out.Code(opcode);
                    out.data.insert(out.data.end(), reply.body.begin(), reply.body.end());
                    const auto send_deadline = transport::After(m_config.network.ioTimeoutMs);
                    if (!reply.ts.empty()) {
                        socket.SendFrame(MessageType::TsData, 0, reply.ts, send_deadline);
                        m_sentTsBytes.fetch_add(reply.ts.size());
                    }
                    socket.SendFrame(MessageType::Response, request.header.requestId, out.data, send_deadline);
                }
                catch (const std::exception& error) {
                    ++m_requestErrors;
                    Status status = Status::Unavailable;
                    std::uint32_t native_code = 0;
                    if (const auto* protocol_error = dynamic_cast<const core::ProtocolFailure*>(&error)) {
                        status = protocol_error->GetStatus();
                    }
                    else if (const auto* relay_error = dynamic_cast<const core::RelayError*>(&error)) {
                        native_code = relay_error->GetNativeCode();
                        if (relay_error->GetCode() == core::ErrorCode::InvalidStream) {
                            status = Status::InvalidFrame;
                        }
                    }
                    std::string message = error.what();
                    if (m_logger) {
                        m_logger->Write(core::LogLevel::Warn, "-", "request", message, native_code, std::to_string(id));
                    }
                    if (message.empty()) {
                        message = "Operation failed";
                    }
                    if (message.size() > 4096) {
                        message = "Operation failed (diagnostic exceeds wire limit; inspect server log)";
                    }
                    const protocol::Bytes bytes(message.begin(), message.end());
                    socket.SendFrame(MessageType::Error, request.header.requestId, bytes,
                                     transport::After(m_config.network.ioTimeoutMs), status);
                }
            }
        }
        catch (...) {
        }
        socket.Shutdown();
        manager->Disconnect(id);
    }
} // namespace bdr::server
