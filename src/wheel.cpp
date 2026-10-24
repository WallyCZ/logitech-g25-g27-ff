#include "wheel.h"
#include "devices.h"
#include "log.h"
#include <setupapi.h>
#include <algorithm>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace wheel {

namespace {

BYTE SlotMask(Slot slot) { return (BYTE)(0x10 << slot); }
BYTE Clamp8(int v) { return (BYTE)std::clamp(v, 0, 255); }
BYTE Nibble(int v) { return (BYTE)std::clamp(v < 0 ? -v : v, 0, 15); }
BYTE Sign(int v) { return v < 0 ? 1 : 0; }

}  // namespace

Command SetRange(int degrees)
{
    degrees = std::clamp(degrees, 40, 900);
    return { 0xF8, 0x81, (BYTE)(degrees & 0xFF), (BYTE)(degrees >> 8) };
}

// Same scaling as Linux hid-lg4ff: the strength (0..0xFFFF) sets the spring coefficient and the clip
Command Autocenter(int percent, Command* enable)
{
    if (percent <= 0)
        return { 0xF5 };
    DWORD magnitude = (DWORD)std::min(percent, 100) * 0xFFFF / 100, a, b;
    if (magnitude <= 0xAAAA) {
        a = 0x0C * magnitude;
        b = 0x80 * magnitude;
    } else {
        a = 0x0C * 0xAAAA + 0x06 * (magnitude - 0xAAAA);
        b = 0x80 * 0xAAAA + 0xFF * (magnitude - 0xAAAA);
    }
    a >>= 1;  // all wheels but the MOMO
    if (enable)
        *enable = { 0x14 };
    return { 0xFE, 0x0D, (BYTE)(a / 0xAAAA), (BYTE)(a / 0xAAAA), Clamp8(b / 0xAAAA) };
}

Command StopAll() { return { 0xF3 }; }

Command StopSlot(Slot slot) { return { (BYTE)(SlotMask(slot) | 0x03) }; }

Command Constant(BYTE level) { return { (BYTE)(SlotMask(SlotConstant) | 0x01), 0x08, level, NoForce }; }

Command Spring(const SpringParams& p)
{
    int d1 = std::clamp(p.center1, 0, 2047), d2 = std::clamp(p.center2, d1, 2047);
    return { (BYTE)(SlotMask(SlotSpring) | 0x01), 0x0B, (BYTE)(d1 >> 3), (BYTE)(d2 >> 3),
             (BYTE)(Nibble(p.k2) << 4 | Nibble(p.k1)),
             (BYTE)((d2 & 7) << 5 | Sign(p.k2) << 4 | (d1 & 7) << 1 | Sign(p.k1)), Clamp8(p.clip) };
}

Command Damper(const DamperParams& p)
{
    return { (BYTE)(SlotMask(SlotDamper) | 0x01), 0x0C, Nibble(p.k1), Sign(p.k1), Nibble(p.k2), Sign(p.k2),
             Clamp8(p.clip) };
}

Command Friction(const FrictionParams& p)
{
    return { (BYTE)(SlotMask(SlotFriction) | 0x01), 0x0E, Clamp8(p.k1 < 0 ? -p.k1 : p.k1),
             Clamp8(p.k2 < 0 ? -p.k2 : p.k2), Clamp8(p.clip), (BYTE)(Sign(p.k2) << 4 | Sign(p.k1)) };
}

bool Device::Open(const wchar_t* path)
{
    Close();
    handle_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        Log("cannot open %ls (%lu)", path, GetLastError());
        return false;
    }
    HidD_GetAttributes(handle_, &attributes_);
    PHIDP_PREPARSED_DATA preparsed;
    if (HidD_GetPreparsedData(handle_, &preparsed)) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.OutputReportByteLength)
            outputLength_ = caps.OutputReportByteLength;
        HidD_FreePreparsedData(preparsed);
    }
    return true;
}

void Device::Close()
{
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

bool Device::Send(const Command& command)
{
    if (handle_ == INVALID_HANDLE_VALUE)
        return false;
    std::vector<BYTE> report(std::max<size_t>(outputLength_, command.size() + 1), 0);
    std::copy(command.begin(), command.end(), report.begin() + 1);
    DWORD written = 0;
    if (!WriteFile(handle_, report.data(), (DWORD)report.size(), &written, nullptr)) {
        Log("write %02X failed (%lu)", command.empty() ? 0 : command[0], GetLastError());
        return false;
    }
    return true;
}

std::vector<Found> FindWheels(bool compatibility)
{
    std::vector<Found> found;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return found;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &size, nullptr);
        std::vector<BYTE> buffer(size);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, size, nullptr, nullptr))
            continue;
        std::wstring path = detail->DevicePath;
        std::wstring upper = path;
        for (auto& ch : upper)
            ch = towupper(ch);
        if (upper.find(L"VID_046D&PID_") == std::wstring::npos)
            continue;
        // The product id and, in compatibility mode, the revision tell the model
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        HIDD_ATTRIBUTES attributes = { sizeof(attributes) };
        const WheelModel* model = nullptr;
        if (HidD_GetAttributes(h, &attributes))
            model = compatibility ? ModelInCompatibilityMode(attributes.ProductID, attributes.VersionNumber)
                                  : ModelByNativePid(attributes.ProductID);
        CloseHandle(h);
        if (model)
            found.push_back({ path, model });
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
}  // namespace wheel
