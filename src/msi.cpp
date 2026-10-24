// Custom actions for the MSI installer (installer\G25FF.wxs). They run from the 64-bit DLL.
#include "registration.h"
#include "settings.h"
#include "log.h"
#include <msi.h>
#include <msiquery.h>

#pragma comment(lib, "msi.lib")

// Immediate, before the setup dialog: sets LOGITECHDRIVERFOUND when the original Logitech driver is installed,
// so the dialog can offer to remove it
extern "C" UINT __stdcall MsiDetectLogitechDriver(MSIHANDLE install)
{
    if (!FindLogitechDriver().empty())
        MsiSetPropertyW(install, L"LOGITECHDRIVERFOUND", L"1");
    return ERROR_SUCCESS;
}

// Immediate, before the files are checked: the running agent keeps the installed DLL loaded
extern "C" UINT __stdcall MsiStopAgents(MSIHANDLE)
{
    StopAgents();
    return ERROR_SUCCESS;
}

// Deferred, as the local system account: machine-wide registration and the agent's Run entry.
// CustomActionData is "1" when the user chose to remove the original Logitech driver.
extern "C" UINT __stdcall MsiRegisterMachine(MSIHANDLE install)
{
    wchar_t data[16] = L"";
    DWORD size = ARRAYSIZE(data);
    MsiGetPropertyW(install, L"CustomActionData", data, &size);
    if (!wcscmp(data, L"1"))
        RemoveLogitechDriver();
    HRESULT hr = RegisterClasses(HKEY_LOCAL_MACHINE);
    if (SUCCEEDED(hr))
        hr = RegisterDevices(HKEY_LOCAL_MACHINE);
    if (SUCCEEDED(hr))
        RegisterAgent();
    Log("MSI machine registration: %08lX", hr);
    return SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

// Deferred (and rollback), as the local system account
extern "C" UINT __stdcall MsiUnregisterMachine(MSIHANDLE)
{
    UnregisterAgent();
    UnregisterClasses(HKEY_LOCAL_MACHINE);
    UnregisterDevices(HKEY_LOCAL_MACHINE);
    return ERROR_SUCCESS;
}

// Deferred, impersonating the installing user: their DirectInput copy of the wheel's data, and the agent,
// which switches the wheel to native mode right away. Failures here do not fail the installation.
extern "C" UINT __stdcall MsiRegisterUser(MSIHANDLE)
{
    HRESULT hr = RegisterDevices(HKEY_CURRENT_USER);
    Log("MSI user registration: %08lX", hr);
    StartAgent();
    return ERROR_SUCCESS;
}

// Deferred (and rollback), impersonating the installing user
extern "C" UINT __stdcall MsiUnregisterUser(MSIHANDLE)
{
    StopAgents();
    UnregisterDevices(HKEY_CURRENT_USER);
    return ERROR_SUCCESS;
}
