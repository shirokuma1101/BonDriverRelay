#include "BonDriverRelay/client/RemoteClient.h"
#include "BonDriverRelay/core/TunerManager.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/core/Paths.h"
#include <bit>
#include <algorithm>

namespace bdr::client
{
    RemoteClient::RemoteClient(const core::ClientConfig& config)
        : m_config(config), m_socket(std::make_shared<transport::Socket>())
    {
        m_endpoints.push_back({config.host, config.port});
        m_endpoints.insert(m_endpoints.end(), config.fallbackServers.begin(), config.fallbackServers.end());
    }
    protocol::Catalog RemoteClient::Open()
    {
        std::lock_guard lock(m_mutex);
        const auto deadline = transport::After(m_config.selectionTimeoutMs);
        std::exception_ptr failure;
        for (std::size_t index = 0; index < m_endpoints.size() && std::chrono::steady_clock::now() < deadline;
             ++index) {
            try {
                RemoteClient candidate(EndpointConfig(index));
                auto catalog = candidate.OpenSingle(deadline);
                if (m_catalog && catalog != *m_catalog) {
                    throw core::RelayError(core::ErrorCode::InvalidStream, "Server catalog mismatch");
                }
                m_socket->Shutdown();
                m_socket.swap(candidate.m_socket);
                m_nextId = candidate.m_nextId;
                m_alive.store(true);
                m_selected = index;
                m_catalog = catalog;
                return catalog;
            }
            catch (...) {
                failure = std::current_exception();
            }
        }
        m_alive.store(false);
        if (failure) {
            std::rethrow_exception(failure);
        }
        throw core::RelayError(core::ErrorCode::ReceiveTimeout, "Server selection deadline exceeded");
    }
    core::ClientConfig RemoteClient::EndpointConfig(std::size_t index) const
    {
        auto config = m_config;
        config.host = m_endpoints[index].host;
        config.port = m_endpoints[index].port;
        config.fallbackServers.clear();
        return config;
    }
    protocol::Catalog RemoteClient::OpenSingle(transport::Deadline selection_deadline)
    {
        const auto deadline = std::min(selection_deadline, transport::After(m_config.connectTimeoutMs));
        m_socket->Connect(m_config.host, m_config.port, deadline);
        protocol::Writer hello;
        hello.U32(0);
        hello.U32(protocol::TS_PAYLOAD_LIMIT);
        m_socket->SendFrame(protocol::MessageType::Hello, 1, hello.data, deadline);
        const auto ack = m_socket->ReceiveFrame(deadline);
        if (ack.header.type != protocol::MessageType::HelloAck || ack.header.requestId != 1 ||
            ack.payload != hello.data) {
            throw core::RelayError(core::ErrorCode::InvalidStream, "Invalid HelloAck");
        }
        m_alive.store(true);
        protocol::Writer out;
        out.Text(m_config.group);
        out.U32(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(m_config.priority)));
        out.U32(m_config.exclusive ? 1u : 0u);
        return protocol::DecodeCatalog(
            CallSingle(protocol::Opcode::Open, out.data,
                       std::min(selection_deadline, transport::After(m_config.requestTimeoutMs)))
                .body);
    }
    Reply RemoteClient::Call(protocol::Opcode opcode, const protocol::Bytes& body)
    {
        std::lock_guard lock(m_mutex);
        const auto selection_deadline = transport::After(m_config.selectionTimeoutMs);
        const auto deadline = opcode == protocol::Opcode::Tune && m_endpoints.size() > 1
                                  ? std::min(selection_deadline, transport::After(m_config.requestTimeoutMs))
                                  : transport::After(m_config.requestTimeoutMs);
        try {
            return CallSingle(opcode, body, deadline);
        }
        catch (const core::ProtocolFailure& error) {
            if (opcode == protocol::Opcode::Tune && error.GetStatus() == protocol::Status::TunerBusy &&
                m_endpoints.size() > 1) {
                return SelectAlternative(body, selection_deadline);
            }
            throw;
        }
    }
    Reply RemoteClient::SelectAlternative(const protocol::Bytes& body, transport::Deadline deadline)
    {
        for (std::size_t index = 0; index < m_endpoints.size() && std::chrono::steady_clock::now() < deadline;
             ++index) {
            if (index == m_selected) {
                continue;
            }
            try {
                RemoteClient candidate(EndpointConfig(index));
                const auto catalog = candidate.OpenSingle(deadline);
                if (!m_catalog || catalog != *m_catalog) {
                    continue;
                }
                auto reply = candidate.CallSingle(protocol::Opcode::Tune, body,
                                                  std::min(deadline, transport::After(m_config.requestTimeoutMs)));
                m_socket->Shutdown();
                m_socket.swap(candidate.m_socket);
                m_nextId = candidate.m_nextId;
                m_selected = index;
                OutputDebugStringW(L"BonDriverRelay: selected an alternative server for explicit Tune.\n");
                return reply;
            }
            catch (...) { /* Candidate connection destruction releases any temporary subscription. */
            }
        }
        throw core::ProtocolFailure(protocol::Status::TunerBusy, "No compatible server has an available tuner");
    }
    Reply RemoteClient::CallSingle(protocol::Opcode opcode, const protocol::Bytes& body, transport::Deadline deadline)
    {
        if (!m_alive.load()) {
            throw core::RelayError(core::ErrorCode::InvalidState, "Connection is closed");
        }
        protocol::Writer request;
        request.Code(opcode);
        request.data.insert(request.data.end(), body.begin(), body.end());
        if (++m_nextId == 0) {
            ++m_nextId;
        }
        const auto id = m_nextId;
        try {
            m_socket->SendFrame(protocol::MessageType::Request, id, request.data, deadline);
            Reply result;
            auto frame = m_socket->ReceiveFrame(deadline);
            if (frame.header.type == protocol::MessageType::TsData) {
                if (opcode != protocol::Opcode::PollTs) {
                    throw core::RelayError(core::ErrorCode::InvalidStream, "Unexpected TS");
                }
                protocol::ValidateTs(frame.payload);
                result.ts = std::move(frame.payload);
                frame = m_socket->ReceiveFrame(deadline);
            }
            if (frame.header.requestId != id) {
                throw core::RelayError(core::ErrorCode::InvalidStream, "Response ID mismatch");
            }
            if (frame.header.type == protocol::MessageType::Error) {
                const std::string message(frame.payload.begin(), frame.payload.end());
                (void)core::PathFromUtf8(message);
                throw core::ProtocolFailure(frame.header.status, message);
            }
            if (frame.header.type != protocol::MessageType::Response) {
                throw core::RelayError(core::ErrorCode::InvalidStream, "Unexpected response type");
            }
            protocol::Reader reader(frame.payload);
            if (reader.Code() != opcode) {
                throw core::RelayError(core::ErrorCode::InvalidStream, "Response opcode mismatch");
            }
            result.body.assign(frame.payload.begin() + 4, frame.payload.end());
            return result;
        }
        catch (const core::ProtocolFailure&) {
            throw;
        }
        catch (...) {
            Shutdown();
            throw;
        }
    }
    void RemoteClient::Shutdown() noexcept
    {
        m_alive.store(false);
        m_socket->Shutdown();
    }
    bool RemoteClient::IsAlive() const noexcept
    {
        return m_alive.load();
    }
} // namespace bdr::client
