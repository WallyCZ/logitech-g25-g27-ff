#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <dinputd.h>
#include <map>
#include <memory>
#include <vector>
#include "wheel.h"
#include "settings.h"

// Effect ids registered for DirectInput (DIEFFECTATTRIBUTES.dwEffectId); DirectInput passes them back to
// DownloadEffect. They are the HID PID usages of the effect types.
namespace effect {
constexpr DWORD ConstantForce = 0x26, Ramp = 0x27, Square = 0x30, Sine = 0x31, Triangle = 0x32, SawtoothUp = 0x33,
                SawtoothDown = 0x34, Spring = 0x40, Damper = 0x41, Inertia = 0x42, Friction = 0x43;
inline bool IsCondition(DWORD type) { return type >= Spring && type <= Friction; }
}

// One wheel as seen by one DirectInput effect driver instance.
//
// The wheel has four force slots but no effect timing of its own, so the driver plays the effects: an engine
// thread runs every 2 ms while effects play, works out start delays, durations, loops and envelopes, adds all
// constant, ramp and periodic effects up into the constant force slot and combines the conditions of each type
// into the spring, damper and friction slots. A slot is only written when its content changes.
class FFDevice {
public:
    FFDevice();
    ~FFDevice();

    HRESULT Open(const wchar_t* path);
    void Close();
    void GetVersions(LPDIDRIVERVERSIONS versions);
    HRESULT SetGain(DWORD gain);
    HRESULT SendCommand(DWORD command);
    HRESULT GetState(LPDIDEVICESTATE state);
    HRESULT Download(DWORD effectType, LPDWORD handle, LPCDIEFFECT effect, DWORD flags);
    HRESULT Destroy(DWORD handle);
    HRESULT Start(DWORD handle, DWORD mode, DWORD count);
    HRESULT Stop(DWORD handle);
    HRESULT GetStatus(DWORD handle, LPDWORD status);

private:
    struct Effect {
        DWORD handle = 0;
        DWORD type = 0;

        // DirectInput parameters, merged across partial updates
        DWORD duration = INFINITE, gain = DI_FFNOMINALMAX, startDelay = 0, coords = DIEFF_CARTESIAN;
        DWORD axisCount = 0, axes[2] = {};
        LONG direction[2] = {};
        bool hasEnvelope = false;
        DIENVELOPE envelope = {};
        std::vector<BYTE> params;

        // Playback
        bool started = false;
        double startMs = 0;   // engine clock when started
        DWORD loops = 1;      // INFINITE = until stopped
    };

    enum class Phase { Waiting, Playing, Done };

    HRESULT Merge(Effect& e, LPCDIEFFECT effect, DWORD flags);
    HRESULT Validate(const Effect& e) const;
    Effect* Find(DWORD handle);
    Phase PlayPhase(const Effect& e, double now, double* local) const;
    double EnvelopeLevel(const Effect& e, double local, double sustain) const;
    double AxisFactor(const Effect& e) const;
    const DICONDITION* WheelCondition(const Effect& e) const;
    LONG Gain(DWORD typeGain, const Effect& e) const;
    bool Update();  // computes and sends the slots; returns whether anything needs timing
    void SendSlot(wheel::Slot slot, const wheel::Command& command);
    void ResetSlots();
    void Wake() { SetEvent(wake_); }
    double Now() const;

    static DWORD WINAPI EngineThread(LPVOID self);
    void RunEngine();
    static DWORD WINAPI SettingsWatcher(LPVOID self);
    void WatchSettings();
    void StopThreads();

    CRITICAL_SECTION lock_;
    wheel::Device wheel_;
    HANDLE engine_ = nullptr, watcher_ = nullptr, stop_ = nullptr, wake_ = nullptr;
    HANDLE session_ = nullptr;  // marks a running force feedback session, see ApplyWheelSettings
    std::map<DWORD, std::unique_ptr<Effect>> effects_;
    wheel::Command sent_[wheel::SlotCount];  // last command written to each slot; empty = slot stopped
    DWORD nextHandle_ = 1;
    DWORD deviceGain_ = DI_FFNOMINALMAX;
    bool paused_ = false, actuators_ = true;
    double pausedAt_ = 0;
    LARGE_INTEGER frequency_ = {}, origin_ = {};
    Settings settings_;
};
