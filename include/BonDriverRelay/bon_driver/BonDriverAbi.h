#pragma once

#include <Windows.h>

// Independently written interoperability declarations, not a vendored implementation.
// Declaration order, types and global class names match the reference ABI.
// Reference blobs in DBCTRADO/TVTest: 610d8c80b779acdd43391f0cacb9ac59975ee38f (v1),
// f6d6bf69aeb122cc23b2d7f7dad4240d224fcff5 (v2). See THIRD_PARTY_NOTICES.md.
// External ABI names are intentionally exempt from the project's naming rules.
class IBonDriver
{
  public:
    virtual const BOOL OpenTuner() = 0;
    virtual void CloseTuner() = 0;
    virtual const BOOL SetChannel(const BYTE channel) = 0;
    virtual const float GetSignalLevel() = 0;
    virtual const DWORD WaitTsStream(const DWORD timeout = 0) = 0;
    virtual const DWORD GetReadyCount() = 0;
    virtual const BOOL GetTsStream(BYTE* destination, DWORD* size, DWORD* remaining) = 0;
    virtual const BOOL GetTsStream(BYTE** destination, DWORD* size, DWORD* remaining) = 0;
    virtual void PurgeTsStream() = 0;
    virtual void Release() = 0;
};

class IBonDriver2 : public IBonDriver
{
  public:
    virtual LPCTSTR GetTunerName() = 0;
    virtual const BOOL IsTunerOpening() = 0;
    virtual LPCTSTR EnumTuningSpace(const DWORD space) = 0;
    virtual LPCTSTR EnumChannelName(const DWORD space, const DWORD channel) = 0;
    virtual const BOOL SetChannel(const DWORD space, const DWORD channel) = 0;
    virtual const DWORD GetCurSpace() = 0;
    virtual const DWORD GetCurChannel() = 0;
    virtual void Release() override = 0;
};

namespace bdr::bon_driver
{
    using CreateBonDriverFunction = IBonDriver*(__cdecl*)();
    static_assert(sizeof(BOOL) == 4 && sizeof(BYTE) == 1 && sizeof(WORD) == 2 && sizeof(DWORD) == 4);
    static_assert(sizeof(float) == 4 && sizeof(wchar_t) == 2);
} // namespace bdr::bon_driver
