#include "BonDriverRelay/monitor/Http.h"
#include "BonDriverRelay/core/Error.h"
#include <algorithm>
#include <array>
#include <charconv>
namespace bdr::monitor
{
    namespace
    {
        [[noreturn]] void InvalidHttp()
        {
            throw core::RelayError(core::ErrorCode::InvalidStream, "Invalid HTTP message");
        }
        std::span<const std::uint8_t> Bytes(std::string_view text)
        {
            return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
        }
    } // namespace
    HttpHead ReadHttpHead(const transport::Socket& socket, transport::Deadline deadline)
    {
        std::string input;
        std::array<std::uint8_t, 2048> buffer{};
        std::size_t end;
        while ((end = input.find("\r\n\r\n")) == std::string::npos) {
            if (input.size() >= HTTP_HEADER_LIMIT) {
                InvalidHttp();
            }
            const auto count = socket.ReceiveSome(
                std::span(buffer).first(std::min(buffer.size(), HTTP_HEADER_LIMIT - input.size())), deadline);
            if (count == 0) {
                InvalidHttp();
            }
            input.append(reinterpret_cast<const char*>(buffer.data()), count);
        }
        HttpHead head;
        head.bodyPrefix = input.substr(end + 4);
        input.resize(end + 2);
        const auto line_end = input.find("\r\n");
        head.firstLine = input.substr(0, line_end);
        for (std::size_t offset = line_end + 2; offset < input.size();) {
            const auto next = input.find("\r\n", offset);
            if (next == std::string::npos) {
                InvalidHttp();
            }
            const auto line = input.substr(offset, next - offset);
            const auto colon = line.find(':');
            if (colon == 0 || colon == std::string::npos) {
                InvalidHttp();
            }
            auto key = line.substr(0, colon);
            for (auto& ch : key) {
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                      std::string_view("!#$%&'*+-.^_`|~").find(ch) != std::string_view::npos)) {
                    InvalidHttp();
                }
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            auto value = line.substr(colon + 1);
            const auto start = value.find_first_not_of(" \t");
            value = start == std::string::npos ? "" : value.substr(start);
            const auto finish = value.find_last_not_of(" \t");
            if (finish != std::string::npos) {
                value.resize(finish + 1);
            }
            if (std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }) ||
                !head.headers.emplace(key, value).second) {
                InvalidHttp();
            }
            offset = next + 2;
        }
        return head;
    }
    void SendHttp(const transport::Socket& socket, int code, std::string_view type, std::string_view body,
                  transport::Deadline deadline)
    {
        const std::string reason =
            code == 200 ? "OK" : (code == 404 ? "Not Found" : (code == 405 ? "Method Not Allowed" : "Bad Request"));
        const auto header =
            "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + std::string(type) +
            "\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nAllow: GET\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
            "Content-Security-Policy: default-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n\r\n";
        socket.Send(Bytes(header), deadline);
        socket.Send(Bytes(body), deadline);
    }
    std::string FetchLocalStatus(const std::string& host, std::uint16_t port, std::uint32_t timeout_ms)
    {
        transport::Socket socket;
        const auto deadline = transport::After(timeout_ms);
        socket.Connect(host, port, deadline);
        socket.Send(Bytes("GET /api/local HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                          "\r\nConnection: close\r\n\r\n"),
                    deadline);
        auto head = ReadHttpHead(socket, deadline);
        if (head.firstLine != "HTTP/1.1 200 OK" || head.headers.contains("transfer-encoding") ||
            !head.headers.contains("content-length") || !head.headers.contains("content-type") ||
            head.headers.at("content-type").find("application/json") != 0) {
            InvalidHttp();
        }
        std::size_t size = 0;
        const auto& length = head.headers.at("content-length");
        const auto parsed = std::from_chars(length.data(), length.data() + length.size(), size);
        if (parsed.ec != std::errc{} || parsed.ptr != length.data() + length.size() || size > PEER_BODY_LIMIT ||
            size < head.bodyPrefix.size()) {
            InvalidHttp();
        }
        auto body = std::move(head.bodyPrefix);
        const auto offset = body.size();
        body.resize(size);
        if (size > offset) {
            socket.Receive({reinterpret_cast<std::uint8_t*>(body.data() + offset), size - offset}, deadline);
        }
        if (body.empty() || body.front() != '{' || body.back() != '}') {
            InvalidHttp();
        }
        return body;
    }
} // namespace bdr::monitor
