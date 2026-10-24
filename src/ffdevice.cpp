#include "ffdevice.h"
#include "devices.h"
#include "log.h"
#include <algorithm>
#include <math.h>

namespace {

constexpr LONG Nominal = DI_FFNOMINALMAX;  // 10000, DirectInput's full scale
constexpr double Pi = 3.14159265358979;
constexpr LONGLONG TickInterval = 2 * 10000;  // 2 ms in 100 ns units

class Lock {
public:
    explicit Lock(CRITICAL_SECTION& cs) : cs_(cs) { EnterCriticalSection(&cs_); }
    ~Lock() { LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION& cs_;
};

int AxisInstance(DWORD objectId) { return DIDFT_GETINSTANCE(objectId); }

int Round(double v) { return (int)lround(v); }

double Wave(DWORD type, double cycle)  // cycle 0..1 -> -1..1, all waves start rising from their middle
{
    switch (type) {
    case effect::Square:
        return cycle < 0.5 ? 1.0 : -1.0;
    case effect::Triangle:
        return cycle < 0.25 ? 4 * cycle : cycle < 0.75 ? 2 - 4 * cycle : 4 * cycle - 4;
    case effect::SawtoothUp:
        return 2 * cycle - 1;
    case effect::SawtoothDown:
        return 1 - 2 * cycle;
    default:
        return sin(2 * Pi * cycle);
    }
}

}  // namespace

FFDevice::FFDevice()
{
    InitializeCriticalSection(&lock_);
    QueryPerformanceFrequency(&frequency_);
    QueryPerformanceCounter(&origin_);
}

FFDevice::~FFDevice()
{
    Close();
    DeleteCriticalSection(&lock_);
}

double FFDevice::Now() const
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - origin_.QuadPart) * 1000.0 / (double)frequency_.QuadPart;
}

HRESULT FFDevice::Open(const wchar_t* path)
{
    Lock lock(lock_);
    if (!wheel_.Open(path))
        return DIERR_DEVICENOTREG;
    const WheelModel* model = ModelByNativePid(wheel_.ProductId());
    if (!model) {
        Log("not a supported wheel in native mode: %04X", wheel_.ProductId());
        wheel_.Close();
        return DIERR_DEVICENOTREG;
    }
    settings_ = Settings::Load();
    session_ = CreateEventW(nullptr, TRUE, FALSE, SessionEventName);
    // Games center the wheel with their own spring effects
    wheel_.Send(wheel::Autocenter(0, nullptr));
    wheel_.Send(wheel::SetRange((int)settings_.range));
    ResetSlots();

    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    engine_ = CreateThread(nullptr, 0, EngineThread, this, 0, nullptr);
    watcher_ = CreateThread(nullptr, 0, SettingsWatcher, this, 0, nullptr);
    Log("device opened: %ls revision %04X, range %lu", model->name, wheel_.Revision(), settings_.range);
    return DI_OK;
}

void FFDevice::Close()
{
    StopThreads();  // outside the lock: the threads take it
    Lock lock(lock_);
    if (!wheel_.IsOpen())
        return;
    effects_.clear();
    wheel_.Send(wheel::StopAll());
    // Back to the autocenter spring the user chose for when no game drives the wheel
    Settings s = Settings::Load();
    wheel::Command enable;
    wheel_.Send(wheel::Autocenter(s.autocenter ? (int)s.autocenterStrength : 0, &enable));
    if (!enable.empty())
        wheel_.Send(enable);
    wheel_.Close();
    if (session_) {
        CloseHandle(session_);
        session_ = nullptr;
    }
    Log("device closed");
}

void FFDevice::StopThreads()
{
    if (stop_)
        SetEvent(stop_);
    for (HANDLE* thread : { &engine_, &watcher_ })
        if (*thread) {
            WaitForSingleObject(*thread, 2000);
            CloseHandle(*thread);
            *thread = nullptr;
        }
    for (HANDLE* event : { &stop_, &wake_ })
        if (*event) {
            CloseHandle(*event);
            *event = nullptr;
        }
}

DWORD WINAPI FFDevice::EngineThread(LPVOID self)
{
    static_cast<FFDevice*>(self)->RunEngine();
    return 0;
}

// Recomputes the slots every 2 ms while effects play (or wait for their start delay); idle otherwise until a
// DirectInput call wakes it
void FFDevice::RunEngine()
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    HANDLE waits[3] = { stop_, wake_, timer };
    for (;;) {
        bool active;
        {
            Lock lock(lock_);
            active = Update();
        }
        if (active) {
            LARGE_INTEGER due;
            due.QuadPart = -TickInterval;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        }
        if (WaitForMultipleObjects(active ? 3 : 2, waits, FALSE, INFINITE) == WAIT_OBJECT_0)
            break;
    }
    CloseHandle(timer);
}

DWORD WINAPI FFDevice::SettingsWatcher(LPVOID self)
{
    static_cast<FFDevice*>(self)->WatchSettings();
    return 0;
}

// Applies changes from the Force Feedback page right away, also in a running game
void FFDevice::WatchSettings()
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, nullptr, 0, KEY_NOTIFY | KEY_READ, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE waits[2] = { stop_, changed };
    while (RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE) == ERROR_SUCCESS &&
           WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        Sleep(100);  // the page writes several values in a row
        Lock lock(lock_);
        DWORD oldRange = settings_.range;
        settings_ = Settings::Load();
        if (settings_.range != oldRange)
            wheel_.Send(wheel::SetRange((int)settings_.range));
        Update();
        Log("settings changed: gain %lu, springs %lu, dampers %lu, range %lu, invert %d", settings_.gain,
            settings_.springGain, settings_.damperGain, settings_.range, settings_.invert);
    }
    CloseHandle(changed);
    RegCloseKey(key);
}

void FFDevice::GetVersions(LPDIDRIVERVERSIONS versions)
{
    Lock lock(lock_);
    versions->dwFirmwareRevision = wheel_.Revision();
    versions->dwHardwareRevision = wheel_.Revision();
    versions->dwFFDriverVersion = 0x00010000;
}

HRESULT FFDevice::SetGain(DWORD gain)
{
    Lock lock(lock_);
    deviceGain_ = std::min<DWORD>(gain, Nominal);
    Update();
    return DI_OK;
}

HRESULT FFDevice::SendCommand(DWORD command)
{
    Lock lock(lock_);
    Log("command %08lX", command);
    switch (command) {
    case DISFFC_RESET:
        effects_.clear();
        paused_ = false;
        actuators_ = true;
        wheel_.Send(wheel::Autocenter(0, nullptr));
        settings_ = Settings::Load();
        ResetSlots();
        return DI_OK;
    case DISFFC_STOPALL:
        for (auto& [handle, e] : effects_)
            e->started = false;
        Update();
        return DI_OK;
    case DISFFC_PAUSE:
        if (!paused_) {
            paused_ = true;
            pausedAt_ = Now();
            ResetSlots();
        }
        return DI_OK;
    case DISFFC_CONTINUE:
        if (paused_) {
            // Effects continue where they were paused
            double pause = Now() - pausedAt_;
            for (auto& [handle, e] : effects_)
                e->startMs += pause;
            paused_ = false;
            Update();
            Wake();
        }
        return DI_OK;
    case DISFFC_SETACTUATORSON:
        actuators_ = true;
        Update();
        Wake();
        return DI_OK;
    case DISFFC_SETACTUATORSOFF:
        actuators_ = false;
        ResetSlots();
        return DI_OK;
    }
    return DIERR_INVALIDPARAM;
}

HRESULT FFDevice::GetState(LPDIDEVICESTATE state)
{
    Lock lock(lock_);
    if (state->dwSize < sizeof(DIDEVICESTATE))
        return DIERR_INVALIDPARAM;
    double now = Now();
    bool playing = false;
    for (auto& [handle, e] : effects_)
        playing = playing || PlayPhase(*e, now, nullptr) != Phase::Done;
    DWORD s = DIGFFS_USERFFSWITCHON | DIGFFS_POWERON | DIGFFS_SAFETYSWITCHON;
    if (effects_.empty())
        s |= DIGFFS_EMPTY;
    if (paused_)
        s |= DIGFFS_PAUSED;
    else if (!playing)
        s |= DIGFFS_STOPPED;
    s |= actuators_ ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF;
    state->dwState = s;
    state->dwLoad = 0;  // effects live in the driver, there is no device memory to fill
    return DI_OK;
}

FFDevice::Effect* FFDevice::Find(DWORD handle)
{
    auto it = effects_.find(handle);
    return it == effects_.end() ? nullptr : it->second.get();
}

HRESULT FFDevice::Merge(Effect& e, LPCDIEFFECT p, DWORD flags)
{
    if (flags & DIEP_DURATION)
        e.duration = p->dwDuration;
    if (flags & DIEP_GAIN)
        e.gain = std::min<DWORD>(p->dwGain, Nominal);
    if (flags & DIEP_AXES) {
        if (!p->cAxes || p->cAxes > 2 || !p->rgdwAxes)
            return DIERR_INVALIDPARAM;
        e.axisCount = p->cAxes;
        for (DWORD i = 0; i < p->cAxes; i++)
            e.axes[i] = p->rgdwAxes[i];
    }
    if (flags & DIEP_DIRECTION) {
        if (!p->rglDirection || !p->cAxes)
            return DIERR_INVALIDPARAM;
        e.coords = p->dwFlags & (DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL);
        for (DWORD i = 0; i < std::min<DWORD>(p->cAxes, 2); i++)
            e.direction[i] = p->rglDirection[i];
    }
    if (flags & DIEP_ENVELOPE) {
        e.hasEnvelope = p->lpEnvelope != nullptr;
        if (e.hasEnvelope)
            e.envelope = *p->lpEnvelope;
    }
    if (flags & DIEP_TYPESPECIFICPARAMS) {
        if (!p->lpvTypeSpecificParams)
            return DIERR_INVALIDPARAM;
        const BYTE* params = static_cast<const BYTE*>(p->lpvTypeSpecificParams);
        e.params.assign(params, params + p->cbTypeSpecificParams);
    }
    if ((flags & DIEP_STARTDELAY) && p->dwSize >= sizeof(DIEFFECT))
        e.startDelay = p->dwStartDelay;
    return DI_OK;
}

HRESULT FFDevice::Validate(const Effect& e) const
{
    if (!e.axisCount)
        return DIERR_INVALIDPARAM;
    size_t size = e.params.size();
    switch (e.type) {
    case effect::ConstantForce:
        return size == sizeof(DICONSTANTFORCE) ? DI_OK : DIERR_INVALIDPARAM;
    case effect::Ramp:
        return size == sizeof(DIRAMPFORCE) ? DI_OK : DIERR_INVALIDPARAM;
    case effect::Square:
    case effect::Sine:
    case effect::Triangle:
    case effect::SawtoothUp:
    case effect::SawtoothDown:
        return size == sizeof(DIPERIODIC) ? DI_OK : DIERR_INVALIDPARAM;
    case effect::Spring:
    case effect::Damper:
    case effect::Inertia:
    case effect::Friction:
        return size == sizeof(DICONDITION) || size == 2 * sizeof(DICONDITION) ? DI_OK : DIERR_INVALIDPARAM;
    }
    return DIERR_UNSUPPORTED;
}

HRESULT FFDevice::Download(DWORD effectType, LPDWORD handle, LPCDIEFFECT effect, DWORD flags)
{
    Lock lock(lock_);
    if (!wheel_.IsOpen())
        return DIERR_NOTACQUIRED;
    Effect* existing = *handle ? Find(*handle) : nullptr;
    if (*handle && !existing)
        return DIERR_INVALIDPARAM;

    // Work on a copy so a rejected update leaves the effect untouched
    Effect updated = existing ? *existing : Effect{};
    updated.type = effectType;
    HRESULT hr = Merge(updated, effect, flags);
    if (SUCCEEDED(hr))
        hr = Validate(updated);
    if (FAILED(hr) || (flags & DIEP_NODOWNLOAD))
        return hr;

    Effect* e;
    if (existing) {
        *existing = std::move(updated);
        e = existing;
    } else {
        updated.handle = nextHandle_++;
        auto owned = std::make_unique<Effect>(std::move(updated));
        e = owned.get();
        effects_[e->handle] = std::move(owned);
        Log("new effect %lu type %02X, %lu axes", e->handle, e->type, e->axisCount);
    }
    *handle = e->handle;
    if (flags & DIEP_START) {
        e->started = true;
        e->startMs = Now();
        e->loops = 1;
    }
    Update();
    Wake();
    return DI_OK;
}

HRESULT FFDevice::Destroy(DWORD handle)
{
    Lock lock(lock_);
    if (!effects_.erase(handle))
        return DIERR_INVALIDPARAM;
    Update();
    return DI_OK;
}

HRESULT FFDevice::Start(DWORD handle, DWORD mode, DWORD count)
{
    Lock lock(lock_);
    Effect* e = Find(handle);
    if (!e)
        return DIERR_INVALIDPARAM;
    if (mode & DIES_SOLO)
        for (auto& [h, other] : effects_)
            other->started = false;
    e->started = true;
    e->startMs = Now();
    e->loops = count ? count : 1;
    Update();
    Wake();
    return DI_OK;
}

HRESULT FFDevice::Stop(DWORD handle)
{
    Lock lock(lock_);
    Effect* e = Find(handle);
    if (!e)
        return DIERR_INVALIDPARAM;
    e->started = false;
    Update();
    return DI_OK;
}

HRESULT FFDevice::GetStatus(DWORD handle, LPDWORD status)
{
    Lock lock(lock_);
    Effect* e = Find(handle);
    if (!e)
        return DIERR_INVALIDPARAM;
    *status = PlayPhase(*e, Now(), nullptr) != Phase::Done ? DIEGES_PLAYING : 0;
    return DI_OK;
}

FFDevice::Phase FFDevice::PlayPhase(const Effect& e, double now, double* local) const
{
    if (!e.started)
        return Phase::Done;
    double t = (paused_ ? pausedAt_ : now) - e.startMs - e.startDelay / 1000.0;
    if (t < 0)
        return Phase::Waiting;
    if (e.duration == INFINITE) {
        if (local)
            *local = t;
        return Phase::Playing;
    }
    double duration = e.duration / 1000.0;
    if (duration <= 0)
        return Phase::Done;
    double cycle = floor(t / duration);
    if (e.loops != INFINITE && cycle >= e.loops)
        return Phase::Done;
    if (local)
        *local = t - cycle * duration;
    return Phase::Playing;
}

// Magnitude with the envelope applied: attack from the attack level to the sustain level, fade from it to
// the fade level at the end of the duration
double FFDevice::EnvelopeLevel(const Effect& e, double local, double sustain) const
{
    if (!e.hasEnvelope)
        return sustain;
    double level = sustain;
    double attack = e.envelope.dwAttackTime / 1000.0, fade = e.envelope.dwFadeTime / 1000.0;
    if (attack > 0 && local < attack)
        level = e.envelope.dwAttackLevel + (sustain - e.envelope.dwAttackLevel) * local / attack;
    if (fade > 0 && e.duration != INFINITE) {
        double remaining = e.duration / 1000.0 - local;
        if (remaining < fade)
            level = e.envelope.dwFadeLevel + (sustain - e.envelope.dwFadeLevel) * std::max(remaining, 0.0) / fade;
    }
    return level;
}

// The part of the effect's direction along the wheel axis, -1..1. The wheel is the device's only force
// feedback axis: a one axis effect acts on it whatever axis it names, a two axis effect through its X part.
double FFDevice::AxisFactor(const Effect& e) const
{
    if (e.axisCount == 1)
        return (e.coords & DIEFF_CARTESIAN) && e.direction[0] < 0 ? -1.0 : 1.0;
    int x = AxisInstance(e.axes[0]) != 0 && AxisInstance(e.axes[1]) == 0 ? 1 : 0;
    if (e.coords & DIEFF_CARTESIAN) {
        double length = hypot((double)e.direction[0], (double)e.direction[1]);
        return length > 0 ? e.direction[x] / length : 0.0;
    }
    double angle = e.direction[0] / 100.0 * Pi / 180.0;
    if (e.coords & DIEFF_SPHERICAL)  // from the +X axis towards +Y
        return x == 0 ? cos(angle) : sin(angle);
    return x == 0 ? sin(angle) : -cos(angle);  // polar: from north (-Y), clockwise
}

const DICONDITION* FFDevice::WheelCondition(const Effect& e) const
{
    const DICONDITION* c = reinterpret_cast<const DICONDITION*>(e.params.data());
    if (e.params.size() < 2 * sizeof(DICONDITION) || e.axisCount < 2)
        return c;
    return AxisInstance(e.axes[0]) != 0 && AxisInstance(e.axes[1]) == 0 ? c + 1 : c;
}

// Effect gain x device gain x the strength settings, in DirectInput units (0..10000)
LONG FFDevice::Gain(DWORD typeGain, const Effect& e) const
{
    double g = (double)e.gain * deviceGain_ / Nominal * settings_.gain / 100.0 * typeGain / 100.0;
    return (LONG)g;
}

bool FFDevice::Update()
{
    if (!wheel_.IsOpen() || paused_ || !actuators_)
        return false;
    double now = Now();
    bool active = false;
    double force = 0;  // towards the direction the effects come from, DirectInput units
    struct Condition {
        bool any = false;
        double negative = 0, positive = 0;  // coefficients, gain applied
        LONG offset = 0, deadBand = 0, strongest = -1;
        DWORD saturation = 0;
    } spring, damper, friction;

    for (auto& [handle, owned] : effects_) {
        Effect& e = *owned;
        double local = 0;
        Phase phase = PlayPhase(e, now, &local);
        if (phase == Phase::Done) {
            e.started = false;
            continue;
        }
        active = true;
        if (phase == Phase::Waiting)
            continue;

        if (effect::IsCondition(e.type)) {
            Condition& c = e.type == effect::Spring ? spring
                         : e.type == effect::Friction && settings_.hardwareFriction ? friction : damper;
            const DICONDITION* given = WheelCondition(e);
            double g = Gain(e.type == effect::Spring ? settings_.springGain : settings_.damperGain, e) / (double)Nominal;
            c.any = true;
            c.negative += given->lNegativeCoefficient * g;
            c.positive += given->lPositiveCoefficient * g;
            LONG strength = labs(given->lNegativeCoefficient) + labs(given->lPositiveCoefficient);
            if (strength > c.strongest) {
                c.strongest = strength;
                c.offset = given->lOffset;
                c.deadBand = given->lDeadBand;
            }
            DWORD saturation = std::max(given->dwPositiveSaturation, given->dwNegativeSaturation);
            c.saturation = std::max<DWORD>(c.saturation, saturation ? saturation : Nominal);
            continue;
        }

        double value;
        if (e.type == effect::ConstantForce) {
            LONG m = reinterpret_cast<const DICONSTANTFORCE*>(e.params.data())->lMagnitude;
            value = (m < 0 ? -1 : 1) * EnvelopeLevel(e, local, labs(m));
        } else if (e.type == effect::Ramp) {
            const DIRAMPFORCE* ramp = reinterpret_cast<const DIRAMPFORCE*>(e.params.data());
            double v = e.duration == INFINITE || !e.duration
                     ? ramp->lStart : ramp->lStart + (double)(ramp->lEnd - ramp->lStart) * local * 1000.0 / e.duration;
            value = (v < 0 ? -1 : 1) * EnvelopeLevel(e, local, fabs(v));
        } else {
            const DIPERIODIC* p = reinterpret_cast<const DIPERIODIC*>(e.params.data());
            double period = p->dwPeriod / 1000.0;
            double cycle = (period > 0 ? local / period : 0) + (p->dwPhase % 36000) / 36000.0;
            cycle -= floor(cycle);
            value = p->lOffset + EnvelopeLevel(e, local, std::min<DWORD>(p->dwMagnitude, Nominal)) * Wave(e.type, cycle);
        }
        force += value * Gain(100, e) / Nominal * AxisFactor(e);
    }

    // Constant force slot. DirectInput directions say where a force comes from, so it pushes the other way;
    // levels below 0x80 turn the wheel right.
    double right = std::clamp(-force, (double)-Nominal, (double)Nominal);
    if (settings_.invert)
        right = -right;
    SendSlot(wheel::SlotConstant, wheel::Constant((BYTE)(wheel::NoForce - Round(right * 127 / Nominal))));

    auto coefficient = [](double k, int max) { return std::clamp(Round(k * max / Nominal), -max, max); };
    auto clip = [](DWORD saturation) { return (int)(std::min<DWORD>(saturation, Nominal) * 255 / Nominal); };

    if (spring.any) {
        wheel::SpringParams p;
        int center = 1024 + Round(std::clamp(spring.offset, -Nominal, Nominal) * 1023.0 / Nominal);
        int band = Round(std::clamp(spring.deadBand, 0L, Nominal) * 1023.0 / Nominal);
        p.center1 = std::max(center - band, 0);
        p.center2 = std::min(center + band, 2047);
        p.k1 = coefficient(spring.negative, 15);
        p.k2 = coefficient(spring.positive, 15);
        p.clip = clip(spring.saturation);
        SendSlot(wheel::SlotSpring, wheel::Spring(p));
    } else {
        SendSlot(wheel::SlotSpring, wheel::StopSlot(wheel::SlotSpring));
    }
    if (damper.any) {
        wheel::DamperParams p;
        p.k1 = coefficient(damper.negative, 15);
        p.k2 = coefficient(damper.positive, 15);
        p.clip = clip(damper.saturation);
        SendSlot(wheel::SlotDamper, wheel::Damper(p));
    } else {
        SendSlot(wheel::SlotDamper, wheel::StopSlot(wheel::SlotDamper));
    }
    if (friction.any) {
        wheel::FrictionParams p;
        p.k1 = coefficient(friction.negative, 255);
        p.k2 = coefficient(friction.positive, 255);
        p.clip = clip(friction.saturation);
        SendSlot(wheel::SlotFriction, wheel::Friction(p));
    } else {
        SendSlot(wheel::SlotFriction, wheel::StopSlot(wheel::SlotFriction));
    }
    return active;
}

void FFDevice::SendSlot(wheel::Slot slot, const wheel::Command& command)
{
    if (sent_[slot] == command)
        return;
    if (wheel_.Send(command))
        sent_[slot] = command;
}

// Stops all forces; the next update sends whatever plays again
void FFDevice::ResetSlots()
{
    wheel_.Send(wheel::StopAll());
    sent_[wheel::SlotConstant] = wheel::Constant(wheel::NoForce);
    for (int slot = wheel::SlotSpring; slot < wheel::SlotCount; slot++)
        sent_[slot] = wheel::StopSlot((wheel::Slot)slot);
}
