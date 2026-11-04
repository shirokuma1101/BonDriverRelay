#pragma once
#include <Windows.h>
#include <cstdint>

namespace bdr::client
{
    struct RelayStatus
    {
        DWORD size = sizeof(RelayStatus);
        DWORD lastError = 0;
        DWORD decoderState = 0;
        DWORD readyBlocks = 0;
        std::uint64_t receivedBytes = 0;
        std::uint64_t droppedPackets = 0;
        std::uint64_t reconnects = 0;
    };
    static_assert(sizeof(RelayStatus) == 40);
} // namespace bdr::client
