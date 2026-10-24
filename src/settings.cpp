#include "settings.h"
#include "wheel.h"
#include "log.h"
#include <algorithm>

namespace {

DWORD Read(const wchar_t* name, DWORD fallback, DWORD min, DWORD max)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, SettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return std::clamp(value, min, max);
}

void Write(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
}

}  // namespace

Settings Settings::Load()
{
    Settings s;
    s.gain = Read(L"Gain", s.gain, 0, 100);
    s.springGain = Read(L"SpringGain", s.springGain, 0, 100);
    s.damperGain = Read(L"DamperGain", s.damperGain, 0, 100);
    s.range = Read(L"Range", s.range, 40, 900);
    s.autocenter = Read(L"Autocenter", s.autocenter, 0, 1) != 0;
    s.autocenterStrength = Read(L"AutocenterStrength", s.autocenterStrength, 0, 100);
    s.hardwareFriction = Read(L"HardwareFriction", s.hardwareFriction, 0, 1) != 0;
    s.invert = Read(L"Invert", s.invert, 0, 1) != 0;
    return s;
}

void Settings::Save() const
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    Write(key, L"Gain", gain);
    Write(key, L"SpringGain", springGain);
    Write(key, L"DamperGain", damperGain);
    Write(key, L"Range", range);
    Write(key, L"Autocenter", autocenter);
    Write(key, L"AutocenterStrength", autocenterStrength);
    Write(key, L"HardwareFriction", hardwareFriction);
    Write(key, L"Invert", invert);
    RegCloseKey(key);
}

void ApplyWheelSettings(const Settings& settings)
{
    bool session = false;
    if (HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, SessionEventName)) {
        CloseHandle(event);
        session = true;
    }
    for (const auto& w : wheel::FindWheels(false)) {
        wheel::Device device;
        if (!device.Open(w.path.c_str()))
            continue;
        device.Send(wheel::SetRange((int)settings.range));
        if (!session) {
            wheel::Command enable;
            device.Send(wheel::Autocenter(settings.autocenter ? (int)settings.autocenterStrength : 0, &enable));
            if (!enable.empty())
                device.Send(enable);
        }
        Log("%ls settings: range %lu, autocenter %s", w.model->name, settings.range,
            session ? "left to the running game" : settings.autocenter ? "on" : "off");
    }
}

int SwitchWheelsToNative()
{
    int switched = 0;
    for (const auto& w : wheel::FindWheels(true)) {
        wheel::Device device;
        if (!device.Open(w.path.c_str()))
            continue;
        bool sent = true;
        for (const auto& command : w.model->nativeModeCommands)
            sent = sent && device.Send(command);
        if (sent) {
            Log("switched %ls to native mode: %ls", w.model->name, w.path.c_str());
            switched++;
        }
    }
    return switched;
}
