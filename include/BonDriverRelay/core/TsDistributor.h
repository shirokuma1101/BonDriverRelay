#pragma once
#include "BonDriverRelay/protocol/Control.h"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace bdr::core
{
    class TsQueue
    {
      public:
        TsQueue(std::size_t capacity, bool drop_oldest = false);
        bool Push(std::shared_ptr<const protocol::Bytes> data);
        protocol::Bytes Pop(std::size_t maximum);
        void Clear();
        void Close(protocol::Status reason);
        void Reset();
        protocol::Status GetError() const;
        std::uint64_t GetDroppedPackets() const;
        std::size_t GetBytes() const;
        std::uint32_t GetReadyCount() const;
        std::uint32_t Wait(std::uint32_t timeout_ms);

      private:
        struct Entry
        {
            std::shared_ptr<const protocol::Bytes> data;
            std::size_t offset = 0;
        };
        const std::size_t m_capacity;
        const bool m_dropOldest;
        mutable std::mutex m_mutex;
        std::condition_variable m_changed;
        std::deque<Entry> m_entries;
        std::size_t m_bytes = 0;
        std::uint64_t m_droppedPackets = 0;
        protocol::Status m_error = protocol::Status::Ok;
    };
} // namespace bdr::core
