#pragma once
#include <windows.h>

// User settings from the Force Feedback page in the Game Controllers panel, stored in HKCU\Software\G25FF.
// Percentages are 0..100.
struct Settings {
    DWORD gain = 100;               // all effects
    DWORD springGain = 100;         // spring effects
    DWORD damperGain = 100;         // damper, inertia and friction effects
    DWORD range = 900;              // rotation range in degrees, 40..900 (multiple of 10)
    bool autocenter = true;         // the wheel's centering spring when no game uses force feedback
    DWORD autocenterStrength = 40;
    bool hardwareFriction = true;   // friction with the wheel's own friction effect (else played as damper)
    bool invert = false;            // reverse the direction of all forces

    static Settings Load();
    void Save() const;
};

constexpr wchar_t SettingsKey[] = L"Software\\G25FF";

// Exists while a game (or the page's test effects) drives the wheel; the autocenter spring stays off then and
// the session restores it when it ends
constexpr wchar_t SessionEventName[] = L"Local\\G25FF.ForceFeedbackSession";

// Rotation range and (unless a force feedback session runs) autocenter of every connected wheel in native mode
void ApplyWheelSettings(const Settings& settings);

// Switches every connected wheel in compatibility mode to native mode; returns how many
int SwitchWheelsToNative();
