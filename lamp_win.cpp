// AK820 Knob -> ESP LED Controller (C++ / Windows) - System Tray Edition
//
// This is a background tray app: no console window, a small status icon in
// the system tray (color-coded: gray = off, amber = on, split amber/white =
// "Both" mode), a right-click menu, and Windows toast/balloon notifications
// instead of console prints. It registers itself to start with Windows on
// first run (toggle-able from the tray menu).
//
// Behavior:
//   Ctrl + Knob            -> Warm  (Ctrl+Shift+Knob -> White), unless toggled (see below)
//   Ctrl + Press           -> Toggle which combo controls Warm vs White. Fires
//                             INSTANTLY on press (no wait) for a snappy feel.
//                             (also cancels "Both" mode if it's currently active)
//   Ctrl + Long Press      -> Power On/Off. Fires the instant the hold crosses the
//                             threshold WHILE STILL HELD - not on release. Note:
//                             turning Ctrl+Knob while off auto-wakes the LEDs too
//                             (restores last levels, then applies the turn) - no
//                             need to long-press first just to start adjusting.
//   Ctrl + Double Press    -> (2nd press within DOUBLE_PRESS_WINDOW_MS) Activates
//                             "Both" mode for BOTH_MODE_DURATION_MS: Ctrl+Knob
//                             (shift ignored) drives Warm and White together.
//                             Cancel early with a single Ctrl+Press.
//
// Rotation feels smoother via a simple speed-based step size:
//   - First tick of a new turning gesture (after a pause)  -> STEP_FIRST
//   - Slow/deliberate continued turning                    -> STEP_SLOW (fine control)
//   - Fast continuous turning                               -> STEP_FAST
//
// Volume conflict fix:
//   A WH_KEYBOARD_LL hook intercepts VK_VOLUME_UP / VK_VOLUME_DOWN / VK_VOLUME_MUTE
//   and swallows them (returns 1, doesn't call the next hook) whenever Ctrl is held,
//   so Windows never changes system volume for those combos. When Ctrl is NOT held,
//   the hook passes the keys through untouched -> knob behaves as a normal volume knob.
//   The hook is installed on the main GUI thread, which already pumps a message loop
//   for the tray icon - no separate hook thread needed.
//
// Tray icon + menu:
//   Left double-click  -> quick power on/off toggle
//   Right-click        -> context menu (status, swap mapping, power, Both mode,
//                          "Start with Windows" checkbox, Exit)
//   The icon is drawn procedurally with GDI+ (no .ico resource needed) and updates
//   live to reflect state; the tooltip shows current Warm/White percentages.
//
// Runs in the background at all times: the HID thread blocks in hid_read_timeout
// (no busy-spin), and the both-mode watcher wakes up just 5x/second.
//
// Build: create a "Desktop Application" (Windows Desktop Wizard) project in
// Visual Studio 2022, delete its generated .cpp/.rc/icon files, add this single
// main.cpp, and add a vcpkg.json (see README.md) so hidapi is picked up
// automatically. No resource script needed - the tray icon is generated at
// runtime. See README.md for full step-by-step instructions.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <hidapi/hidapi.h>

#include <objidl.h>
#include <ole2.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

// ------------------------- Config -------------------------

static const std::string ESP_IP = "192.168.0.212";

// Speed-based rotation step. Which one applies is decided in onRotate() based
// on the gap between successive ticks (see FAST_THRESHOLD_MS / PAUSE_THRESHOLD_MS).
constexpr int STEP_FIRST = 2; // first tick of a new turning gesture
constexpr int STEP_SLOW = 1; // slow/deliberate turning -> fine control
constexpr int STEP_FAST = 5; // fast continuous turning

constexpr int FAST_THRESHOLD_MS = 120;  // tick gap smaller than this counts as "fast"
constexpr int PAUSE_THRESHOLD_MS = 400; // tick gap bigger than this starts a "new gesture"

// HID consumer-control usage codes seen from the device
constexpr int CODE_VOL_UP = 233;  // 0xE9 Volume Increment
constexpr int CODE_VOL_DN = 234;  // 0xEA Volume Decrement
constexpr int CODE_MUTE = 226;    // 0xE2 Mute (knob press)
constexpr int CODE_NONE = 0;

constexpr unsigned short VID = 0x320F;
constexpr unsigned short PID = 0x505B;

// Timing (all adjustable)
constexpr int LONG_PRESS_MS = 600;           // hold longer than this = long press (fires live, while held)
constexpr int DOUBLE_PRESS_WINDOW_MS = 1000; // 2nd press must arrive within this to count as a double-press
constexpr int BOTH_MODE_DURATION_MS = 30000; // "Both" mode auto-expires after this long

// Tray / UI
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR TRAY_ICON_ID = 1;
constexpr UINT IDM_TOGGLE = 1001;
constexpr UINT IDM_POWER = 1002;
constexpr UINT IDM_BOTH = 1003;
constexpr UINT IDM_AUTOSTART = 1004;
constexpr UINT IDM_EXIT = 1005;

const wchar_t* RUN_KEY_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* RUN_VALUE_NAME = L"AK820Controller";
const wchar_t* APP_KEY_PATH = L"Software\\AK820Controller";
const wchar_t* CONFIGURED_VALUE = L"AutostartConfigured";

// ------------------------- State -------------------------

std::recursive_mutex g_mutex;
int g_warm = 0;
int g_white = 0;

bool g_toggled = false;   // false: Ctrl->Warm, Ctrl+Shift->White | true: swapped
bool g_ledsOn = true;
int g_savedWarm = 0, g_savedWhite = 0;

std::atomic<bool> g_bothMode{ false };
std::chrono::steady_clock::time_point g_bothModeExpiry;

std::atomic<bool> g_running{ true };

// click/press bookkeeping
std::atomic<bool> g_buttonDown{ false };
std::atomic<bool> g_buttonCtrlHeld{ false }; // was Ctrl held when the button went down?
std::atomic<bool> g_longPressFired{ false }; // did the live long-press timer already fire this press?
std::chrono::steady_clock::time_point g_buttonDownTime;
std::atomic<long long> g_pressEpoch{ 0 }; // invalidates stale long-press timers on release

bool g_pendingSingleClick = false;
std::chrono::steady_clock::time_point g_lastClickTime;
std::atomic<long long> g_clickEpoch{ 0 };

// rotation speed tracking (for the smooth accelerated step size)
std::chrono::steady_clock::time_point g_lastRotateTime;
int g_lastRotateDir = 0;
bool g_hasLastRotate = false;

HHOOK g_hook = nullptr;
HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_nid{};
HICON g_currentIcon = nullptr;

// ------------------------- Helpers -------------------------

bool ctrlDown() { return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; }
bool shiftDown() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

std::wstring toWide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Minimal WinHTTP GET with a ~1s timeout
std::string httpGet(const std::wstring& pathAndQuery) {
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"AK820Controller/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    WinHttpSetTimeouts(hSession, 1000, 1000, 1000, 1000);

    std::wstring host = toWide(ESP_IP);
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathAndQuery.c_str(),
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hRequest) {
            BOOL sent = WinHttpSendRequest(hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            if (sent && WinHttpReceiveResponse(hRequest, NULL)) {
                DWORD avail = 0;
                do {
                    avail = 0;
                    WinHttpQueryDataAvailable(hRequest, &avail);
                    if (avail == 0) break;
                    std::string buf(avail, 0);
                    DWORD read = 0;
                    WinHttpReadData(hRequest, &buf[0], avail, &read);
                    result.append(buf, 0, read);
                } while (avail > 0);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

int getPercent(int led) {
    std::wstringstream ss;
    ss << L"/getPWM?led=" << led;
    std::string resp = httpGet(ss.str());
    try {
        int pwm = std::stoi(resp);
        return (int)std::lround(pwm * 100.0 / 255.0);
    }
    catch (...) {
        return 0;
    }
}

int setLed(int led, int value) {
    value = std::max(0, std::min(100, value));
    std::wstringstream ss;
    ss << L"/setPWM?led=" << led << L"&value=" << value;
    httpGet(ss.str());
    return value;
}

// ------------------------- Autostart (HKCU Run key) -------------------------

std::wstring getExePathQuoted() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return std::wstring(L"\"") + path + L"\"";
}

bool isAutostartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LONG res = RegQueryValueExW(hKey, RUN_VALUE_NAME, NULL, &type, (LPBYTE)buf, &size);
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}

void setAutostart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;
    if (enable) {
        std::wstring exePath = getExePathQuoted();
        RegSetValueExW(hKey, RUN_VALUE_NAME, 0, REG_SZ,
            (const BYTE*)exePath.c_str(), (DWORD)((exePath.size() + 1) * sizeof(wchar_t)));
    }
    else {
        RegDeleteValueW(hKey, RUN_VALUE_NAME);
    }
    RegCloseKey(hKey);
}

bool isFirstRun() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_KEY_PATH, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return true;
    DWORD val = 0, size = sizeof(val), type = 0;
    LONG res = RegQueryValueExW(hKey, CONFIGURED_VALUE, NULL, &type, (LPBYTE)&val, &size);
    RegCloseKey(hKey);
    return res != ERROR_SUCCESS;
}

void markConfigured() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_KEY_PATH, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = 1;
        RegSetValueExW(hKey, CONFIGURED_VALUE, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}

// ------------------------- Low-level keyboard hook -------------------------
// Suppresses the OS's own volume-up/down/mute handling whenever Ctrl is held,
// so the knob doesn't fight with Windows' built-in volume control.

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (kb->vkCode == VK_VOLUME_UP ||
            kb->vkCode == VK_VOLUME_DOWN ||
            kb->vkCode == VK_VOLUME_MUTE) {
            if (ctrlDown()) {
                return 1; // swallow: Windows never sees this key event
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ------------------------- Tray icon rendering (GDI+) -------------------------

HICON makeDotIcon(bool on, bool both) {
    const int size = 32;
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    Graphics gfx(&bmp);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.Clear(Color(0, 0, 0, 0)); // transparent background

    const float pad = 4.0f;
    const float d = (float)size - pad * 2;

    if (!on) {
        SolidBrush brush(Color(255, 90, 90, 90));
        gfx.FillEllipse(&brush, pad, pad, d, d);
        Pen pen(Color(255, 60, 60, 60), 2.0f);
        gfx.DrawEllipse(&pen, pad, pad, d, d);
    }
    else if (both) {
        SolidBrush amber(Color(255, 255, 176, 59));
        SolidBrush white(Color(255, 214, 236, 255));
        gfx.FillPie(&amber, pad, pad, d, d, 90.0f, 180.0f);
        gfx.FillPie(&white, pad, pad, d, d, 270.0f, 180.0f);
        Pen pen(Color(255, 200, 200, 200), 1.0f);
        gfx.DrawEllipse(&pen, pad, pad, d, d);
    }
    else {
        SolidBrush brush(Color(255, 255, 176, 59));
        gfx.FillEllipse(&brush, pad, pad, d, d);
    }

    HICON hIcon = nullptr;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

void showBalloon(const std::wstring& title, const std::wstring& text) {
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; // clear so later modifies don't re-fire the balloon
}

// Cheap: just refreshes the tooltip text (call this often, e.g. every rotate tick)
void refreshTooltip() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    std::wstringstream tip;
    tip << L"AK820 Controller\n"
        << L"Warm " << g_warm << L"%  |  White " << g_white << L"%\n"
        << (g_toggled ? L"Ctrl->White, Ctrl+Shift->Warm" : L"Ctrl->Warm, Ctrl+Shift->White");
    if (g_bothMode.load()) tip << L"\nBoth Mode active";
    if (!g_ledsOn) tip << L"\n(Off)";

    wcsncpy_s(g_nid.szTip, tip.str().c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// Heavier: regenerates the icon bitmap too - call only when on/off/Both actually changes
void refreshIcon() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    HICON newIcon = makeDotIcon(g_ledsOn, g_bothMode.load());
    HICON oldIcon = g_nid.hIcon;

    g_nid.hIcon = newIcon;
    g_currentIcon = newIcon;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    if (oldIcon) DestroyIcon(oldIcon);
}

void updateTray() {
    refreshIcon();
    refreshTooltip();
}

// ------------------------- LED control actions -------------------------

void activateBothMode() {
    g_bothMode = true;
    g_bothModeExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(BOTH_MODE_DURATION_MS);
    updateTray();
    std::wstringstream msg;
    msg << L"Active for " << (BOTH_MODE_DURATION_MS / 1000) << L"s - Ctrl+Knob now moves Warm & White together.";
    showBalloon(L"Both Mode", msg.str());
}

// Just clears the "waiting for a possible 2nd press" flag once the double-press
// window has passed - no LED action needed here, since the toggle already
// happened instantly in onShortPress() below.
void scheduleClearPendingClick() {
    long long myEpoch = ++g_clickEpoch;
    std::thread([myEpoch]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(DOUBLE_PRESS_WINDOW_MS));
        if (g_clickEpoch.load() == myEpoch) {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            g_pendingSingleClick = false;
        }
        }).detach();
}

void onShortPress() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);

    if (g_bothMode.load()) {
        g_bothMode = false;
        g_pendingSingleClick = false;
        updateTray();
        showBalloon(L"Both Mode", L"Cancelled by Ctrl+Press.");
        return;
    }

    auto now = std::chrono::steady_clock::now();
    long long sinceLast = g_pendingSingleClick
        ? std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastClickTime).count()
        : (long long)DOUBLE_PRESS_WINDOW_MS + 1;

    if (g_pendingSingleClick && sinceLast <= DOUBLE_PRESS_WINDOW_MS) {
        // 2nd press arrived in time -> this is a double-press. The 1st press
        // already applied a toggle optimistically (for instant feedback), so
        // revert that before switching into Both mode.
        g_pendingSingleClick = false;
        g_clickEpoch++; // cancel the pending clear-timer
        g_toggled = !g_toggled;
        activateBothMode();
    }
    else {
        // Apply the toggle immediately - no waiting, feels instant.
        g_toggled = !g_toggled;
        updateTray();
        showBalloon(L"Mapping Changed",
            g_toggled ? L"Ctrl -> White, Ctrl+Shift -> Warm" : L"Ctrl -> Warm, Ctrl+Shift -> White");
        g_pendingSingleClick = true;
        g_lastClickTime = now;
        scheduleClearPendingClick();
    }
}

void onLongPress() {
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    if (g_ledsOn) {
        g_savedWarm = g_warm;
        g_savedWhite = g_white;
        g_warm = setLed(3, 0);
        g_white = setLed(4, 0);
        g_ledsOn = false;
        updateTray();
        showBalloon(L"Power", L"LEDs OFF");
    }
    else {
        g_warm = setLed(3, g_savedWarm);
        g_white = setLed(4, g_savedWhite);
        g_ledsOn = true;
        updateTray();
        std::wstringstream msg;
        msg << L"LEDs ON (Warm=" << g_warm << L" White=" << g_white << L")";
        showBalloon(L"Power", msg.str());
    }
}

void manualToggleAutostart() {
    bool now = !isAutostartEnabled();
    setAutostart(now);
    markConfigured();
    showBalloon(L"Startup",
        now ? L"AK820 Controller will start with Windows" : L"Removed from Windows startup");
}

// Picks a step size based on how fast the knob is being turned:
//   - long pause since the last tick, or a direction change -> STEP_FIRST
//   - ticks arriving in quick succession                    -> STEP_FAST
//   - steady, moderate-speed ticks                          -> STEP_SLOW (fine control)
int computeStep(int dir) {
    auto now = std::chrono::steady_clock::now();
    int step;

    if (!g_hasLastRotate || dir != g_lastRotateDir) {
        step = STEP_FIRST;
    }
    else {
        long long gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_lastRotateTime).count();
        if (gapMs > PAUSE_THRESHOLD_MS) {
            step = STEP_FIRST;
        }
        else if (gapMs < FAST_THRESHOLD_MS) {
            step = STEP_FAST;
        }
        else {
            step = STEP_SLOW;
        }
    }

    g_lastRotateTime = now;
    g_lastRotateDir = dir;
    g_hasLastRotate = true;
    return step;
}

void onRotate(int dir) { // dir = +1 or -1
    std::lock_guard<std::recursive_mutex> lk(g_mutex);
    bool stateChanged = false;

    if (g_bothMode.load() && std::chrono::steady_clock::now() > g_bothModeExpiry) {
        g_bothMode = false;
        stateChanged = true;
        showBalloon(L"Both Mode", L"Expired.");
    }

    if (!g_ledsOn) {
        // Rotating while "off" auto-wakes the LEDs (restores last levels)
        // instead of requiring a separate long-press first - faster to use.
        g_warm = setLed(3, g_savedWarm);
        g_white = setLed(4, g_savedWhite);
        g_ledsOn = true;
        stateChanged = true;
        showBalloon(L"Power", L"LEDs ON (auto, via knob)");
    }

    int step = computeStep(dir);

    if (g_bothMode.load()) {
        g_warm = setLed(3, g_warm + dir * step);
        g_white = setLed(4, g_white + dir * step);
    }
    else {
        bool wantWhite = shiftDown();
        if (g_toggled) wantWhite = !wantWhite;

        if (wantWhite) {
            g_white = setLed(4, g_white + dir * step);
        }
        else {
            g_warm = setLed(3, g_warm + dir * step);
        }
    }

    if (stateChanged) refreshIcon();
    refreshTooltip(); // cheap - keep the displayed percentages fresh every tick
}

// ------------------------- Device discovery -------------------------

hid_device* openConsumerInterface() {
    hid_device_info* devs = hid_enumerate(VID, PID);
    hid_device_info* target = nullptr;

    for (hid_device_info* p = devs; p; p = p->next) {
        if (p->usage_page == 0x0C) { target = p; break; }
    }
    if (!target) {
        for (hid_device_info* p = devs; p; p = p->next) {
            if (p->path && strstr(p->path, "col03")) { target = p; break; }
        }
    }

    hid_device* dev = nullptr;
    if (target) {
        dev = hid_open_path(target->path);
    }
    if (devs) hid_free_enumeration(devs);
    return dev;
}

// ------------------------- HID reading thread -------------------------
// Dynamically manages the HID device connection. If it disconnects, it closes the 
// handle cleanly and attempts to re-open it every 1 second without breaking the runtime.

void hidThreadFunc() {
    hid_device* dev = nullptr;
    unsigned char buf[8];
    int prevCode = -1;

    while (g_running.load()) {
        // If device is not open, attempt reconnection loop
        if (!dev) {
            dev = openConsumerInterface();
            if (!dev) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        int res = hid_read_timeout(dev, buf, sizeof(buf), 100);

        // A negative result implies an interface error or physical disconnection
        if (res < 0) {
            hid_close(dev);
            dev = nullptr;
            prevCode = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }
        if (res == 0) continue; // read timed out, try again next loop
        if (res < 2) continue;

        int code = buf[1];
        if (code == prevCode) continue; // de-dupe repeated identical reports
        prevCode = code;

        if (code == CODE_NONE) {
            if (g_buttonDown.load()) {
                g_buttonDown = false;
                g_pressEpoch++; // invalidate any still-pending long-press timer

                if (!g_buttonCtrlHeld.load()) continue; // wasn't a "Ctrl + Press", ignore

                if (g_longPressFired.load()) {
                    // Long-press action already fired live, while the button
                    // was still held - nothing further to do on release.
                }
                else {
                    onShortPress();
                }
            }
            continue;
        }

        if (code == CODE_MUTE) {
            g_buttonDown = true;
            g_buttonCtrlHeld = ctrlDown();
            g_longPressFired = false;
            g_buttonDownTime = std::chrono::steady_clock::now();

            if (g_buttonCtrlHeld.load()) {
                long long myEpoch = ++g_pressEpoch;
                // Fires the moment the hold crosses LONG_PRESS_MS, while the
                // button is still down - not on release.
                std::thread([myEpoch]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(LONG_PRESS_MS));
                    if (g_pressEpoch.load() == myEpoch && g_buttonDown.load()) {
                        g_longPressFired = true;
                        onLongPress();
                    }
                    }).detach();
            }
            continue;
        }

        if (!ctrlDown()) continue; // not our combo - let default volume behavior occur

        if (code == CODE_VOL_UP) {
            onRotate(+1);
        }
        else if (code == CODE_VOL_DN) {
            onRotate(-1);
        }
    }

    if (dev) {
        hid_close(dev);
    }
}

void bothModeWatcher() {
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (g_bothMode.load() && std::chrono::steady_clock::now() > g_bothModeExpiry) {
            g_bothMode = false;
            refreshIcon();
            refreshTooltip();
            showBalloon(L"Both Mode", L"Expired.");
        }
    }
}

// ------------------------- Tray menu + window procedure -------------------------

void showContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();

    wchar_t statusBuf[128];
    {
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        swprintf_s(statusBuf, L"Warm %d%%   |   White %d%%", g_warm, g_white);
    }
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, statusBuf);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    wchar_t toggleBuf[80];
    {
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        swprintf_s(toggleBuf, L"Swap Warm/White  (Ctrl -> %s)", g_toggled ? L"White" : L"Warm");
    }
    AppendMenuW(hMenu, MF_STRING, IDM_TOGGLE, toggleBuf);

    bool ledsOn;
    { std::lock_guard<std::recursive_mutex> lk(g_mutex); ledsOn = g_ledsOn; }
    AppendMenuW(hMenu, MF_STRING, IDM_POWER, ledsOn ? L"Turn Off" : L"Turn On");

    bool both = g_bothMode.load();
    AppendMenuW(hMenu, MF_STRING | (both ? MF_CHECKED : 0), IDM_BOTH,
        both ? L"Both Mode active - click to cancel" : L"Activate Both Mode");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (isAutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON: {
        UINT evt = LOWORD(lParam);
        if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
            showContextMenu(hwnd);
        }
        else if (evt == WM_LBUTTONDBLCLK) {
            onLongPress(); // quick double-click on the icon = power toggle
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TOGGLE:
            onShortPress();
            break;
        case IDM_POWER:
            onLongPress();
            break;
        case IDM_BOTH:
            if (g_bothMode.load()) {
                g_bothMode = false;
                updateTray();
                showBalloon(L"Both Mode", L"Cancelled.");
            }
            else {
                activateBothMode();
            }
            break;
        case IDM_AUTOSTART:
            manualToggleAutostart();
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// ------------------------- Entry point -------------------------

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    // Single instance guard
    HANDLE hSingleInstance = CreateMutexW(NULL, TRUE, L"AK820Controller_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"AK820 Controller is already running - check the system tray.",
            L"AK820 Controller", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    if (hid_init() != 0) {
        MessageBoxW(NULL, L"Failed to initialize the HID subsystem.", L"AK820 Controller", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Getting current states from ESP doesn't require the keyboard to be connected yet!
    g_warm = getPercent(3);
    g_white = getPercent(4);
    g_savedWarm = g_warm;
    g_savedWhite = g_white;

    const wchar_t CLASS_NAME[] = L"AK820ControllerWndClass";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    // Message-only window: no visible UI, just receives tray/menu messages
    g_hwnd = CreateWindowExW(0, CLASS_NAME, L"AK820 Controller", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ICON_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = makeDotIcon(g_ledsOn, false);
    g_currentIcon = g_nid.hIcon;
    wcscpy_s(g_nid.szTip, L"AK820 Controller");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

    updateTray();
    showBalloon(L"AK820 Controller", L"Running in the system tray. Right-click the icon for options.");

    if (isFirstRun()) {
        setAutostart(true);
        markConfigured();
    }

    // Install the low-level keyboard hook on this thread - it already runs
    // the message loop below, which is all a WH_KEYBOARD_LL hook needs.
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

    // Threads handle device lifetimes dynamically now
    std::thread hidThread(hidThreadFunc);
    std::thread watcherThread(bothModeWatcher);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    hidThread.join();
    watcherThread.join();

    if (g_hook) UnhookWindowsHookEx(g_hook);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_currentIcon) DestroyIcon(g_currentIcon);

    hid_exit();

    GdiplusShutdown(gdiplusToken);
    CloseHandle(hSingleInstance);
    return (int)msg.wParam;
}