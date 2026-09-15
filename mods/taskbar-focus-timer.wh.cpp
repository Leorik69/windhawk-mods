// ==WindhawkMod==
// @id              taskbar-focus-timer
// @name            Taskbar Focus Timer
// @description     Pomodoro-style focus timer with tray icon, balloon tips, and Ctrl+Alt+F hotkey; optional clock-area wheel adjust
// @version         1.0.1
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lshell32 -luser32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Focus Timer

A lightweight Pomodoro-style countdown that lives in the Explorer process.

## Features
- Tray icon (Shell_NotifyIcon) with remaining time in the tooltip
- Global hotkey **Ctrl+Alt+F** (configurable via settings flag) to start/pause
- Balloon notifications when a work or break segment ends
- Companion tiny topmost tool window (optional chip) showing MM:SS
- Mouse-wheel over the notification clock area adjusts minutes when the
  cursor is near the system clock (best-effort hit-test)

## Settings
- **workMinutes** — focus segment length (default 25)
- **breakMinutes** — break segment length (default 5)
- **showSeconds** — show seconds in tooltip / chip
- **hotkeyEnabled** — register Ctrl+Alt+F

## Limitations
Win11 XAML taskbar clock tooltip override is fragile across builds; this mod
ships a solid tray + hotkey UX instead of depending on Taskbar.View internals.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- workMinutes: 25
  $name: Work minutes
  $description: Length of a focus segment (1–180)
- breakMinutes: 5
  $name: Break minutes
  $description: Length of a break segment (1–60)
- showSeconds: true
  $name: Show seconds
  $description: Include seconds in tooltip and companion chip
- hotkeyEnabled: true
  $name: Hotkey enabled
  $description: Register Ctrl+Alt+F to start/pause
- showChip: true
  $name: Show companion chip
  $description: Tiny topmost MM:SS chip (off = tray + hotkey only)
- clockWheelAdjust: true
  $name: Clock-area wheel adjust
  $description: Install WH_MOUSE_LL only when enabled; only acts over clock band
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <atomic>
#include <mutex>

enum class TimerPhase { Idle, Work, Break, PausedWork, PausedBreak };

static int g_workMinutes = 25;
static int g_breakMinutes = 5;
static bool g_showSeconds = true;
static bool g_hotkeyEnabled = true;
static bool g_showChip = true;
static bool g_clockWheelAdjust = true;

static const UINT WM_TRAYICON = WM_APP + 42;
static const UINT WM_TICK = WM_APP + 43;
static const UINT HOTKEY_ID = 1;
static const UINT_PTR TIMER_ID = 1;
static const UINT TRAY_UID = 0x74667431; // 'tft1'

static HWND g_msgWnd = nullptr;
static HWND g_chipWnd = nullptr;
static HHOOK g_mouseHook = nullptr;
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;

static std::mutex g_stateMutex;
static TimerPhase g_phase = TimerPhase::Idle;
static int g_remainingSec = 0;
static int g_adjustMinutes = 25;

static void LoadSettings() {
    g_workMinutes = Wh_GetIntSetting(L"workMinutes");
    if (g_workMinutes < 1) g_workMinutes = 1;
    if (g_workMinutes > 180) g_workMinutes = 180;
    g_breakMinutes = Wh_GetIntSetting(L"breakMinutes");
    if (g_breakMinutes < 1) g_breakMinutes = 1;
    if (g_breakMinutes > 60) g_breakMinutes = 60;
    g_showSeconds = Wh_GetIntSetting(L"showSeconds") != 0;
    g_hotkeyEnabled = Wh_GetIntSetting(L"hotkeyEnabled") != 0;
    g_showChip = Wh_GetIntSetting(L"showChip") != 0;
    g_clockWheelAdjust = Wh_GetIntSetting(L"clockWheelAdjust") != 0;
    g_adjustMinutes = g_workMinutes;
}

static std::wstring FormatRemaining(int sec) {
    if (sec < 0) sec = 0;
    int m = sec / 60;
    int s = sec % 60;
    wchar_t buf[64];
    if (g_showSeconds)
        swprintf_s(buf, L"%02d:%02d", m, s);
    else
        swprintf_s(buf, L"%d min", (sec + 59) / 60);
    return buf;
}

static const wchar_t* PhaseLabel(TimerPhase p) {
    switch (p) {
        case TimerPhase::Work: return L"Focus";
        case TimerPhase::Break: return L"Break";
        case TimerPhase::PausedWork: return L"Paused (Focus)";
        case TimerPhase::PausedBreak: return L"Paused (Break)";
        default: return L"Idle";
    }
}

static void UpdateTrayTooltip() {
    if (!g_trayAdded) return;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    std::wstring tip = L"Focus Timer — ";
    tip += PhaseLabel(g_phase);
    if (g_phase != TimerPhase::Idle) {
        tip += L" ";
        tip += FormatRemaining(g_remainingSec);
    } else {
        tip += L" (Ctrl+Alt+F)";
    }
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void UpdateChip() {
    if (!g_chipWnd || !IsWindow(g_chipWnd)) return;
    std::lock_guard<std::mutex> lock(g_stateMutex);
    std::wstring t = PhaseLabel(g_phase);
    if (g_phase != TimerPhase::Idle) {
        t += L" ";
        t += FormatRemaining(g_remainingSec);
    } else {
        t = L"Focus Timer";
    }
    SetWindowTextW(g_chipWnd, t.c_str());
    InvalidateRect(g_chipWnd, nullptr, TRUE);
}

static void ShowBalloon(const wchar_t* title, const wchar_t* body) {
    if (!g_trayAdded) return;
    g_nid.uFlags = NIF_INFO | NIF_TIP | NIF_MESSAGE | NIF_ICON;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, body, _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.szInfo[0] = 0;
    g_nid.szInfoTitle[0] = 0;
}

static void StartWork() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_phase = TimerPhase::Work;
    g_remainingSec = g_adjustMinutes * 60;
    Wh_Log(L"Focus segment started: %d min", g_adjustMinutes);
}

static void StartBreak() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_phase = TimerPhase::Break;
    g_remainingSec = g_breakMinutes * 60;
    Wh_Log(L"Break segment started: %d min", g_breakMinutes);
}

static void TogglePauseOrStart() {
    TimerPhase nextBalloon = TimerPhase::Idle;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        switch (g_phase) {
            case TimerPhase::Idle:
                g_phase = TimerPhase::Work;
                g_remainingSec = g_adjustMinutes * 60;
                nextBalloon = TimerPhase::Work;
                break;
            case TimerPhase::Work:
                g_phase = TimerPhase::PausedWork;
                break;
            case TimerPhase::Break:
                g_phase = TimerPhase::PausedBreak;
                break;
            case TimerPhase::PausedWork:
                g_phase = TimerPhase::Work;
                break;
            case TimerPhase::PausedBreak:
                g_phase = TimerPhase::Break;
                break;
        }
        Wh_Log(L"Toggle -> phase=%s rem=%d", PhaseLabel(g_phase), g_remainingSec);
    }
    if (nextBalloon == TimerPhase::Work)
        ShowBalloon(L"Focus Timer", L"Focus segment started. Stay sharp!");
    UpdateTrayTooltip();
    UpdateChip();
}

static void OnTick() {
    bool finishedWork = false, finishedBreak = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_phase == TimerPhase::Work || g_phase == TimerPhase::Break) {
            if (g_remainingSec > 0) g_remainingSec--;
            if (g_remainingSec <= 0) {
                if (g_phase == TimerPhase::Work) finishedWork = true;
                else finishedBreak = true;
            }
        }
    }
    if (finishedWork) {
        ShowBalloon(L"Focus Timer", L"Work segment done — time for a break!");
        StartBreak();
    } else if (finishedBreak) {
        ShowBalloon(L"Focus Timer", L"Break over — ready for another focus block?");
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_phase = TimerPhase::Idle;
        g_remainingSec = 0;
    }
    UpdateTrayTooltip();
    UpdateChip();
}

static bool PointNearClock(POINT pt) {
    // Best-effort: rightmost ~220px of the primary monitor work area bottom band
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT wa = mi.rcWork;
    int bandTop = wa.bottom - 48;
    int bandLeft = wa.right - 220;
    return pt.x >= bandLeft && pt.x <= wa.right && pt.y >= bandTop && pt.y <= wa.bottom + 48;
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        auto* hs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        POINT pt = hs->pt;
        if (PointNearClock(pt)) {
            short delta = HIWORD(hs->mouseData);
            int step = (delta > 0) ? 1 : -1;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_adjustMinutes += step;
                if (g_adjustMinutes < 1) g_adjustMinutes = 1;
                if (g_adjustMinutes > 180) g_adjustMinutes = 180;
                if (g_phase == TimerPhase::Idle || g_phase == TimerPhase::PausedWork) {
                    // Preview duration in tooltip when idle
                }
                Wh_Log(L"Clock-wheel adjust -> %d min", g_adjustMinutes);
            }
            UpdateTrayTooltip();
            UpdateChip();
            // Do not eat the wheel event for other apps in most cases; still allow
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK ChipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(RGB(32, 32, 36));
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(220, 220, 230));
            wchar_t text[128];
            GetWindowTextW(hwnd, text, 128);
            DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            // Drag without HTCAPTION so LBUTTONUP still receives the click
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, lParam);
            return 0;
        }
        case WM_LBUTTONUP:
            TogglePauseOrStart();
            return 0;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CreateChipWindow() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ChipWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindhawkFocusTimerChip";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int x = GetSystemMetrics(SM_CXSCREEN) - 180;
    int y = GetSystemMetrics(SM_CYSCREEN) - 100;
    g_chipWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"WindhawkFocusTimerChip", L"Focus Timer",
        WS_POPUP | WS_VISIBLE,
        x, y, 160, 36,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (g_chipWnd) {
        SetLayeredWindowAttributes(g_chipWnd, 0, 230, LWA_ALPHA);
        UpdateChip();
    }
}

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, TIMER_ID, 1000, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == TIMER_ID) OnTick();
            return 0;
        case WM_HOTKEY:
            if (wParam == HOTKEY_ID) TogglePauseOrStart();
            return 0;
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP)
                TogglePauseOrStart();
            else if (lParam == WM_RBUTTONUP) {
                // Reset to idle
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_phase = TimerPhase::Idle;
                    g_remainingSec = 0;
                    g_adjustMinutes = g_workMinutes;
                }
                UpdateTrayTooltip();
                UpdateChip();
                ShowBalloon(L"Focus Timer", L"Timer reset.");
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ID);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool AddTrayIcon(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, L"Focus Timer — Idle (Ctrl+Alt+F)", _TRUNCATE);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
    if (g_trayAdded) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    }
    return g_trayAdded;
}

static void RemoveTrayIcon() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

BOOL Wh_ModInit() {
    LoadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindhawkFocusTimerMsg";
    RegisterClassExW(&wc);

    g_msgWnd = CreateWindowExW(0, L"WindhawkFocusTimerMsg", L"", WS_OVERLAPPED,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_msgWnd) {
        Wh_Log(L"Failed to create message window");
        return FALSE;
    }

    AddTrayIcon(g_msgWnd);
    if (g_showChip) CreateChipWindow();

    if (g_hotkeyEnabled) {
        if (!RegisterHotKey(g_msgWnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'F'))
            Wh_Log(L"RegisterHotKey Ctrl+Alt+F failed (maybe in use)");
        else
            Wh_Log(L"Hotkey Ctrl+Alt+F registered");
    }

    if (g_clockWheelAdjust) {
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
        if (!g_mouseHook)
            Wh_Log(L"WH_MOUSE_LL hook failed; clock-wheel adjust disabled");
    } else {
        Wh_Log(L"clock-wheel adjust off — no WH_MOUSE_LL");
    }

    Wh_Log(L"taskbar-focus-timer init (work=%d break=%d)", g_workMinutes, g_breakMinutes);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_msgWnd) {
        UnregisterHotKey(g_msgWnd, HOTKEY_ID);
        RemoveTrayIcon();
        DestroyWindow(g_msgWnd);
        g_msgWnd = nullptr;
    }
    if (g_chipWnd) {
        DestroyWindow(g_chipWnd);
        g_chipWnd = nullptr;
    }
    UnregisterClassW(L"WindhawkFocusTimerMsg", GetModuleHandleW(nullptr));
    UnregisterClassW(L"WindhawkFocusTimerChip", GetModuleHandleW(nullptr));
    Wh_Log(L"taskbar-focus-timer uninit");
}

void Wh_ModSettingsChanged() {
    bool prevHotkey = g_hotkeyEnabled;
    bool prevChip = g_showChip;
    bool prevWheel = g_clockWheelAdjust;
    LoadSettings();
    if (g_msgWnd) {
        if (prevHotkey && !g_hotkeyEnabled)
            UnregisterHotKey(g_msgWnd, HOTKEY_ID);
        else if (!prevHotkey && g_hotkeyEnabled)
            RegisterHotKey(g_msgWnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'F');
    }
    if (prevChip && !g_showChip && g_chipWnd) {
        DestroyWindow(g_chipWnd);
        g_chipWnd = nullptr;
    } else if (!prevChip && g_showChip && !g_chipWnd) {
        CreateChipWindow();
    }
    if (prevWheel && !g_clockWheelAdjust && g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    } else if (!prevWheel && g_clockWheelAdjust && !g_mouseHook) {
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
    }
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_phase == TimerPhase::Idle)
            g_adjustMinutes = g_workMinutes;
    }
    UpdateTrayTooltip();
    UpdateChip();
}