#pragma once
#include "BonDriverRelay/protocol/Header.h"
#include <string>
#include <vector>

namespace bdr::protocol
{
    using Bytes = std::vector<std::uint8_t>;
    enum class Opcode : std::uint16_t
    {
        Open = 1,
        Close = 2,
        Tune = 3,
        Purge = 4,
        PollTs = 5
    };
    struct Catalog
    {
        bool extended = false;
        std::string tunerName;
        std::vector<std::string> spaces;
        std::vector<std::vector<std::string>> channels;
        bool operator==(const Catalog&) const = default;
    };
    class Writer
    {
      public:
        Bytes data;
        void U32(std::uint32_t value);
        void U64(std::uint64_t value);
        void Text(const std::string& value);
        void Code(Opcode code);
    };
    class Reader
    {
      public:
        explicit Reader(std::span<const std::uint8_t> data);
        std::uint32_t U32();
        std::uint64_t U64();
        std::string Text();
        Opcode Code();
        void End() const;

      private:
        std::span<const std::uint8_t> m_data;
    };
    Bytes EncodeCatalog(const Catalog& catalog);
    Catalog DecodeCatalog(std::span<const std::uint8_t> data);
    void ValidateTs(std::span<const std::uint8_t> data);
} // namespace bdr::protocol
