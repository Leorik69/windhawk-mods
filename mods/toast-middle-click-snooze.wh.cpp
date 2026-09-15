// ==WindhawkMod==
// @id              toast-middle-click-snooze
// @name            Toast Middle-Click Snooze
// @description     Middle-click a Windows toast to dismiss it and get a reminder after N minutes (best-effort snooze)
// @version         1.0.0
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         ShellExperienceHost.exe
// @include         explorer.exe
// @include         ShellHost.exe
// @architecture    x86-64
// @compilerOptions -luser32 -lshell32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Toast Middle-Click Snooze

Middle-click a notification toast to hide/close it and schedule a local reminder
after a configurable delay (15 / 30 / 60 minutes).

## How it works
- Installs a `WH_MOUSE_LL` hook
- On middle-button down, hit-tests the window under the cursor
- Matches toast-like classes (`Windows.UI.Core.CoreWindow`, toast frame heuristics)
- Posts close/hide to the toast window and starts a one-shot timer
- Reminder is delivered via tray balloon (`Shell_NotifyIcon`) or `MessageBoxW`
  fallback when the OS Action Center snooze API is unavailable

## Settings
- **enabled** — master switch
- **snoozeMinutes** — 15, 30, or 60

## Limitations
True Action Center / Windows Notification snooze APIs are not publicly stable
for third-party injection. This mod provides **best-effort local snooze**:
the original toast content cannot always be re-raised through the system
Notification Center; you get a Windhawk/mod reminder instead. Toast window
classes differ across Windows 11 builds — heuristics may miss some toasts.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch for middle-click snooze
- snoozeMinutes: 15
  $name: Snooze minutes
  $description: Delay before reminder (use 15, 30, or 60)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

static bool g_enabled = true;
static int g_snoozeMinutes = 15;

static HHOOK g_mouseHook = nullptr;
static HWND g_msgWnd = nullptr;
static const UINT WM_TRAYICON = WM_APP + 80;
static const UINT WM_SNOOZE_FIRE = WM_APP + 81;
static const UINT TRAY_UID = 0x746d6373; // 'tmcs'
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;

struct SnoozeJob {
    UINT_PTR timerId;
    std::wstring summary;
};
static std::mutex g_jobsMutex;
static std::vector<SnoozeJob> g_jobs;
static std::atomic<UINT_PTR> g_nextTimerId{1000};
static std::wstring g_lastToastText;

static void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    g_snoozeMinutes = Wh_GetIntSetting(L"snoozeMinutes");
    // Snap to allowed values
    if (g_snoozeMinutes <= 20) g_snoozeMinutes = 15;
    else if (g_snoozeMinutes <= 45) g_snoozeMinutes = 30;
    else g_snoozeMinutes = 60;
}

static bool IsToastLikeWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);

    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0) return true;
    if (_wcsicmp(cls, L"Windows.UI.Input.InputSite.WindowClass") == 0) return true;
    if (wcsstr(cls, L"Toast") != nullptr) return true;
    if (wcsstr(cls, L"Notification") != nullptr) return true;
    if (_wcsicmp(cls, L"ApplicationFrameWindow") == 0) {
        // Narrow: only if title suggests toast host
        if (wcsstr(title, L"New notification") || wcsstr(title, L"Notification"))
            return true;
    }
    // Win11 toast sometimes under DesktopWindowXamlSource host
    if (wcsstr(cls, L"Xaml_WindowedPopupClass") != nullptr) return true;
    if (_wcsicmp(cls, L"Shell_TrayWnd") == 0) return false;
    return false;
}

static HWND FindToastRoot(HWND hwnd) {
    HWND cur = hwnd;
    HWND best = nullptr;
    for (int i = 0; i < 8 && cur; ++i) {
        if (IsToastLikeWindow(cur)) best = cur;
        HWND parent = GetParent(cur);
        if (!parent) {
            // Try owner / root
            HWND root = GetAncestor(cur, GA_ROOT);
            if (root && root != cur && IsToastLikeWindow(root))
                best = root;
            break;
        }
        cur = parent;
    }
    if (!best && IsToastLikeWindow(hwnd)) best = hwnd;
    return best;
}

static std::wstring GetWindowTextStr(HWND hwnd) {
    wchar_t buf[512]{};
    GetWindowTextW(hwnd, buf, 512);
    return buf;
}

static void EnsureTray(HWND hwnd) {
    if (g_trayAdded) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    wcsncpy_s(g_nid.szTip, L"Toast Snooze", _TRUNCATE);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

static void RemoveTray() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

static void ShowReminderBalloon(const std::wstring& summary) {
    EnsureTray(g_msgWnd);
    if (g_trayAdded) {
        g_nid.uFlags = NIF_INFO | NIF_TIP | NIF_MESSAGE | NIF_ICON;
        wcsncpy_s(g_nid.szInfoTitle, L"Snoozed notification", _TRUNCATE);
        std::wstring body = summary.empty() ? L"Your snoozed toast reminder." : summary;
        wcsncpy_s(g_nid.szInfo, body.c_str(), _TRUNCATE);
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.szInfo[0] = 0;
        g_nid.szInfoTitle[0] = 0;
    } else {
        std::wstring body = L"Snoozed reminder:\n";
        body += summary.empty() ? L"(toast)" : summary;
        MessageBoxW(nullptr, body.c_str(), L"Toast Snooze", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }
}

static void DismissToast(HWND toast) {
    if (!toast || !IsWindow(toast)) return;
    // Try graceful close, then hide
    PostMessageW(toast, WM_CLOSE, 0, 0);
    ShowWindow(toast, SW_HIDE);
    // Some toast hosts respond to ESC / AppCommands
    PostMessageW(toast, WM_SYSCOMMAND, SC_CLOSE, 0);
    Wh_Log(L"Dismissed toast hwnd=%p", toast);
}

static void ScheduleSnooze(const std::wstring& summary) {
    if (!g_msgWnd) return;
    UINT_PTR id = g_nextTimerId.fetch_add(1);
    UINT ms = (UINT)g_snoozeMinutes * 60u * 1000u;
    if (!SetTimer(g_msgWnd, id, ms, nullptr)) {
        Wh_Log(L"SetTimer failed for snooze");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        g_jobs.push_back({id, summary});
    }
    Wh_Log(L"Snooze scheduled: %d min, timer=%llu", g_snoozeMinutes, (unsigned long long)id);

    // Immediate feedback balloon
    EnsureTray(g_msgWnd);
    if (g_trayAdded) {
        wchar_t info[128];
        swprintf_s(info, L"Reminder in %d minutes.", g_snoozeMinutes);
        g_nid.uFlags = NIF_INFO | NIF_TIP | NIF_MESSAGE | NIF_ICON;
        wcsncpy_s(g_nid.szInfoTitle, L"Toast snoozed", _TRUNCATE);
        wcsncpy_s(g_nid.szInfo, info, _TRUNCATE);
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.szInfo[0] = 0;
        g_nid.szInfoTitle[0] = 0;
    }
}

static void OnSnoozeTimer(UINT_PTR id) {
    KillTimer(g_msgWnd, id);
    std::wstring summary;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it) {
            if (it->timerId == id) {
                summary = it->summary;
                g_jobs.erase(it);
                break;
            }
        }
    }
    Wh_Log(L"Snooze fired timer=%llu", (unsigned long long)id);
    ShowReminderBalloon(summary);
}

static bool HandleMiddleClick(POINT pt) {
    HWND under = WindowFromPoint(pt);
    if (!under) return false;
    HWND toast = FindToastRoot(under);
    if (!toast) {
        // Also check physical cursor window chain via ChildWindowFromPoint
        HWND root = GetAncestor(under, GA_ROOT);
        toast = FindToastRoot(root);
    }
    if (!toast) return false;

    g_lastToastText = GetWindowTextStr(toast);
    if (g_lastToastText.empty())
        g_lastToastText = GetWindowTextStr(under);
    if (g_lastToastText.empty())
        g_lastToastText = L"Notification";

    DismissToast(toast);
    ScheduleSnooze(g_lastToastText);
    return true;
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_enabled && wParam == WM_MBUTTONDOWN) {
        auto* hs = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (HandleMiddleClick(hs->pt)) {
            return 1; // eat middle-click so it doesn't pass through oddly
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            OnSnoozeTimer((UINT_PTR)wParam);
            return 0;
        case WM_TRAYICON:
            return 0;
        case WM_DESTROY: {
            std::lock_guard<std::mutex> lock(g_jobsMutex);
            for (auto& j : g_jobs) KillTimer(hwnd, j.timerId);
            g_jobs.clear();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL Wh_ModInit() {
    LoadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindhawkToastSnoozeMsg";
    RegisterClassExW(&wc);

    g_msgWnd = CreateWindowExW(0, L"WindhawkToastSnoozeMsg", L"", WS_OVERLAPPED,
                               0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_msgWnd) {
        Wh_Log(L"Failed to create message window");
        return FALSE;
    }

    EnsureTray(g_msgWnd);

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                    GetModuleHandleW(nullptr), 0);
    if (!g_mouseHook) {
        Wh_Log(L"WH_MOUSE_LL failed");
        // Still allow mod to load; user can reload
    }

    Wh_Log(L"toast-middle-click-snooze init (snooze=%d min, enabled=%d)",
           g_snoozeMinutes, (int)g_enabled);
    Wh_Log(L"Note: true Action Center snooze API unavailable; using local reminder");
    return TRUE;
}

void Wh_ModUninit() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    if (g_msgWnd) {
        {
            std::lock_guard<std::mutex> lock(g_jobsMutex);
            for (auto& j : g_jobs) KillTimer(g_msgWnd, j.timerId);
            g_jobs.clear();
        }
        RemoveTray();
        DestroyWindow(g_msgWnd);
        g_msgWnd = nullptr;
    }
    UnregisterClassW(L"WindhawkToastSnoozeMsg", GetModuleHandleW(nullptr));
    Wh_Log(L"toast-middle-click-snooze uninit");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    Wh_Log(L"Settings changed: enabled=%d snoozeMinutes=%d",
           (int)g_enabled, g_snoozeMinutes);
}
