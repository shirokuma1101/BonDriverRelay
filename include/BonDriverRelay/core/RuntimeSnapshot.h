#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace bdr::core
{
    enum class TunerPhase : std::uint32_t
    {
        Idle,
        Tuning,
        Streaming,
        Fault,
        Stopped
    };
    struct TunerSnapshot
    {
        std::string name;
        TunerPhase phase = TunerPhase::Idle;
        std::uint32_t decoderState = 0;
        std::uint64_t receivedBytes = 0, outputBytes = 0, decoderFailures = 0, tuneFailures = 0;
        std::uint64_t subscribers = 0, queuedBytes = 0, droppedPackets = 0, cleanupFailures = 0;
    };
    struct SubscriberSnapshot
    {
        std::uint64_t id = 0, queuedBytes = 0, droppedPackets = 0;
        std::string group, tuner;
        bool exclusive = false, pending = false;
    };
    struct ManagerSnapshot
    {
        std::vector<TunerSnapshot> tuners;
        std::vector<SubscriberSnapshot> clients;
        std::uint64_t subscriptions = 0, queuedBytes = 0, droppedPackets = 0;
    };
    struct ServerSnapshot
    {
        ManagerSnapshot manager;
        std::uint64_t activeConnections = 0, acceptedConnections = 0, rejectedConnections = 0;
        std::uint64_t requests = 0, requestErrors = 0, sentTsBytes = 0;
    };
} // namespace bdr::core
