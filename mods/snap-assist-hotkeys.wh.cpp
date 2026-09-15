// ==WindhawkMod==
// @id              snap-assist-hotkeys
// @name            Snap Assist Hotkeys
// @description     Number-key snap targets and optional overlay when Snap Assist is active; maps 1-9 to windows on the current monitor
// @version         1.0.2
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -luser32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Snap Assist Hotkeys

When Windows Snap Assist / Snap Layouts UI is actually visible, press **1–9** to
activate the Nth visible top-level window on the current monitor. An optional
numbered overlay lists candidates.

## How it works
- Tight detection: only arms when a snap overlay is confidently visible
  (process explorer.exe + Snap Assist / Snap Layouts class+title checks;
  MultitaskingViewFrame only with Snap in title). Broad classes like bare `XamlExplorerHostIslandWindow`
  or `ForegroundStaging` alone no longer arm the hook.
- Optional **requireWinKey**: only intercept digits while Win is held (extra
  safety if detection still false-positives on your build).
- WH_KEYBOARD_LL only eats keys 1–9 while armed; otherwise keys pass through.
- Overlay is a layered WS_EX_TOPMOST window; destroyed when Snap Assist ends.

## Settings
- **enableOverlay** — show numbered list overlay
- **enableDigits** — map keys 1–9 to candidates while snap UI is armed
- **maxWindows** — max candidates (1–9)
- **requireWinKey** — only intercept digits while the Windows key is held

## Limitations / mitigations (SL review)
Earlier builds matched XamlExplorerHostIslandWindow / ForegroundStaging too
broadly and could eat digits globally. v1.0.2 narrows detection and adds
requireWinKey so LL hook never swallows 1–9 unless snap overlay is detected
(and optionally Win is held).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enableOverlay: true
  $name: Enable overlay
  $description: Show a numbered list of snap targets
- enableDigits: true
  $name: Enable digit hotkeys
  $description: Map keys 1-9 only while snap overlay is detected (keys pass through otherwise)
- maxWindows: 9
  $name: Max windows
  $description: Maximum number of candidates (1-9)
- requireWinKey: false
  $name: Require Win key
  $description: Only intercept digits while the Windows key is also held (extra false-positive guard)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>

static bool g_enableOverlay = true;
static bool g_enableDigits = true;
static int g_maxWindows = 9;
static bool g_requireWinKey = false;

static HHOOK g_kbHook = nullptr;
static HANDLE g_pollThread = nullptr;
static HANDLE g_stopEvent = nullptr;
static HWND g_overlayWnd = nullptr;

static std::mutex g_mutex;
static bool g_snapActive = false;
static std::vector<HWND> g_candidates;
static std::vector<std::wstring> g_titles;

static void LoadSettings() {
    g_enableOverlay = Wh_GetIntSetting(L"enableOverlay") != 0;
    g_enableDigits = Wh_GetIntSetting(L"enableDigits") != 0;
    g_maxWindows = Wh_GetIntSetting(L"maxWindows");
    if (g_maxWindows < 1) g_maxWindows = 1;
    if (g_maxWindows > 9) g_maxWindows = 9;
    g_requireWinKey = Wh_GetIntSetting(L"requireWinKey") != 0;
}

static bool IsExplorerProcess(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &sz) != 0;
    CloseHandle(h);
    if (!ok) return false;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"explorer.exe") == 0;
}

static bool IsSnapAssistWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsWindowVisible(hwnd)) return false;

    wchar_t cls[256]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);

    // Tight checks only — do NOT match bare XamlExplorerHostIslandWindow or
    // ForegroundStaging (those host many non-snap UIs and caused global digit eat).

    // MultitaskingViewFrame hosts Alt+Tab AND Snap Assist — only arm when
    // title strongly indicates snap (bare MTV = Alt+Tab/Task View, do not eat digits).
    if (_wcsicmp(cls, L"MultitaskingViewFrame") == 0) {
        if (wcsstr(title, L"Snap Assist") != nullptr ||
            wcsstr(title, L"Snap Layouts") != nullptr ||
            wcsstr(title, L"Snap layouts") != nullptr ||
            wcsstr(title, L"Snap") != nullptr)
            return true;
        return false;
    }

    // Explicit Snap in class name (SnapAssistFlyout, etc.)
    if (wcsstr(cls, L"SnapAssist") != nullptr) return true;
    if (wcsstr(cls, L"SnapLayout") != nullptr) return true;

    // Title strongly indicates Snap Assist / Snap Layouts
    if (wcsstr(title, L"Snap Assist") != nullptr) return true;
    if (wcsstr(title, L"Snap Layouts") != nullptr) return true;
    if (wcsstr(title, L"Snap layouts") != nullptr) return true;

    // CoreWindow / XAML island only when explorer-hosted AND title mentions Snap
    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
        _wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0) {
        if ((wcsstr(title, L"Snap") != nullptr || wcsstr(title, L"snap") != nullptr) &&
            IsExplorerProcess(hwnd)) {
            RECT rc{};
            if (GetWindowRect(hwnd, &rc)) {
                int w = rc.right - rc.left;
                int h = rc.bottom - rc.top;
                // Snap overlays are typically sizable popups, not tiny staging HWNDs
                if (w >= 120 && h >= 80) return true;
            }
        }
    }
    return false;
}

struct EnumCtx {
    HMONITOR mon;
    HWND skip;
    std::vector<HWND>* out;
};

static BOOL CALLBACK EnumTopWindows(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (hwnd == ctx->skip) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE; // skip owned
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_VISIBLE)) return TRUE;

    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    if (title[0] == 0) return TRUE;

    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0) return TRUE;
    if (_wcsicmp(cls, L"Shell_TrayWnd") == 0) return TRUE;
    if (_wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0) return TRUE;
    if (IsSnapAssistWindow(hwnd)) return TRUE;

    HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (ctx->mon && m != ctx->mon) return TRUE;

    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(hwnd, &wp);
    if (wp.showCmd == SW_SHOWMINIMIZED) return TRUE;

    ctx->out->push_back(hwnd);
    return (int)ctx->out->size() < g_maxWindows ? TRUE : FALSE;
}

static void RebuildCandidates() {
    HWND fg = GetForegroundWindow();
    HMONITOR mon = MonitorFromWindow(fg ? fg : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    std::vector<HWND> list;
    EnumCtx ctx{mon, fg, &list};
    EnumWindows(EnumTopWindows, (LPARAM)&ctx);

    std::vector<std::wstring> titles;
    titles.reserve(list.size());
    for (HWND h : list) {
        wchar_t t[256]{};
        GetWindowTextW(h, t, 256);
        if (wcslen(t) > 48) {
            t[45] = L'.'; t[46] = L'.'; t[47] = L'.'; t[48] = 0;
        }
        titles.emplace_back(t);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_candidates = std::move(list);
    g_titles = std::move(titles);
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(RGB(20, 22, 28));
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(240, 240, 245));
            HFONT font = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT old = (HFONT)SelectObject(hdc, font);

            std::vector<std::wstring> lines;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (size_t i = 0; i < g_titles.size(); ++i) {
                    wchar_t buf[300];
                    swprintf_s(buf, L"%d  %s", (int)i + 1, g_titles[i].c_str());
                    lines.emplace_back(buf);
                }
            }
            int y = 12;
            for (auto& line : lines) {
                RECT tr{16, y, rc.right - 16, y + 24};
                DrawTextW(hdc, line.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                y += 28;
            }
            if (lines.empty()) {
                RECT tr{16, 12, rc.right - 16, 40};
                DrawTextW(hdc, L"No snap targets", -1, &tr, DT_LEFT);
            }

            SelectObject(hdc, old);
            DeleteObject(font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT; // click-through
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void DestroyOverlay() {
    if (g_overlayWnd) {
        DestroyWindow(g_overlayWnd);
        g_overlayWnd = nullptr;
    }
}

static void ShowOverlay() {
    if (!g_enableOverlay) {
        DestroyOverlay();
        return;
    }
    RebuildCandidates();
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        count = (int)g_candidates.size();
    }
    int height = 24 + std::max(1, count) * 28 + 12;
    int width = 360;

    HWND fg = GetForegroundWindow();
    HMONITOR mon = MonitorFromWindow(fg ? fg : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + 40;
    int y = mi.rcWork.top + 80;

    if (!g_overlayWnd) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WindhawkSnapAssistOverlay";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        g_overlayWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            L"WindhawkSnapAssistOverlay", L"Snap Targets",
            WS_POPUP,
            x, y, width, height,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (g_overlayWnd)
            SetLayeredWindowAttributes(g_overlayWnd, 0, 220, LWA_ALPHA);
    } else {
        SetWindowPos(g_overlayWnd, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE);
    }
    if (g_overlayWnd) {
        ShowWindow(g_overlayWnd, SW_SHOWNOACTIVATE);
        InvalidateRect(g_overlayWnd, nullptr, TRUE);
    }
}

static void ActivateCandidate(int index1based) {
    HWND target = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        int idx = index1based - 1;
        if (idx < 0 || idx >= (int)g_candidates.size()) return;
        target = g_candidates[idx];
    }
    if (!target || !IsWindow(target)) return;
    Wh_Log(L"Activating snap candidate #%d hwnd=%p", index1based, target);
    // Restore if minimized, then foreground
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    // Allow SetForegroundWindow
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    AllowSetForegroundWindow(pid);
    SetForegroundWindow(target);
    BringWindowToTop(target);
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_enableDigits) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool snap = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            snap = g_snapActive;
        }
        // Pass through unless snap overlay is actively detected
        if (snap && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            if (g_requireWinKey) {
                bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                               (GetAsyncKeyState(VK_RWIN) & 0x8000);
                if (!winDown)
                    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
            }
            int digit = -1;
            if (kb->vkCode >= '1' && kb->vkCode <= '9')
                digit = (int)(kb->vkCode - '0');
            else if (kb->vkCode >= VK_NUMPAD1 && kb->vkCode <= VK_NUMPAD9)
                digit = (int)(kb->vkCode - VK_NUMPAD1 + 1);
            if (digit >= 1 && digit <= g_maxWindows) {
                ActivateCandidate(digit);
                return 1; // eat key only while armed
            }
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

static bool DetectSnapAssistActive() {
    HWND fg = GetForegroundWindow();
    if (IsSnapAssistWindow(fg)) return true;
    // Also scan top-level for snap hosts that may not hold focus
    bool found = false;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (IsWindowVisible(hwnd) && IsSnapAssistWindow(hwnd)) {
            *reinterpret_cast<bool*>(lp) = true;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&found);
    return found;
}

static DWORD WINAPI PollThreadProc(LPVOID) {
    Wh_Log(L"Snap assist poll thread started");
    bool wasActive = false;
    while (WaitForSingleObject(g_stopEvent, 250) == WAIT_TIMEOUT) {
        bool active = DetectSnapAssistActive();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snapActive = active;
        }
        if (active && !wasActive) {
            Wh_Log(L"Snap Assist armed");
            RebuildCandidates();
            ShowOverlay();
        } else if (active && wasActive) {
            // Refresh candidates periodically
            RebuildCandidates();
            if (g_overlayWnd) InvalidateRect(g_overlayWnd, nullptr, TRUE);
        } else if (!active && wasActive) {
            Wh_Log(L"Snap Assist disarmed");
            DestroyOverlay();
        }
        wasActive = active;
    }
    DestroyOverlay();
    return 0;
}

BOOL Wh_ModInit() {
    LoadSettings();
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                 GetModuleHandleW(nullptr), 0);
    if (!g_kbHook)
        Wh_Log(L"WH_KEYBOARD_LL failed");
    g_pollThread = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
    Wh_Log(L"snap-assist-hotkeys init");
    return TRUE;
}

void Wh_ModUninit() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, 5000);
        CloseHandle(g_pollThread);
        g_pollThread = nullptr;
    }
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
    DestroyOverlay();
    UnregisterClassW(L"WindhawkSnapAssistOverlay", GetModuleHandleW(nullptr));
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    Wh_Log(L"snap-assist-hotkeys uninit");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    Wh_Log(L"Settings changed: overlay=%d digits=%d max=%d requireWin=%d",
           (int)g_enableOverlay, (int)g_enableDigits, g_maxWindows, (int)g_requireWinKey);
}
