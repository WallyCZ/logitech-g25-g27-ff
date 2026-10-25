#include "registration.h"
#include "effectdriver.h"
#include "proppage.h"
#include "devices.h"
#include "agent.h"
#include "log.h"
#include <cfgmgr32.h>
#include <newdev.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "newdev.lib")

namespace {

constexpr wchar_t OemKey[] = L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\";
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t RunValue[] = L"G25FFAgent";
// Version 1.0.0 started the agent with rundll32 under this name. Windows keeps a startup entry's enabled or
// disabled state (Task Manager > Startup apps) under its name in StartupApproved\Run.
constexpr wchar_t OldRunValue[] = L"G25FF";
constexpr wchar_t StartupApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

// DirectInput DIEFFECTATTRIBUTES as stored in the registry. dwEffectId is what DirectInput passes back to
// DownloadEffect.
struct EffectAttributes {
    DWORD effectId, effectType, staticParams, dynamicParams, coords;
};

struct EffectInfo {
    const wchar_t* guid;
    const wchar_t* name;
    EffectAttributes attributes;
};

constexpr DWORD CommonFlags = DIEFT_FFATTACK | DIEFT_FFFADE | DIEFT_STARTDELAY;
constexpr DWORD ConditionFlags = DIEFT_SATURATION | DIEFT_POSNEGCOEFFICIENTS | DIEFT_DEADBAND | DIEFT_STARTDELAY;
// No trigger buttons: the wheel cannot play effects by itself
constexpr DWORD AllParams = DIEP_DURATION | DIEP_GAIN | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE |
                            DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
constexpr DWORD ConditionParams = AllParams & ~DIEP_ENVELOPE;
constexpr DWORD Coords = DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL;

const EffectInfo g_effects[] = {
    { L"{13541C20-8E33-11D0-9AD0-00A0C9A06E35}", L"Constant Force",
      { effect::ConstantForce, DIEFT_CONSTANTFORCE | CommonFlags, AllParams, AllParams, Coords } },
    { L"{13541C21-8E33-11D0-9AD0-00A0C9A06E35}", L"Ramp Force",
      { effect::Ramp, DIEFT_RAMPFORCE | CommonFlags, AllParams, AllParams, Coords } },
    { L"{13541C22-8E33-11D0-9AD0-00A0C9A06E35}", L"Square Wave",
      { effect::Square, DIEFT_PERIODIC | CommonFlags, AllParams, AllParams, Coords } },
    { L"{13541C23-8E33-11D0-9AD0-00A0C9A06E35}", L"Sine Wave",
      { effect::Sine, DIEFT_PERIODIC | CommonFlags, AllParams, AllParams, Coords } },
    { L"{13541C24-8E33-11D0-9AD0-00A0C9A06E35}", L"Triangle Wave",
      { effect::Triangle, DIEFT_PERIODIC | CommonFlags, AllParams, AllParams, Coords } },
    { L"{13541C25-8E33-11D0-9AD0-00A0C9A06E35}", L"Sawtooth Up Wave",
      { effect::SawtoothUp, DIEFT_PERIODIC | CommonFlags, AllParams, AllParams, Coords } },
    { L"{13541C26-8E33-11D0-9AD0-00A0C9A06E35}", L"Sawtooth Down Wave",
      { effect::SawtoothDown, DIEFT_PERIODIC | CommonFlags, AllParams, AllParams, Coords } },
    { L"{13541C27-8E33-11D0-9AD0-00A0C9A06E35}", L"Spring",
      { effect::Spring, DIEFT_CONDITION | ConditionFlags, ConditionParams, ConditionParams, Coords } },
    { L"{13541C28-8E33-11D0-9AD0-00A0C9A06E35}", L"Damper",
      { effect::Damper, DIEFT_CONDITION | ConditionFlags, ConditionParams, ConditionParams, Coords } },
    { L"{13541C29-8E33-11D0-9AD0-00A0C9A06E35}", L"Inertia",
      { effect::Inertia, DIEFT_CONDITION | ConditionFlags, ConditionParams, ConditionParams, Coords } },
    { L"{13541C2A-8E33-11D0-9AD0-00A0C9A06E35}", L"Friction",
      { effect::Friction, DIEFT_CONDITION | ConditionFlags, ConditionParams, ConditionParams, Coords } },
};

struct ComClass {
    const CLSID& clsid;
    const wchar_t* name;
};
const ComClass g_classes[] = {
    { CLSID_G25FF, L"Logitech G25 force feedback driver" },
    { CLSID_G25FFConfig, L"Logitech G25 force feedback settings" },
};

LSTATUS SetString(HKEY key, const wchar_t* name, const wchar_t* value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, (DWORD)(wcslen(value) + 1) * sizeof(wchar_t));
}

std::wstring GuidString(REFGUID guid)
{
    wchar_t text[64];
    StringFromGUID2(guid, text, ARRAYSIZE(text));
    return text;
}

std::wstring ClassKey(REFCLSID clsid) { return L"Software\\Classes\\CLSID\\" + GuidString(clsid); }

std::wstring ModulePath()
{
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(g_module, module, MAX_PATH);
    return module;
}

// view: 0 for the process' own registry view, KEY_WOW64_32KEY for the 32-bit view from a 64-bit process
LSTATUS RegisterComClass(HKEY root, REGSAM view, const ComClass& c, const wchar_t* module)
{
    std::wstring key = ClassKey(c.clsid);
    HKEY k;
    LSTATUS rc = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE | view, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS)
        return rc;
    SetString(k, nullptr, c.name);
    RegCloseKey(k);
    key += L"\\InprocServer32";
    rc = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE | view, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS)
        return rc;
    SetString(k, nullptr, module);
    SetString(k, L"ThreadingModel", L"Both");
    RegCloseKey(k);
    return ERROR_SUCCESS;
}

// G25FFAgent.exe next to this DLL
std::wstring AgentPath()
{
    std::wstring p = ModulePath();
    return p.substr(0, p.find_last_of(L'\\') + 1) + L"G25FFAgent.exe";
}

// The 32-bit build installed next to this 64-bit one, or empty
std::wstring Sibling32BitModule()
{
#ifdef _WIN64
    std::wstring p = ModulePath();
    size_t dir = p.find_last_of(L'\\');  // ...\x64\G25FF.dll
    size_t parent = dir == std::wstring::npos ? dir : p.find_last_of(L'\\', dir - 1);
    if (parent != std::wstring::npos) {
        std::wstring x86 = p.substr(0, parent) + L"\\x86" + p.substr(dir);
        if (GetFileAttributesW(x86.c_str()) != INVALID_FILE_ATTRIBUTES)
            return x86;
    }
#endif
    return L"";
}

LSTATUS RegisterDevice(HKEY root, const WheelModel& model)
{
    std::wstring oem = std::wstring(OemKey) + model.hardwareId, ffPath = oem + L"\\OEMForceFeedback";
    RegDeleteTreeW(root, ffPath.c_str());

    HKEY key;
    LSTATUS rc = RegCreateKeyExW(root, oem.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_QUERY_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS)
        return rc;
    SetString(key, L"ConfigCLSID", GuidString(CLSID_G25FFConfig).c_str());
    // DirectInput only reports the joystick type (and so our ConfigCLSID to joy.cpl) when its hardware
    // settings exist; Windows does not create them for HID devices. JOYREGHWSETTINGS: a car controller with a
    // hat switch.
    if (root == HKEY_LOCAL_MACHINE) {
        const DWORD oemData[] = { JOY_HWS_ISCARCTRL | JOY_HWS_HASPOV | 0x10000000, model.buttons };
        RegSetValueExW(key, L"OEMData", 0, REG_BINARY, (const BYTE*)oemData, sizeof(oemData));
    }
    RegCloseKey(key);

    // The wheel's HID descriptor has no force feedback usages, so DirectInput only accepts effects on the
    // steering axis (X, axis 0) when the registry marks it as an actuator
    std::wstring axisPath = oem + L"\\Axes\\0";
    rc = RegCreateKeyExW(root, axisPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS)
        return rc;
    DIOBJECTATTRIBUTES axis = { DIDOI_FFACTUATOR | DIDOI_ASPECTPOSITION, 0x01, 0x30 };  // Generic Desktop X
    DIFFOBJECTATTRIBUTES axisForce = { DI_FFNOMINALMAX, DI_FFNOMINALMAX / 127 };         // 8-bit constant force
    RegSetValueExW(key, L"Attributes", 0, REG_BINARY, (const BYTE*)&axis, sizeof(axis));
    RegSetValueExW(key, L"FFAttributes", 0, REG_BINARY, (const BYTE*)&axisForce, sizeof(axisForce));
    RegCloseKey(key);

    HKEY ff;
    rc = RegCreateKeyExW(root, ffPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &ff, nullptr);
    if (rc != ERROR_SUCCESS)
        return rc;
    DIFFDEVICEATTRIBUTES attributes = { 0, 1000, 1000 };
    SetString(ff, L"CLSID", GuidString(CLSID_G25FF).c_str());
    RegSetValueExW(ff, L"Attributes", 0, REG_BINARY, (const BYTE*)&attributes, sizeof(attributes));
    for (const auto& effect : g_effects) {
        std::wstring sub = std::wstring(L"Effects\\") + effect.guid;
        if (RegCreateKeyExW(ff, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            SetString(key, nullptr, effect.name);
            RegSetValueExW(key, L"Attributes", 0, REG_BINARY, (const BYTE*)&effect.attributes,
                           sizeof(effect.attributes));
            RegCloseKey(key);
        }
    }
    RegCloseKey(ff);
    return ERROR_SUCCESS;
}

bool ValueIs(HKEY root, const wchar_t* path, const wchar_t* name, REFGUID guid)
{
    wchar_t current[64] = L"";
    DWORD size = sizeof(current);
    return RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS &&
           !_wcsicmp(current, GuidString(guid).c_str());
}

// Removes our registration (only the parts that still point to this driver)
void UnregisterDevice(HKEY root, const wchar_t* device)
{
    std::wstring oem = std::wstring(OemKey) + device, ffPath = oem + L"\\OEMForceFeedback";
    if (ValueIs(root, ffPath.c_str(), L"CLSID", CLSID_G25FF))
        RegDeleteTreeW(root, ffPath.c_str());
    if (ValueIs(root, oem.c_str(), L"ConfigCLSID", CLSID_G25FFConfig))
        RegDeleteKeyValueW(root, oem.c_str(), L"ConfigCLSID");
}

// Reads an INF file (UTF-16 or ANSI) and checks whether it installs one of the original Logitech wheel driver's
// kernel filters. Newer Logitech packages for the same wheels use other files and may work, so they stay.
bool InfInstallsBlockedDriver(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::vector<BYTE> data(size + 2, 0);
    ReadFile(f, data.data(), size, &got, nullptr);
    CloseHandle(f);
    std::wstring text;
    if (got >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        text.assign(reinterpret_cast<const wchar_t*>(data.data() + 2), (got - 2) / 2);
    else
        text.assign(data.begin(), data.begin() + got);
    for (auto& ch : text)
        ch = towupper(ch);
    for (const wchar_t* file : { L"WMFILTER.SYS", L"WMHIDLO.SYS", L"WMBENUM.SYS", L"WMVIRHID.SYS" })
        if (text.find(file) != std::wstring::npos)
            return true;
    return false;
}

}  // namespace

HRESULT RegisterClasses(HKEY root)
{
    std::wstring module = ModulePath();
    std::wstring x86 = root == HKEY_LOCAL_MACHINE ? Sibling32BitModule() : L"";
    for (const auto& c : g_classes) {
        LSTATUS rc = RegisterComClass(root, 0, c, module.c_str());
        if (rc == ERROR_SUCCESS && !x86.empty())
            rc = RegisterComClass(root, KEY_WOW64_32KEY, c, x86.c_str());
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);
    }
    return S_OK;
}

void UnregisterClasses(HKEY root)
{
    for (const auto& c : g_classes) {
        RegDeleteTreeW(root, ClassKey(c.clsid).c_str());
#ifdef _WIN64
        if (root == HKEY_LOCAL_MACHINE) {
            HKEY classes;
            if (RegOpenKeyExW(root, L"Software\\Classes\\CLSID", 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, &classes) == ERROR_SUCCESS) {
                RegDeleteTreeW(classes, GuidString(c.clsid).c_str());
                RegCloseKey(classes);
            }
        }
#endif
    }
}

HRESULT RegisterDevices(HKEY root)
{
    for (const auto& model : WheelModels()) {
        if (!model.hardwareId)
            continue;
        LSTATUS rc = RegisterDevice(root, model);
        if (rc != ERROR_SUCCESS)
            return HRESULT_FROM_WIN32(rc);
    }
    return S_OK;
}

void UnregisterDevices(HKEY root)
{
    for (const auto& model : WheelModels())
        if (model.hardwareId)
            UnregisterDevice(root, model.hardwareId);
}

void RegisterAgent()
{
    std::wstring command = L"\"" + AgentPath() + L"\"";
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, RunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        SetString(key, RunValue, command.c_str());
        RegDeleteValueW(key, OldRunValue);
        RegCloseKey(key);
    }
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, StartupApprovedKey, OldRunValue);
}

void UnregisterAgent()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RunKey, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, RunValue);
        RegDeleteValueW(key, OldRunValue);
        RegCloseKey(key);
    }
    for (const wchar_t* name : { RunValue, OldRunValue })
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, StartupApprovedKey, name);
}

void StartAgent()
{
    if (HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, AgentMutexName)) {
        CloseHandle(running);
        return;
    }
    std::wstring exe = AgentPath();
    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("agent started");
    } else {
        Log("cannot start the agent %ls (%lu)", exe.c_str(), GetLastError());
    }
}
void StopAgents()
{
    if (HANDLE exit = OpenEventW(EVENT_MODIFY_STATE, FALSE, AgentExitEventName)) {
        SetEvent(exit);
        CloseHandle(exit);
        Sleep(500);  // let it release the DLL before the installer replaces it
    }
}

std::vector<std::wstring> FindLogitechDriver()
{
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    std::wstring infDir = std::wstring(windows) + L"\\INF\\";
    std::vector<std::wstring> found;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((infDir + L"oem*.inf").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return found;
    do {
        std::wstring inf = infDir + fd.cFileName;
        if (InfInstallsBlockedDriver(inf.c_str()))
            found.push_back(inf);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return found;
}

void RemoveLogitechDriver()
{
    bool removed = false;
    for (const auto& inf : FindLogitechDriver()) {
        BOOL reboot = FALSE;
        // Uninstalls the package from the devices using it and deletes it from the driver store
        if (DiUninstallDriverW(nullptr, inf.c_str(), 0, &reboot)) {
            Log("removed Logitech driver package %ls%s", inf.c_str(), reboot ? " (reboot needed)" : "");
            removed = true;
        } else {
            Log("cannot remove driver package %ls (%lu)", inf.c_str(), GetLastError());
        }
    }
    if (removed) {
        // Let the wheel get the standard Windows HID driver right away
        DEVINST root;
        if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
            CM_Reenumerate_DevNode(root, CM_REENUMERATE_RETRY_INSTALLATION);
    }
}
