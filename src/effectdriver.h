#pragma once
#include "ffdevice.h"

// {EC802B4D-2A2D-4CB0-9CDF-C6F48640435A}
DEFINE_GUID(CLSID_G25FF, 0xec802b4d, 0x2a2d, 0x4cb0, 0x9c, 0xdf, 0xc6, 0xf4, 0x86, 0x40, 0x43, 0x5a);

extern LONG g_objects;  // live COM objects and server locks, for DllCanUnloadNow

// DirectInput force feedback effect driver. DirectInput loads it through the OEMForceFeedback CLSID
// registered for the wheel and calls it with an external device id (one per attached device).
class EffectDriver : public IDirectInputEffectDriver {
public:
    EffectDriver();
    virtual ~EffectDriver();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDirectInputEffectDriver
    STDMETHODIMP DeviceID(DWORD directInputVersion, DWORD externalId, DWORD begin, DWORD internalId, LPVOID reserved) override;
    STDMETHODIMP GetVersions(LPDIDRIVERVERSIONS versions) override;
    STDMETHODIMP Escape(DWORD id, DWORD effect, LPDIEFFESCAPE escape) override;
    STDMETHODIMP SetGain(DWORD id, DWORD gain) override;
    STDMETHODIMP SendForceFeedbackCommand(DWORD id, DWORD command) override;
    STDMETHODIMP GetForceFeedbackState(DWORD id, LPDIDEVICESTATE state) override;
    STDMETHODIMP DownloadEffect(DWORD id, DWORD effectId, LPDWORD effect, LPCDIEFFECT params, DWORD flags) override;
    STDMETHODIMP DestroyEffect(DWORD id, DWORD effect) override;
    STDMETHODIMP StartEffect(DWORD id, DWORD effect, DWORD mode, DWORD count) override;
    STDMETHODIMP StopEffect(DWORD id, DWORD effect) override;
    STDMETHODIMP GetEffectStatus(DWORD id, DWORD effect, LPDWORD status) override;

private:
    static constexpr DWORD MaxDevices = 16;
    FFDevice* Device(DWORD id) { return id < MaxDevices ? devices_[id].get() : nullptr; }

    LONG refs_ = 1;
    CRITICAL_SECTION lock_;
    std::unique_ptr<FFDevice> devices_[MaxDevices];
};
