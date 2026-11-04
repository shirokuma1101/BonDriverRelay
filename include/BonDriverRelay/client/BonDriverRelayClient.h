#pragma once
#include "BonDriverRelay/client/RemoteClient.h"
#include "BonDriverRelay/bon_driver/BonDriverAbi.h"
#include "BonDriverRelay/client/Diagnostics.h"
#include "BonDriverRelay/core/TsDistributor.h"
#include <thread>

namespace bdr::client
{
    class BonDriverRelayClient final : public IBonDriver2
    {
      public:
        explicit BonDriverRelayClient(const core::ClientConfig& config);
        ~BonDriverRelayClient();
        const BOOL OpenTuner() override;
        void CloseTuner() override;
        const BOOL SetChannel(const BYTE channel) override;
        const float GetSignalLevel() override;
        const DWORD WaitTsStream(const DWORD timeout = 0) override;
        const DWORD GetReadyCount() override;
        const BOOL GetTsStream(BYTE* destination, DWORD* size, DWORD* remaining) override;
        const BOOL GetTsStream(BYTE** destination, DWORD* size, DWORD* remaining) override;
        void PurgeTsStream() override;
        void Release() override;
        LPCTSTR GetTunerName() override;
        const BOOL IsTunerOpening() override;
        LPCTSTR EnumTuningSpace(const DWORD space) override;
        LPCTSTR EnumChannelName(const DWORD space, const DWORD channel) override;
        const BOOL SetChannel(const DWORD space, const DWORD channel) override;
        const DWORD GetCurSpace() override;
        const DWORD GetCurChannel() override;
        RelayStatus GetDiagnostics() const;

      private:
        BOOL Tune(bool legacy, DWORD space, DWORD channel);
        BOOL ReadStream(BYTE* copy, BYTE** borrowed, DWORD* size, DWORD* remaining);
        void StartPump();
        void StopPump();
        void Close();
        void Error(const std::exception& error) noexcept;
        const core::ClientConfig m_config;
        mutable std::mutex m_apiMutex;
        core::TsQueue m_queue;
        std::unique_ptr<RemoteClient> m_remote;
        std::jthread m_pump;
        protocol::Catalog m_catalog;
        bool m_hasCatalog = false;
        std::vector<std::wstring> m_spaces;
        std::vector<std::vector<std::wstring>> m_channels;
        protocol::Bytes m_returnedTs;
        std::atomic<bool> m_open = false;
        std::atomic<DWORD> m_lastError = ERROR_SUCCESS;
        std::atomic<DWORD> m_space = 0xffffffff;
        std::atomic<DWORD> m_channel = 0xffffffff;
        std::atomic<float> m_signal = 0;
        std::atomic<DWORD> m_decoderState = 0;
        std::atomic<std::uint64_t> m_receivedBytes = 0;
        std::atomic<std::uint64_t> m_remoteDrops = 0;
        std::atomic<std::uint64_t> m_reconnects = 0;
    };
} // namespace bdr::client
