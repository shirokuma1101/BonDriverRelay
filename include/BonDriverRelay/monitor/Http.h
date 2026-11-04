#pragma once
#include "BonDriverRelay/transport/Tcp.h"
#include <map>
#include <string>
namespace bdr::monitor
{
    constexpr std::size_t HTTP_HEADER_LIMIT = 8192;
    constexpr std::size_t PEER_BODY_LIMIT = 262144;
    struct HttpHead
    {
        std::string firstLine;
        std::map<std::string, std::string> headers;
        std::string bodyPrefix;
    };
    HttpHead ReadHttpHead(const transport::Socket& socket, transport::Deadline deadline);
    void SendHttp(const transport::Socket& socket, int code, std::string_view type, std::string_view body,
                  transport::Deadline deadline);
    std::string FetchLocalStatus(const std::string& host, std::uint16_t port, std::uint32_t timeout_ms);
} // namespace bdr::monitor
