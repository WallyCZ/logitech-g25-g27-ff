#include "proppage.h"
#include "effectdriver.h"
#include "resource.h"
#include "settings.h"
#include "devices.h"
#include "log.h"
#include <commctrl.h>
#include <prsht.h>

#pragma comment(lib, "dinput8.lib")

namespace {

// Test effects played through DirectInput, the same way a game plays them
class TestEffects {
public:
    ~TestEffects() { Stop(); }
    bool Play(HWND window, int test);
    void Stop();

private:
    static BOOL CALLBACK FindWheel(LPCDIDEVICEINSTANCEW instance, LPVOID context);
    bool Acquire(HWND window);

    IDirectInput8W* di_ = nullptr;
    IDirectInputDevice8W* device_ = nullptr;
    IDirectInputEffect* effect_ = nullptr;
};

TestEffects g_test;  // joy.cpl shows one property sheet at a time

BOOL CALLBACK TestEffects::FindWheel(LPCDIDEVICEINSTANCEW instance, LPVOID context)
{
    // DirectInput product GUIDs of HID devices start with the product and vendor id
    if (LOWORD(instance->guidProduct.Data1) == LogitechVid && ModelByNativePid(HIWORD(instance->guidProduct.Data1))) {
        *static_cast<GUID*>(context) = instance->guidInstance;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

bool TestEffects::Acquire(HWND window)
{
    if (device_)
        return true;
    GUID wheel = GUID_NULL;
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&di_), nullptr)) ||
        FAILED(di_->EnumDevices(DI8DEVCLASS_GAMECTRL, FindWheel, &wheel, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK)) ||
        wheel == GUID_NULL || FAILED(di_->CreateDevice(wheel, &device_, nullptr))) {
        Stop();
        return false;
    }
    // Force feedback needs exclusive access, which fails while a game holds the wheel
    if (FAILED(device_->SetDataFormat(&c_dfDIJoystick)) ||
        FAILED(device_->SetCooperativeLevel(GetAncestor(window, GA_ROOT), DISCL_EXCLUSIVE | DISCL_BACKGROUND)) ||
        FAILED(device_->Acquire())) {
        Stop();
        return false;
    }
    return true;
}

bool TestEffects::Play(HWND window, int test)
{
    if (!Acquire(window))
        return false;
    if (effect_) {
        effect_->Release();
        effect_ = nullptr;
    }
    DWORD axes[1] = { DIJOFS_X };
    LONG direction[1] = { 1 };
    DIEFFECT e = { sizeof(e) };
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER;
    e.cAxes = 1;
    e.rgdwAxes = axes;
    e.rglDirection = direction;
    DICONSTANTFORCE push = { 5000 };
    DIPERIODIC rumble = { 4000, 0, 0, 60 * 1000 };
    DICONDITION spring = { 0, 8000, 8000, 10000, 10000, 0 };
    const GUID* type;
    switch (test) {
    case IDC_TEST_LEFT:
    case IDC_TEST_RIGHT:
        // A force comes from its direction: from the right (+1) it pushes the wheel left
        direction[0] = test == IDC_TEST_LEFT ? 1 : -1;
        type = &GUID_ConstantForce;
        e.dwDuration = 1000 * 1000;
        e.cbTypeSpecificParams = sizeof(push);
        e.lpvTypeSpecificParams = &push;
        break;
    case IDC_TEST_RUMBLE:
        type = &GUID_Sine;
        e.dwDuration = 1500 * 1000;
        e.cbTypeSpecificParams = sizeof(rumble);
        e.lpvTypeSpecificParams = &rumble;
        break;
    default:
        type = &GUID_Spring;
        e.dwDuration = 4000 * 1000;
        e.cbTypeSpecificParams = sizeof(spring);
        e.lpvTypeSpecificParams = &spring;
        break;
    }
    HRESULT hr = device_->CreateEffect(*type, &e, &effect_, nullptr);
    if (SUCCEEDED(hr))
        hr = effect_->Start(1, 0);
    Log("test effect %d: %08lX", test, hr);
    return SUCCEEDED(hr);
}

void TestEffects::Stop()
{
    if (effect_) {
        effect_->Release();
        effect_ = nullptr;
    }
    if (device_) {
        device_->Unacquire();
        device_->Release();
        device_ = nullptr;
    }
    if (di_) {
        di_->Release();
        di_ = nullptr;
    }
}

struct Slider {
    int control, value, min, max;
    const wchar_t* unit;
};
constexpr Slider Sliders[] = {
    { IDC_RANGE, IDC_RANGE_VALUE, 40, 900, L"\u00B0" },
    { IDC_GAIN, IDC_GAIN_VALUE, 0, 100, L" %" },
    { IDC_SPRING, IDC_SPRING_VALUE, 0, 100, L" %" },
    { IDC_DAMPER, IDC_DAMPER_VALUE, 0, 100, L" %" },
    { IDC_AUTOCENTER_STRENGTH, IDC_AUTOCENTER_VALUE, 0, 100, L" %" },
};

DWORD Position(HWND page, int control) { return (DWORD)SendDlgItemMessageW(page, control, TBM_GETPOS, 0, 0); }

void ShowValue(HWND page, const Slider& s)
{
    wchar_t text[16];
    swprintf_s(text, L"%lu%s", Position(page, s.control), s.unit);
    SetDlgItemTextW(page, s.value, text);
}

void UpdateAutocenterControls(HWND page)
{
    BOOL on = IsDlgButtonChecked(page, IDC_AUTOCENTER) == BST_CHECKED;
    for (int id : { IDC_AUTOCENTER_LABEL, IDC_AUTOCENTER_STRENGTH, IDC_AUTOCENTER_VALUE })
        EnableWindow(GetDlgItem(page, id), on);
}

void Show(HWND page, const Settings& s)
{
    SendDlgItemMessageW(page, IDC_RANGE, TBM_SETPOS, TRUE, s.range);
    SendDlgItemMessageW(page, IDC_GAIN, TBM_SETPOS, TRUE, s.gain);
    SendDlgItemMessageW(page, IDC_SPRING, TBM_SETPOS, TRUE, s.springGain);
    SendDlgItemMessageW(page, IDC_DAMPER, TBM_SETPOS, TRUE, s.damperGain);
    SendDlgItemMessageW(page, IDC_AUTOCENTER_STRENGTH, TBM_SETPOS, TRUE, s.autocenterStrength);
    CheckDlgButton(page, IDC_AUTOCENTER, s.autocenter ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(page, IDC_HARDWARE_FRICTION, s.hardwareFriction ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(page, IDC_INVERT, s.invert ? BST_CHECKED : BST_UNCHECKED);
    for (const auto& slider : Sliders)
        ShowValue(page, slider);
    UpdateAutocenterControls(page);
}

Settings Read(HWND page)
{
    Settings s;
    s.range = (Position(page, IDC_RANGE) + 5) / 10 * 10;
    s.gain = Position(page, IDC_GAIN);
    s.springGain = Position(page, IDC_SPRING);
    s.damperGain = Position(page, IDC_DAMPER);
    s.autocenterStrength = Position(page, IDC_AUTOCENTER_STRENGTH);
    s.autocenter = IsDlgButtonChecked(page, IDC_AUTOCENTER) == BST_CHECKED;
    s.hardwareFriction = IsDlgButtonChecked(page, IDC_HARDWARE_FRICTION) == BST_CHECKED;
    s.invert = IsDlgButtonChecked(page, IDC_INVERT) == BST_CHECKED;
    return s;
}

INT_PTR CALLBACK PageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
        for (const auto& slider : Sliders) {
            bool range = slider.control == IDC_RANGE;
            SendDlgItemMessageW(page, slider.control, TBM_SETRANGE, TRUE, MAKELPARAM(slider.min, slider.max));
            SendDlgItemMessageW(page, slider.control, TBM_SETTICFREQ, 10, 0);
            SendDlgItemMessageW(page, slider.control, TBM_SETLINESIZE, 0, range ? 10 : 1);
            SendDlgItemMessageW(page, slider.control, TBM_SETPAGESIZE, 0, range ? 90 : 10);
        }
        Show(page, Settings::Load());
        return TRUE;
    case WM_HSCROLL:
        for (const auto& slider : Sliders)
            if ((HWND)lParam == GetDlgItem(page, slider.control))
                ShowValue(page, slider);
        PropSheet_Changed(GetParent(page), page);
        return TRUE;
    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED)
            return TRUE;
        switch (LOWORD(wParam)) {
        case IDC_AUTOCENTER:
            UpdateAutocenterControls(page);
            PropSheet_Changed(GetParent(page), page);
            break;
        case IDC_HARDWARE_FRICTION:
        case IDC_INVERT:
            PropSheet_Changed(GetParent(page), page);
            break;
        case IDC_DEFAULTS:
            Show(page, Settings{});
            PropSheet_Changed(GetParent(page), page);
            break;
        case IDC_TEST_LEFT:
        case IDC_TEST_RIGHT:
        case IDC_TEST_RUMBLE:
        case IDC_TEST_SPRING:
            if (!g_test.Play(page, LOWORD(wParam)))
                MessageBoxW(page, L"The wheel is not available for force feedback. Close any game that uses it and try again.",
                            L"Force Feedback", MB_ICONWARNING);
            break;
        }
        return TRUE;
    case WM_DESTROY:
        g_test.Stop();
        return FALSE;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == PSN_APPLY) {
            Settings s = Read(page);
            s.Save();
            ApplyWheelSettings(s);
            SetWindowLongPtrW(page, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

}  // namespace

PropSheet::PropSheet(HINSTANCE module) : module_(module)
{
    InterlockedIncrement(&g_objects);
    // Keep Windows' own Test and Settings (calibration) pages: the sheet we register replaces the default one
    if (FAILED(CoCreateInstance(CLSID_GameControllerDefaultSheet, nullptr, CLSCTX_INPROC_SERVER, IID_IDIGameCntrlPropSheet,
                                reinterpret_cast<void**>(&defaultSheet_))))
        defaultSheet_ = nullptr;
}

PropSheet::~PropSheet()
{
    if (defaultSheet_)
        defaultSheet_->Release();
    InterlockedDecrement(&g_objects);
}

STDMETHODIMP PropSheet::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDIGameCntrlPropSheet) {
        *ppv = static_cast<IDIGameCntrlPropSheet*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) PropSheet::AddRef() { return InterlockedIncrement(&refs_); }

STDMETHODIMP_(ULONG) PropSheet::Release()
{
    ULONG refs = InterlockedDecrement(&refs_);
    if (!refs)
        delete this;
    return refs;
}

STDMETHODIMP PropSheet::SetID(USHORT id)
{
    id_ = id;
    if (defaultSheet_)
        defaultSheet_->SetID(id);
    return S_OK;
}

void PropSheet::Build()
{
    if (built_)
        return;
    built_ = true;
    sheet_.dwSize = sizeof(sheet_);
    sheet_.lpwszSheetCaption = const_cast<LPWSTR>(L"Logitech wheel");
    DIGCSHEETINFO* defaults = nullptr;
    DIGCPAGEINFO* defaultPages = nullptr;
    if (defaultSheet_ && SUCCEEDED(defaultSheet_->GetSheetInfo(&defaults)) && defaults &&
        SUCCEEDED(defaultSheet_->GetPageInfo(&defaultPages)) && defaultPages) {
        sheet_ = *defaults;
        pages_.assign(defaultPages, defaultPages + defaults->nNumPages);
    }
    DIGCPAGEINFO page = { sizeof(DIGCPAGEINFO) };
    page.lpwszPageTitle = const_cast<LPWSTR>(L"Force Feedback");
    page.fpPageProc = PageProc;
    page.lpwszTemplate = MAKEINTRESOURCEW(IDD_FORCE);
    page.hInstance = module_;
    pages_.push_back(page);
    sheet_.nNumPages = (USHORT)pages_.size();
    Log("property sheet for joystick %u: %u pages", id_, sheet_.nNumPages);
}

STDMETHODIMP PropSheet::GetSheetInfo(DIGCSHEETINFO** sheet)
{
    if (!sheet)
        return E_POINTER;
    Build();
    *sheet = &sheet_;
    return S_OK;
}

STDMETHODIMP PropSheet::GetPageInfo(DIGCPAGEINFO** pages)
{
    if (!pages)
        return E_POINTER;
    Build();
    *pages = pages_.data();
    return S_OK;
}
