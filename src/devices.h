#pragma once
#include <windows.h>
#include <vector>

// Logitech wheels driven by this driver. They all speak the same "classic" force feedback protocol (see wheel.h)
// and start in a compatibility mode: they act as a Driving Force EX (USB product id C294, 200 degrees, combined
// pedals) until they get a command that switches them to their native mode, with its own product id. In
// compatibility mode the USB device revision tells them apart. Only the G25 has been tested on real hardware;
// the G27 and Driving Force GT follow Linux hid-lg4ff.
constexpr USHORT LogitechVid = 0x046D;
constexpr USHORT CompatibilityPid = 0xC294;

struct WheelModel {
    const wchar_t* name;
    USHORT nativePid;
    const wchar_t* hardwareId;      // as used in the DirectInput OEM registry keys
    USHORT revisionMask, revision;  // identification in compatibility mode
    std::vector<std::vector<BYTE>> nativeModeCommands;
    DWORD buttons;                  // for the DirectInput hardware settings
};

// In identification order: the first model whose revision rule matches a wheel in compatibility mode is it.
// The G29 (revision 135x) comes first only so that the Driving Force GT rule does not take it; Logitech's
// current software supports it, so it is left alone.
inline const std::vector<WheelModel>& WheelModels()
{
    static const std::vector<WheelModel> models = {
        { L"Logitech G29", 0, nullptr, 0xFFF8, 0x1350, {}, 0 },
        // Extended command 0x0A: revert to compatibility mode on USB reset; 0x09: switch to mode 4 (G27) / 3 (DFGT),
        // detaching from USB
        { L"Logitech G27", 0xC29B, L"VID_046D&PID_C29B", 0xFFF0, 0x1230, { { 0xF8, 0x0A }, { 0xF8, 0x09, 0x04, 0x01 } }, 22 },
        { L"Logitech Driving Force GT", 0xC29A, L"VID_046D&PID_C29A", 0xFF00, 0x1300, { { 0xF8, 0x0A }, { 0xF8, 0x09, 0x03, 0x01 } }, 21 },
        // The G25 only knows extended command 0x10: switch to native mode, detaching from USB
        { L"Logitech G25", 0xC299, L"VID_046D&PID_C299", 0xFF00, 0x1200, { { 0xF8, 0x10 } }, 19 },
    };
    return models;
}

// The supported model with this native product id, or null
inline const WheelModel* ModelByNativePid(USHORT productId)
{
    for (const auto& m : WheelModels())
        if (m.nativePid && m.nativePid == productId)
            return &m;
    return nullptr;
}

// The supported model of a wheel in compatibility mode with this device revision, or null
inline const WheelModel* ModelInCompatibilityMode(USHORT productId, USHORT revision)
{
    if (productId != CompatibilityPid)
        return nullptr;
    for (const auto& m : WheelModels())
        if ((revision & m.revisionMask) == m.revision)
            return m.nativePid ? &m : nullptr;
    return nullptr;
}
