#pragma once
#include <windows.h>
#include <hidsdi.h>
#include <string>
#include <vector>
#include "devices.h"

// The force feedback protocol of the G25, G27 and Driving Force GT: 7-byte output reports (after the zero report
// ID), Logitech's "classic" protocol. The first byte is a command, with a bit mask of the four force slots in its
// high nibble for the slot commands. Values verified on a G25:
//   F8 ...                   extended commands, among them the native mode switch (see devices.h)
//   F8 81 lo hi              rotation range in degrees, 40..900
//   FE 0D k k strength / 14  set and enable the firmware's autocenter spring; F5 disables it
//   <slots>1 type params     download and play a force; <slots>3 stops it; F3 stops all
// Force types in the slots:
//   08 level 80              constant ("variable") force, 0x80 = none, lower values turn the wheel right
//   0B d1 d2 k2|k1 low clip  spring: dead band d1..d2 (11-bit position, upper 8 bits + low 3 bits in byte 5),
//                            coefficients 0..15 left (k1) and right (k2) of it, signs in byte 5, clip 0..255
//   0C k1 s1 k2 s2 clip      damper: coefficients 0..15 for movement left (k1) and right (k2), signs
//   0E k1 k2 clip signs      friction: coefficients 0..255
namespace wheel {

constexpr int ReportSize = 8;  // report ID + 7 bytes
constexpr BYTE NoForce = 0x80;

enum Slot { SlotConstant = 0, SlotSpring = 1, SlotDamper = 2, SlotFriction = 3, SlotCount = 4 };

struct SpringParams {
    int center1 = 1024, center2 = 1024;  // dead band, 0..2047 (left..right), center1 <= center2
    int k1 = 0, k2 = 0;                  // -15..15 left and right of the dead band; negative pushes away
    int clip = 255;                      // 0..255
};

struct DamperParams {
    int k1 = 0, k2 = 0;  // -15..15 for movement to the left and to the right
    int clip = 255;
};

struct FrictionParams {
    int k1 = 0, k2 = 0;  // -255..255
    int clip = 255;
};

using Command = std::vector<BYTE>;  // up to 7 bytes, without the report ID

Command SetRange(int degrees);
Command Autocenter(int percent, Command* enable);  // percent 0 = off; else also fills the enable command
Command StopAll();
Command StopSlot(Slot slot);
Command Constant(BYTE level);
Command Spring(const SpringParams& p);
Command Damper(const DamperParams& p);
Command Friction(const FrictionParams& p);

// One open wheel (its HID interface)
class Device {
public:
    ~Device() { Close(); }
    bool Open(const wchar_t* path);
    void Close();
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool Send(const Command& command);
    USHORT ProductId() const { return attributes_.ProductID; }
    USHORT Revision() const { return attributes_.VersionNumber; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HIDD_ATTRIBUTES attributes_ = { sizeof(HIDD_ATTRIBUTES) };
    USHORT outputLength_ = ReportSize;
};

struct Found {
    std::wstring path;  // HID interface
    const WheelModel* model;
};

// The connected supported wheels, in native mode or (with compatibility = true) in compatibility mode
std::vector<Found> FindWheels(bool compatibility);

}  // namespace wheel
