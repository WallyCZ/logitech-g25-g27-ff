#pragma once
#include <windows.h>
#include <string>
#include <vector>

extern HMODULE g_module;

// COM classes (effect driver and property sheet) in HKLM or HKCU Software\Classes. Machine-wide registration
// from the 64-bit DLL also registers the 32-bit DLL next to it (..\x86\G25FF.dll) for 32-bit programs.
HRESULT RegisterClasses(HKEY root);
void UnregisterClasses(HKEY root);

// DirectInput OEM data of the supported wheels in native mode: our effect driver, its effects and the property
// sheet. DirectInput reads it from HKLM and keeps a per-user copy in HKCU, so both need to be written. It replaces
// the property sheet of the Logitech Profiler, which no longer works without the Logitech driver.
HRESULT RegisterDevices(HKEY root);
void UnregisterDevices(HKEY root);

// The agent: G25FFAgent.exe (next to the DLL) at logon (HKLM Run key), which switches the wheels to native mode
void RegisterAgent();
void UnregisterAgent();
void StartAgent();  // in the current session, unless it runs already
void StopAgents();  // asks the agent of the current session to exit

// The original Logitech wheel driver packages (oemNN.inf in the Windows INF folder). Their kernel filters are
// blocked by Memory Integrity and leave the wheel unusable.
std::vector<std::wstring> FindLogitechDriver();
// Removes them; the wheel then gets the standard Windows HID driver again
void RemoveLogitechDriver();
