#include "BonDriverRelay/protocol/Header.h"

namespace bdr::protocol
{
    namespace
    {
        void WriteUint16(std::uint8_t* dst, std::uint16_t value) noexcept
        {
            dst[0] = static_cast<std::uint8_t>(value >> 8);
            dst[1] = static_cast<std::uint8_t>(value);
        }
        void WriteUint32(std::uint8_t* dst, std::uint32_t value) noexcept
        {
            WriteUint16(dst, static_cast<std::uint16_t>(value >> 16));
            WriteUint16(dst + 2, static_cast<std::uint16_t>(value));
        }
        std::uint16_t ReadUint16(const std::uint8_t* src) noexcept
        {
            return static_cast<std::uint16_t>((static_cast<std::uint16_t>(src[0]) << 8) | src[1]);
        }
        std::uint32_t ReadUint32(const std::uint8_t* src) noexcept
        {
            return (static_cast<std::uint32_t>(ReadUint16(src)) << 16) | ReadUint16(src + 2);
        }
    } // namespace

    HeaderError ValidateHeader(const Header& header) noexcept
    {
        if (header.major != 1 || header.minor != 0) {
            return HeaderError::UnsupportedVersion;
        }
        switch (header.type) {
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::Error:
        case MessageType::TsData:
        case MessageType::Request:
        case MessageType::Response:
            break;
        default:
            return HeaderError::UnknownType;
        }
        if (header.flags != 0) {
            return HeaderError::InvalidFlags;
        }
        const bool is_ts_data = header.type == MessageType::TsData;
        if ((is_ts_data && header.requestId != 0) || (!is_ts_data && header.requestId == 0)) {
            return HeaderError::InvalidRequestId;
        }
        if (header.type == MessageType::Error) {
            if (header.status < Status::UnsupportedVersion || header.status > Status::Timeout) {
                return HeaderError::InvalidStatus;
            }
        }
        else if (header.status != Status::Ok) {
            return HeaderError::InvalidStatus;
        }
        // Check limits before any size addition or allocation (including on Win32).
        if (is_ts_data) {
            if (header.payloadLength == 0 || header.payloadLength > TS_PAYLOAD_LIMIT ||
                header.payloadLength % TS_PACKET_SIZE != 0) {
                return HeaderError::InvalidLength;
            }
        }
        else {
            if (header.payloadLength == 0 || header.payloadLength > CONTROL_PAYLOAD_LIMIT) {
                return HeaderError::InvalidLength;
            }
            if (header.type == MessageType::Error) {
                if (header.payloadLength > 4'096) {
                    return HeaderError::InvalidLength;
                }
            }
            else if (header.type == MessageType::Request || header.type == MessageType::Response) {
                if (header.payloadLength < 4) {
                    return HeaderError::InvalidLength;
                }
            }
            else if (header.payloadLength != 8) {
                return HeaderError::InvalidLength;
            }
        }
        return HeaderError::None;
    }

    std::array<std::uint8_t, HEADER_SIZE> EncodeHeader(const Header& header) noexcept
    {
        std::array<std::uint8_t, HEADER_SIZE> bytes{0x42, 0x44, 0x52, 0x4c};
        WriteUint16(bytes.data() + 4, header.major);
        WriteUint16(bytes.data() + 6, header.minor);
        WriteUint16(bytes.data() + 8, static_cast<std::uint16_t>(header.type));
        WriteUint16(bytes.data() + 10, header.flags);
        WriteUint32(bytes.data() + 12, header.requestId);
        WriteUint32(bytes.data() + 16, header.payloadLength);
        WriteUint32(bytes.data() + 20, static_cast<std::uint32_t>(header.status));
        return bytes;
    }

    HeaderResult DecodeHeader(std::span<const std::uint8_t> bytes) noexcept
    {
        if (bytes.size() != HEADER_SIZE) {
            return {{}, HeaderError::WrongSize};
        }
        if (bytes[0] != 0x42 || bytes[1] != 0x44 || bytes[2] != 0x52 || bytes[3] != 0x4c) {
            return {{}, HeaderError::BadMagic};
        }
        Header header{ReadUint16(bytes.data() + 4),
                      ReadUint16(bytes.data() + 6),
                      static_cast<MessageType>(ReadUint16(bytes.data() + 8)),
                      ReadUint16(bytes.data() + 10),
                      ReadUint32(bytes.data() + 12),
                      ReadUint32(bytes.data() + 16),
                      static_cast<Status>(ReadUint32(bytes.data() + 20))};
        return {header, ValidateHeader(header)};
    }
} // namespace bdr::protocol
