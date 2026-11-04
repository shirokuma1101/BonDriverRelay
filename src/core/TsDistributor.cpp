#include "BonDriverRelay/core/TsDistributor.h"
#include "BonDriverRelay/core/Error.h"
#include <algorithm>
#include <chrono>

namespace bdr::core
{
    TsQueue::TsQueue(std::size_t capacity, bool drop_oldest) : m_capacity(capacity), m_dropOldest(drop_oldest)
    {
        if (capacity < 188 || capacity % 188 != 0) {
            throw RelayError(ErrorCode::InvalidArgument, "Queue capacity must be a positive multiple of 188");
        }
    }
    bool TsQueue::Push(std::shared_ptr<const protocol::Bytes> data)
    {
        if (!data) {
            throw RelayError(ErrorCode::InvalidArgument, "Null TS block");
        }
        protocol::ValidateTs(*data);
        std::lock_guard lock(m_mutex);
        if (m_error != protocol::Status::Ok) {
            return false;
        }
        if (data->size() > m_capacity || (!m_dropOldest && data->size() > m_capacity - m_bytes)) {
            m_droppedPackets += (m_bytes + data->size()) / 188;
            m_entries.clear();
            m_bytes = 0;
            m_error = protocol::Status::QueueSaturated;
            m_changed.notify_all();
            return false;
        }
        while (data->size() > m_capacity - m_bytes) {
            const auto remaining = m_entries.front().data->size() - m_entries.front().offset;
            m_droppedPackets += remaining / 188;
            m_bytes -= remaining;
            m_entries.pop_front();
        }
        m_bytes += data->size();
        m_entries.push_back({std::move(data), 0});
        m_changed.notify_all();
        return true;
    }
    protocol::Bytes TsQueue::Pop(std::size_t maximum)
    {
        std::lock_guard lock(m_mutex);
        if (m_entries.empty() || maximum < 188) {
            return {};
        }
        const auto limit = maximum / 188 * 188;
        protocol::Bytes result;
        result.reserve(std::min(limit, m_bytes));
        while (!m_entries.empty() && result.size() < limit) {
            auto& entry = m_entries.front();
            const auto count = std::min(entry.data->size() - entry.offset, limit - result.size());
            result.insert(result.end(), entry.data->begin() + static_cast<std::ptrdiff_t>(entry.offset),
                          entry.data->begin() + static_cast<std::ptrdiff_t>(entry.offset + count));
            entry.offset += count;
            m_bytes -= count;
            if (entry.offset == entry.data->size()) {
                m_entries.pop_front();
            }
        }
        return result;
    }
    void TsQueue::Clear()
    {
        std::lock_guard lock(m_mutex);
        m_entries.clear();
        m_bytes = 0;
    }
    void TsQueue::Close(protocol::Status reason)
    {
        std::lock_guard lock(m_mutex);
        if (m_error == protocol::Status::Ok) {
            m_error = reason;
        }
        m_entries.clear();
        m_bytes = 0;
        m_changed.notify_all();
    }
    void TsQueue::Reset()
    {
        std::lock_guard lock(m_mutex);
        m_entries.clear();
        m_bytes = 0;
        m_error = protocol::Status::Ok;
        m_changed.notify_all();
    }
    protocol::Status TsQueue::GetError() const
    {
        std::lock_guard lock(m_mutex);
        return m_error;
    }
    std::uint64_t TsQueue::GetDroppedPackets() const
    {
        std::lock_guard lock(m_mutex);
        return m_droppedPackets;
    }
    std::size_t TsQueue::GetBytes() const
    {
        std::lock_guard lock(m_mutex);
        return m_bytes;
    }
    std::uint32_t TsQueue::GetReadyCount() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<std::uint32_t>(m_entries.size());
    }
    std::uint32_t TsQueue::Wait(std::uint32_t timeout_ms)
    {
        std::unique_lock lock(m_mutex);
        const auto predicate = [&] { return !m_entries.empty() || m_error != protocol::Status::Ok; };
        if (timeout_ms == 0 || timeout_ms == 0xffffffff) {
            m_changed.wait(lock, predicate);
        }
        else if (!m_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate)) {
            return 258; // WAIT_TIMEOUT
        }
        return m_entries.empty() ? 128u : 0u; // WAIT_ABANDONED / WAIT_OBJECT_0
    }
} // namespace bdr::core
