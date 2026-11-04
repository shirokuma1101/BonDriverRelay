#include "BonDriverRelay/protocol/Control.h"
#include "BonDriverRelay/core/Error.h"
#include "BonDriverRelay/core/Paths.h"

namespace bdr::protocol
{
    namespace
    {
        void Invalid()
        {
            throw core::RelayError(core::ErrorCode::InvalidStream, "Invalid control payload");
        }
    } // namespace
    void Writer::U32(std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8) {
            data.push_back(static_cast<std::uint8_t>(value >> shift));
        }
        if (data.size() > CONTROL_PAYLOAD_LIMIT) {
            Invalid();
        }
    }
    void Writer::U64(std::uint64_t value)
    {
        U32(static_cast<std::uint32_t>(value >> 32));
        U32(static_cast<std::uint32_t>(value));
    }
    void Writer::Text(const std::string& value)
    {
        if (value.size() > 16'384 || value.find('\0') != std::string::npos) {
            Invalid();
        }
        if (!value.empty()) {
            (void)core::PathFromUtf8(value); // strict UTF-8 conversion
        }
        U32(static_cast<std::uint32_t>(value.size()));
        if (value.size() > CONTROL_PAYLOAD_LIMIT - data.size()) {
            Invalid();
        }
        data.insert(data.end(), value.begin(), value.end());
    }
    void Writer::Code(Opcode code)
    {
        const auto value = static_cast<std::uint16_t>(code);
        data = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value), 0, 0};
    }
    Reader::Reader(std::span<const std::uint8_t> data) : m_data(data) {}
    std::uint32_t Reader::U32()
    {
        if (m_data.size() < 4) {
            Invalid();
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value << 8) | m_data[static_cast<std::size_t>(i)];
        }
        m_data = m_data.subspan(4);
        return value;
    }
    std::uint64_t Reader::U64()
    {
        const auto high = U32();
        return (static_cast<std::uint64_t>(high) << 32) | U32();
    }
    std::string Reader::Text()
    {
        const auto length = U32();
        if (length > 16'384 || length > m_data.size()) {
            Invalid();
        }
        std::string text(m_data.begin(), m_data.begin() + length);
        m_data = m_data.subspan(length);
        if (text.find('\0') != std::string::npos) {
            Invalid();
        }
        if (!text.empty()) {
            (void)core::PathFromUtf8(text);
        }
        return text;
    }
    Opcode Reader::Code()
    {
        if (m_data.size() < 4 || m_data[2] != 0 || m_data[3] != 0) {
            Invalid();
        }
        const auto code = static_cast<Opcode>((static_cast<std::uint16_t>(m_data[0]) << 8) | m_data[1]);
        m_data = m_data.subspan(4);
        return code;
    }
    void Reader::End() const
    {
        if (!m_data.empty()) {
            Invalid();
        }
    }
    Bytes EncodeCatalog(const Catalog& catalog)
    {
        Writer out;
        out.U32(catalog.extended ? 1u : 0u);
        out.Text(catalog.tunerName);
        if (catalog.spaces.size() > 64 || catalog.channels.size() != catalog.spaces.size()) {
            Invalid();
        }
        out.U32(static_cast<std::uint32_t>(catalog.spaces.size()));
        for (std::size_t i = 0; i < catalog.spaces.size(); ++i) {
            out.Text(catalog.spaces[i]);
            if (catalog.channels[i].size() > 4096) {
                Invalid();
            }
            out.U32(static_cast<std::uint32_t>(catalog.channels[i].size()));
            for (const auto& name : catalog.channels[i]) {
                out.Text(name);
            }
        }
        if (out.data.size() > 65532) {
            Invalid();
        }
        return out.data;
    }
    Catalog DecodeCatalog(std::span<const std::uint8_t> data)
    {
        Reader in(data);
        Catalog catalog;
        const auto extended = in.U32();
        if (extended > 1) {
            Invalid();
        }
        catalog.extended = extended != 0;
        catalog.tunerName = in.Text();
        const auto count = in.U32();
        if (count > 64) {
            Invalid();
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            catalog.spaces.push_back(in.Text());
            const auto channels = in.U32();
            if (channels > 4096) {
                Invalid();
            }
            auto& names = catalog.channels.emplace_back();
            for (std::uint32_t n = 0; n < channels; ++n) {
                names.push_back(in.Text());
            }
        }
        in.End();
        return catalog;
    }
    void ValidateTs(std::span<const std::uint8_t> data)
    {
        if (data.empty() || data.size() > TS_PAYLOAD_LIMIT || data.size() % 188 != 0) {
            Invalid();
        }
        for (std::size_t i = 0; i < data.size(); i += 188) {
            if (data[i] != 0x47) {
                Invalid();
            }
        }
    }
} // namespace bdr::protocol
