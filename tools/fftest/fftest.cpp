// fftest - minimal DirectInput force feedback test tool
//   fftest            list game controllers, FF capability and supported effects
//   fftest play [ms]  play a constant force on the first FF device (default 2000 ms)
//   fftest suite [interactive]  play every effect type with a description of what it should feel like;
//                     interactive waits for Enter before each one and saves the answers to suite-results.txt
//   fftest directions measure which way forces from the east and the west turn the wheel
//   fftest state      print the wheel position and buttons
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>

static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    void* pc = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod))
        GetModuleFileNameA(mod, name, sizeof(name));
    printf("ACCESS VIOLATION at %p (%s+0x%llX), %s address %p\n", pc, name,
           (unsigned long long)((char*)pc - (char*)mod),
           ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
           (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

static LPDIRECTINPUT8W g_di;
static GUID g_ffDevice;
static bool g_ffFound;

static BOOL CALLBACK EnumEffect(LPCDIEFFECTINFOW ei, LPVOID)
{
    printf("    effect: %ls (type 0x%08lX)\n", ei->tszName, ei->dwEffType);
    return DIENUM_CONTINUE;
}

static BOOL CALLBACK EnumDevice(LPCDIDEVICEINSTANCEW inst, LPVOID)
{
    printf("%ls  [%ls]\n", inst->tszInstanceName, inst->tszProductName);
    LPDIRECTINPUTDEVICE8W dev;
    if (FAILED(g_di->CreateDevice(inst->guidInstance, &dev, nullptr)))
        return DIENUM_CONTINUE;
    DIDEVCAPS caps = { sizeof(caps) };
    dev->GetCapabilities(&caps);
    bool ff = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    printf("  force feedback: %s, axes %lu, buttons %lu, ffDriverVer 0x%08lX\n",
           ff ? "YES" : "no", caps.dwAxes, caps.dwButtons, caps.dwFFDriverVersion);
    if (ff) {
        dev->EnumEffects(EnumEffect, nullptr, DIEFT_ALL);
        if (!g_ffFound) {
            g_ffDevice = inst->guidInstance;
            g_ffFound = true;
        }
    }
    dev->Release();
    return DIENUM_CONTINUE;
}

static int Play(DWORD ms)
{
    LPDIRECTINPUTDEVICE8W dev;
    HRESULT hr = g_di->CreateDevice(g_ffDevice, &dev, nullptr);
    if (FAILED(hr)) { printf("CreateDevice failed 0x%08lX\n", hr); return 1; }
    dev->SetDataFormat(&c_dfDIJoystick);
    // FF needs exclusive access, which needs a top-level window; a hidden one is enough
    HWND wnd = CreateWindowExW(0, L"STATIC", L"fftest", WS_OVERLAPPED, 0, 0, 0, 0,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    hr = dev->SetCooperativeLevel(wnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr)) printf("SetCooperativeLevel failed 0x%08lX\n", hr);
    hr = dev->Acquire();
    if (FAILED(hr)) { printf("Acquire failed 0x%08lX\n", hr); return 1; }

    DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
    LONG dir[2] = { 0, 0 };
    DICONSTANTFORCE cf = { 5000 };
    DIEFFECT eff = { sizeof(eff) };
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = ms * 1000;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.cAxes = 2;
    eff.rgdwAxes = axes;
    eff.rglDirection = dir;
    dir[0] = 1; // push along X
    eff.cbTypeSpecificParams = sizeof(cf);
    eff.lpvTypeSpecificParams = &cf;

    LPDIRECTINPUTEFFECT effect;
    hr = dev->CreateEffect(GUID_ConstantForce, &eff, &effect, nullptr);
    if (FAILED(hr)) { printf("CreateEffect failed 0x%08lX\n", hr); return 1; }
    hr = effect->Start(1, 0);
    printf("Start: 0x%08lX - playing constant force for %lu ms\n", hr, ms);
    Sleep(ms + 200);
    effect->Release();
    dev->Unacquire();
    dev->Release();
    return 0;
}

// Plays one effect per test with a label, so a person holding the wheel can tell what each should feel like
static int Suite(bool interactive)
{
    LPDIRECTINPUTDEVICE8W dev;
    HRESULT hr = g_di->CreateDevice(g_ffDevice, &dev, nullptr);
    if (FAILED(hr)) { printf("CreateDevice failed 0x%08lX\n", hr); return 1; }
    dev->SetDataFormat(&c_dfDIJoystick);
    HWND wnd = CreateWindowExW(0, L"STATIC", L"fftest", WS_OVERLAPPED, 0, 0, 0, 0,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    dev->SetCooperativeLevel(wnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr = dev->Acquire())) { printf("Acquire failed 0x%08lX\n", hr); return 1; }
    // Answers of the interactive run go to suite-results.txt next to the program
    FILE* results = nullptr;
    if (interactive) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* name = strrchr(path, '\\') + 1;
        strcpy_s(name, MAX_PATH - (name - path), "suite-results.txt");
        if (fopen_s(&results, path, "w") != 0)
            results = nullptr;
        interactive = results != nullptr;
    }

    struct Test {
        const char* label;
        const GUID* type;
        DWORD axes;             // 1: X only (cartesian), 2: X and Y (polar)
        LONG direction;         // 1 axis: +1 or -1; 2 axes: polar, hundredths of a degree
        DWORD durationMs;
        void* params;
        DWORD paramSize;
        DIENVELOPE* envelope;
    };
    DICONSTANTFORCE half = { 5000 };
    DIPERIODIC buzz = { 4000, 0, 0, 60 * 1000 };       // 60 ms period: vibration
    DIPERIODIC knock = { 4000, 0, 0, 500 * 1000 };     // 500 ms period: slow back and forth
    DIRAMPFORCE ramp = { 0, 7000 };
    DICONDITION spring = { 0, 8000, 8000, 10000, 10000, 0 };
    DICONDITION springLeft = { -5000, 8000, 8000, 10000, 10000, 0 };
    DICONDITION damper = { 0, 5000, 5000, 10000, 10000, 0 };
    DICONDITION friction = { 0, 6000, 6000, 10000, 10000, 0 };
    DIENVELOPE fadeIn = { sizeof(DIENVELOPE), 0, 1500 * 1000, 10000, 0 };
    Test tests[] = {
        { "1. Constant force from the EAST (1 axis, +1): wheel pushed LEFT", &GUID_ConstantForce, 1, 1, 1500, &half, sizeof(half), nullptr },
        { "2. Constant force from the WEST (1 axis, -1): wheel pushed RIGHT", &GUID_ConstantForce, 1, -1, 1500, &half, sizeof(half), nullptr },
        { "3. Sine 60 ms: VIBRATION", &GUID_Sine, 1, 1, 2000, &buzz, sizeof(buzz), nullptr },
        { "4. Square 500 ms: slow KNOCKING left and right", &GUID_Square, 1, 1, 3000, &knock, sizeof(knock), nullptr },
        { "5. Ramp from the east: force GROWING from zero, pushing left", &GUID_RampForce, 1, 1, 2500, &ramp, sizeof(ramp), nullptr },
        { "6. Constant with envelope: push left FADING IN over 1.5 s", &GUID_ConstantForce, 1, 1, 2500, &half, sizeof(half), &fadeIn },
        { "7. Spring: turn the wheel, it should PULL BACK TO CENTER", &GUID_Spring, 1, 1, 5000, &spring, sizeof(spring), nullptr },
        { "8. Spring with offset -50%: wheel pulled to a point LEFT of center", &GUID_Spring, 1, 1, 5000, &springLeft, sizeof(springLeft), nullptr },
        { "9. Damper at 50%: turn the wheel, fast turns heavier than slow ones", &GUID_Damper, 1, 1, 5000, &damper, sizeof(damper), nullptr },
        { "10. Friction: turn the wheel, resistance even when turning slowly", &GUID_Friction, 1, 1, 5000, &friction, sizeof(friction), nullptr },
        { "11. Inertia (played as damper at 50%): fast turns heavier than slow ones", &GUID_Inertia, 1, 1, 5000, &damper, sizeof(damper), nullptr },
    };
    for (auto& t : tests) {
        DWORD axes[2] = { DIJOFS_X, DIJOFS_Y };
        LONG dir[2] = { t.direction, 0 };
        DIEFFECT eff = { sizeof(eff) };
        eff.dwFlags = (t.axes == 1 ? DIEFF_CARTESIAN : DIEFF_POLAR) | DIEFF_OBJECTOFFSETS;
        eff.dwDuration = t.durationMs * 1000;
        eff.dwGain = DI_FFNOMINALMAX;
        eff.dwTriggerButton = DIEB_NOTRIGGER;
        eff.cAxes = t.axes;
        eff.rgdwAxes = axes;
        eff.rglDirection = dir;
        eff.lpEnvelope = t.envelope;
        eff.cbTypeSpecificParams = t.paramSize;
        eff.lpvTypeSpecificParams = t.params;
        printf("\n%s\n", t.label);
        if (interactive) {
            printf("   Press Enter to play it (q + Enter to quit): ");
            char line[16] = "";
            if (!fgets(line, sizeof(line), stdin) || line[0] == 'q')
                break;
        }
        LPDIRECTINPUTEFFECT effect;
        hr = dev->CreateEffect(*t.type, &eff, &effect, nullptr);
        if (FAILED(hr)) { printf("   CreateEffect failed 0x%08lX\n", hr); continue; }
        hr = effect->Start(1, 0);
        Sleep(t.durationMs / 2);
        DWORD status = 0, state = 0;
        effect->GetEffectStatus(&status);
        dev->GetForceFeedbackState(&state);
        printf("   start 0x%08lX, playing %s, device state %08lX\n", hr, (status & DIEGES_PLAYING) ? "yes" : "no", state);
        Sleep(t.durationMs / 2 + 300);
        effect->GetEffectStatus(&status);
        printf("   after duration: playing %s\n", (status & DIEGES_PLAYING) ? "yes" : "no");
        effect->Release();
        if (interactive) {
            printf("   Did it feel as described? [y/n, or type a comment]: ");
            char answer[200] = "";
            if (fgets(answer, sizeof(answer), stdin)) {
                answer[strcspn(answer, "\r\n")] = 0;
                fprintf(results, "%s => %s\n", t.label, answer[0] ? answer : "(no answer)");
                fflush(results);
            }
        } else {
            Sleep(1500);
        }
    }
    if (results)
        fclose(results);
    dev->Unacquire();
    dev->Release();
    printf("\nDone.\n");
    return 0;
}

// Plays a constant force from the east and from the west and measures where the wheel actually moves
// (DirectInput: the direction is where the force comes from)
static int DirectionTest()
{
    LPDIRECTINPUTDEVICE8W dev;
    HRESULT hr = g_di->CreateDevice(g_ffDevice, &dev, nullptr);
    if (FAILED(hr)) { printf("CreateDevice failed 0x%08lX\n", hr); return 1; }
    dev->SetDataFormat(&c_dfDIJoystick);
    HWND wnd = CreateWindowExW(0, L"STATIC", L"fftest", WS_OVERLAPPED, 0, 0, 0, 0,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    dev->SetCooperativeLevel(wnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
    if (FAILED(hr = dev->Acquire())) { printf("Acquire failed 0x%08lX\n", hr); return 1; }
    auto position = [&]() {
        DIJOYSTATE js = {};
        dev->Poll();
        dev->GetDeviceState(sizeof(js), &js);
        return js.lX;
    };

    DWORD axes[1] = { DIJOFS_X };
    DICONSTANTFORCE cf = { 4000 };
    struct { const char* name; LONG direction; const char* expected; } dirs[] = {
        { "EAST", 1, "left" },
        { "WEST", -1, "right" },
    };
    for (auto& d : dirs) {
        Sleep(1500);
        LONG x0 = position();
        LONG dir[1] = { d.direction };
        DIEFFECT eff = { sizeof(eff) };
        eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        eff.dwDuration = 700 * 1000;
        eff.dwGain = DI_FFNOMINALMAX;
        eff.dwTriggerButton = DIEB_NOTRIGGER;
        eff.cAxes = 1;
        eff.rgdwAxes = axes;
        eff.rglDirection = dir;
        eff.cbTypeSpecificParams = sizeof(cf);
        eff.lpvTypeSpecificParams = &cf;
        LPDIRECTINPUTEFFECT effect;
        if (FAILED(hr = dev->CreateEffect(GUID_ConstantForce, &eff, &effect, nullptr))) {
            printf("CreateEffect failed 0x%08lX\n", hr);
            continue;
        }
        effect->Start(1, 0);
        Sleep(500);
        LONG dx = position() - x0;
        effect->Release();
        const char* moved = dx < 0 ? "left" : "right";
        printf("from %-4s: X %+6ld -> moved %-5s expected %s %s\n", d.name, dx, moved, d.expected,
               !strcmp(d.expected, moved) ? "OK" : "MISMATCH");
    }
    dev->Unacquire();
    dev->Release();
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, CrashHandler);
    HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                    IID_IDirectInput8W, (void**)&g_di, nullptr);
    if (FAILED(hr)) { printf("DirectInput8Create failed 0x%08lX\n", hr); return 1; }
    g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevice, nullptr, DIEDFL_ATTACHEDONLY);
    int rc = 0;
    if (argc > 1 && !strcmp(argv[1], "state") && g_ffFound) {
        LPDIRECTINPUTDEVICE8W dev;
        g_di->CreateDevice(g_ffDevice, &dev, nullptr);
        dev->SetDataFormat(&c_dfDIJoystick);
        dev->Acquire();
        for (int i = 0; i < 10; i++) {
            DIJOYSTATE js = {};
            dev->Poll();
            dev->GetDeviceState(sizeof(js), &js);
            printf("X %5ld  Y %5ld  Z %5ld  Rz %5ld  POV %5ld  buttons %02X%02X\n", js.lX, js.lY, js.lZ, js.lRz,
                   (long)js.rgdwPOV[0], js.rgbButtons[1], js.rgbButtons[0]);
            Sleep(300);
        }
        dev->Release();
    }
    if (argc > 1 && !strcmp(argv[1], "directions"))
        rc = g_ffFound ? DirectionTest() : 1;
    if (argc > 1 && !strcmp(argv[1], "suite"))
        rc = g_ffFound ? Suite(argc > 2 && !strcmp(argv[2], "interactive")) : 1;
    if (argc > 1 && !strcmp(argv[1], "play")) {
        if (!g_ffFound) { printf("No force feedback device found\n"); rc = 1; }
        else rc = Play(argc > 2 ? atoi(argv[2]) : 2000);
    }
    g_di->Release();
    return rc;
}
