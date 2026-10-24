#include "agent.h"
#include "settings.h"
#include "log.h"
#include <dbt.h>
#include <hidsdi.h>

namespace {

constexpr UINT_PTR CheckTimer = 1;
constexpr UINT CheckDelayMs = 500;  // a wheel arrives as several devices; check once they are all there

void CheckWheels()
{
    // A wheel switched to native mode disconnects and arrives again, then gets its settings
    int switched = SwitchWheelsToNative();
    if (!switched)
        ApplyWheelSettings(Settings::Load());
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL)
            SetTimer(window, CheckTimer, CheckDelayMs, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wParam == CheckTimer) {
            KillTimer(window, CheckTimer);
            CheckWheels();
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

void RunAgent()
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, AgentMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex)
            CloseHandle(mutex);
        return;
    }
    HANDLE exit = CreateEventW(nullptr, TRUE, FALSE, AgentExitEventName);

    // Registered device notifications arrive at a message-only window
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"G25FFAgent";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"G25FF agent", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    DEV_BROADCAST_DEVICEINTERFACE_W filter = { sizeof(filter), DBT_DEVTYP_DEVICEINTERFACE };
    HidD_GetHidGuid(&filter.dbcc_classguid);
    HDEVNOTIFY notify = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    Log("agent running");

    CheckWheels();
    for (;;) {
        DWORD wait = MsgWaitForMultipleObjects(1, &exit, FALSE, INFINITE, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0 + 1)
            break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    Log("agent exiting");
    if (notify)
        UnregisterDeviceNotification(notify);
    DestroyWindow(window);
    CloseHandle(exit);
    CloseHandle(mutex);
}
