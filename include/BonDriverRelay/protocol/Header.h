#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bdr::protocol
{
    inline constexpr std::size_t HEADER_SIZE = 24;
    inline constexpr std::uint32_t CONTROL_PAYLOAD_LIMIT = 65'536;
    inline constexpr std::uint32_t TS_PACKET_SIZE = 188;
    inline constexpr std::uint32_t TS_PAYLOAD_LIMIT = 188 * 5'577;

    enum class MessageType : std::uint16_t
    {
        Hello = 1,
        HelloAck = 2,
        Error = 3,
        Request = 16,
        Response = 17,
        TsData = 256
    };
    enum class Status : std::uint32_t
    {
        Ok = 0,
        UnsupportedVersion = 1,
        InvalidFrame = 2,
        UnsupportedCommand = 3,
        NotImplemented = 4,
        InvalidState = 5,
        Unavailable = 6,
        TunerBusy = 7,
        DecoderFailure = 8,
        QueueSaturated = 9,
        Timeout = 10
    };

    struct Header
    {
        std::uint16_t major = 1;
        std::uint16_t minor = 0;
        MessageType type = MessageType::Hello;
        std::uint16_t flags = 0;
        std::uint32_t requestId = 1;
        std::uint32_t payloadLength = 8;
        Status status = Status::Ok;
        bool operator==(const Header&) const = default;
    };

    enum class HeaderError
    {
        None,
        WrongSize,
        BadMagic,
        UnsupportedVersion,
        UnknownType,
        InvalidFlags,
        InvalidRequestId,
        InvalidStatus,
        InvalidLength
    };
    struct HeaderResult
    {
        Header value{}; // Only usable when error == HeaderError::None.
        HeaderError error = HeaderError::None;
    };

    // Specification helpers only. They do not read sockets or validate payload content.
    [[nodiscard]] HeaderError ValidateHeader(const Header& header) noexcept;
    // Raw field encoding is intentional; callers must validate before sending.
    [[nodiscard]] std::array<std::uint8_t, HEADER_SIZE> EncodeHeader(const Header& header) noexcept;
    [[nodiscard]] HeaderResult DecodeHeader(std::span<const std::uint8_t> bytes) noexcept;
} // namespace bdr::protocol
