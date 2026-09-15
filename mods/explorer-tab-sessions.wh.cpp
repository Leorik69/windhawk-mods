// ==WindhawkMod==
// @id              explorer-tab-sessions
// @name            Explorer Tab Sessions
// @description     Save and restore named Explorer folder sessions via hotkeys; persists JSON under AppData
// @version         1.0.0
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -luuid -lshell32 -luser32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer Tab Sessions

Persist open Explorer windows/folder paths as named sessions and restore them
later with hotkeys.

## Hotkeys
- **Ctrl+Alt+S** — save current Explorer folders as the default session
- **Ctrl+Alt+R** — restore the default session (opens folders via ShellExecute)

## Storage
`%AppData%\WindhawkMods\explorer-tab-sessions\sessions.json`

## Settings
- **defaultSessionName** — session name used by the hotkeys (default: `default`)
- **maxSessions** — cap on stored sessions (oldest extra names trimmed on save)

## Notes
Uses IShellWindows to enumerate open Explorer browser windows and their
LocationURL. Windows 11 tabbed Explorer exposes one window per frame; all
visible folder windows are captured. Restoring re-opens paths — it does not
reattach to existing tab UI internals.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- defaultSessionName: default
  $name: Default session name
  $description: Session name used by Ctrl+Alt+S / Ctrl+Alt+R
- maxSessions: 20
  $name: Max sessions
  $description: Maximum number of stored sessions (1–100)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>

static std::wstring g_defaultSessionName = L"default";
static int g_maxSessions = 20;

static HWND g_msgWnd = nullptr;
static const UINT HOTKEY_SAVE = 1;
static const UINT HOTKEY_RESTORE = 2;

static void LoadSettings() {
    PCWSTR name = Wh_GetStringSetting(L"defaultSessionName");
    if (name && name[0])
        g_defaultSessionName = name;
    else
        g_defaultSessionName = L"default";
    if (name) Wh_FreeStringSetting(name);

    g_maxSessions = Wh_GetIntSetting(L"maxSessions");
    if (g_maxSessions < 1) g_maxSessions = 1;
    if (g_maxSessions > 100) g_maxSessions = 100;
}

static std::wstring GetSessionsDir() {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        return {};
    std::wstring dir = appdata;
    dir += L"\\WindhawkMods\\explorer-tab-sessions";
    return dir;
}

static std::wstring GetSessionsPath() {
    return GetSessionsDir() + L"\\sessions.json";
}

static bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    int ok = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return ok == ERROR_SUCCESS || ok == ERROR_ALREADY_EXISTS || ok == ERROR_FILE_EXISTS;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back((char)c);
                }
        }
    }
    return o;
}

struct Session {
    std::wstring name;
    std::vector<std::wstring> paths;
};

// Minimal JSON: {"sessions":[{"name":"...","paths":["..."]}, ...]}
static std::vector<Session> ParseSessions(const std::string& json) {
    std::vector<Session> out;
    size_t pos = 0;
    auto findStr = [&](const std::string& key) -> std::string {
        std::string pat = "\"" + key + "\"";
        size_t k = json.find(pat, pos);
        if (k == std::string::npos) return {};
        k = json.find(':', k);
        if (k == std::string::npos) return {};
        k = json.find('"', k);
        if (k == std::string::npos) return {};
        size_t start = k + 1;
        std::string val;
        for (size_t i = start; i < json.size(); ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) {
                char n = json[i + 1];
                if (n == '"' || n == '\\' || n == '/') { val.push_back(n); i++; }
                else if (n == 'n') { val.push_back('\n'); i++; }
                else if (n == 'r') { val.push_back('\r'); i++; }
                else if (n == 't') { val.push_back('\t'); i++; }
                else val.push_back(json[i]);
                continue;
            }
            if (json[i] == '"') { pos = i + 1; return val; }
            val.push_back(json[i]);
        }
        return {};
    };

    // Find each {"name":
    size_t search = 0;
    while (true) {
        size_t npos = json.find("\"name\"", search);
        if (npos == std::string::npos) break;
        pos = npos;
        Session s;
        s.name = Utf8ToWide(findStr("name"));
        // paths array after name
        size_t parr = json.find("\"paths\"", pos);
        if (parr == std::string::npos) { search = pos; continue; }
        size_t abracket = json.find('[', parr);
        size_t cbracket = json.find(']', abracket);
        if (abracket == std::string::npos || cbracket == std::string::npos) {
            search = pos;
            continue;
        }
        size_t p = abracket + 1;
        while (p < cbracket) {
            size_t q1 = json.find('"', p);
            if (q1 == std::string::npos || q1 >= cbracket) break;
            pos = q1;
            // reuse find from current quote — manual extract
            std::string val;
            for (size_t i = q1 + 1; i < cbracket; ++i) {
                if (json[i] == '\\' && i + 1 < cbracket) {
                    char n = json[i + 1];
                    if (n == '"' || n == '\\') { val.push_back(n); i++; }
                    else val.push_back(json[i]);
                    continue;
                }
                if (json[i] == '"') { p = i + 1; break; }
                val.push_back(json[i]);
            }
            if (!val.empty()) s.paths.push_back(Utf8ToWide(val));
        }
        if (!s.name.empty()) out.push_back(std::move(s));
        search = cbracket + 1;
    }
    return out;
}

static std::string SerializeSessions(const std::vector<Session>& sessions) {
    std::ostringstream ss;
    ss << "{\n  \"sessions\": [\n";
    for (size_t i = 0; i < sessions.size(); ++i) {
        ss << "    {\n      \"name\": \"" << JsonEscape(WideToUtf8(sessions[i].name)) << "\",\n";
        ss << "      \"paths\": [";
        for (size_t j = 0; j < sessions[i].paths.size(); ++j) {
            if (j) ss << ", ";
            ss << "\"" << JsonEscape(WideToUtf8(sessions[i].paths[j])) << "\"";
        }
        ss << "]\n    }";
        if (i + 1 < sessions.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ]\n}\n";
    return ss.str();
}

static std::vector<Session> LoadAllSessions() {
    std::wstring path = GetSessionsPath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ParseSessions(ss.str());
}

static bool SaveAllSessions(const std::vector<Session>& sessions) {
    std::wstring dir = GetSessionsDir();
    if (!EnsureDir(dir)) {
        Wh_Log(L"Failed to create sessions dir");
        return false;
    }
    std::wstring path = GetSessionsPath();
    std::string data = SerializeSessions(sessions);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        Wh_Log(L"Failed to write sessions.json");
        return false;
    }
    out.write(data.data(), (std::streamsize)data.size());
    return true;
}

static std::wstring UrlToPath(const std::wstring& url) {
    if (url.rfind(L"file:///", 0) == 0) {
        std::wstring path = url.substr(8);
        for (auto& c : path) if (c == L'/') c = L'\\';
        std::wstring decoded;
        for (size_t k = 0; k < path.size(); ++k) {
            if (path[k] == L'%' && k + 2 < path.size()) {
                auto hex = [](wchar_t c) -> int {
                    if (c >= L'0' && c <= L'9') return c - L'0';
                    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                    return -1;
                };
                int hi = hex(path[k + 1]), lo = hex(path[k + 2]);
                if (hi >= 0 && lo >= 0) {
                    decoded.push_back((wchar_t)((hi << 4) | lo));
                    k += 2;
                    continue;
                }
            }
            decoded.push_back(path[k]);
        }
        return decoded;
    }
    if (url.rfind(L"file://", 0) == 0) {
        std::wstring path = url.substr(7);
        for (auto& c : path) if (c == L'/') c = L'\\';
        return path;
    }
    return {};
}

static std::vector<std::wstring> CaptureExplorerPaths() {
    std::vector<std::wstring> paths;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellWindows* psw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_IShellWindows, (void**)&psw)) || !psw) {
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return paths;
    }
    long count = 0;
    psw->get_Count(&count);
    for (long i = 0; i < count; i++) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(psw->Item(v, &disp)) || !disp) continue;
        IWebBrowserApp* browser = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_IWebBrowserApp, (void**)&browser)) && browser) {
            BSTR loc = nullptr;
            if (SUCCEEDED(browser->get_LocationURL(&loc)) && loc) {
                std::wstring url(loc, SysStringLen(loc));
                SysFreeString(loc);
                std::wstring path = UrlToPath(url);
                if (!path.empty()) {
                    // Dedup
                    if (std::find(paths.begin(), paths.end(), path) == paths.end())
                        paths.push_back(path);
                }
            }
            browser->Release();
        }
        disp->Release();
    }
    psw->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return paths;
}

static void SaveCurrentSession() {
    auto paths = CaptureExplorerPaths();
    Wh_Log(L"Captured %d explorer paths for session '%s'",
           (int)paths.size(), g_defaultSessionName.c_str());
    if (paths.empty()) {
        MessageBoxW(nullptr,
                    L"No Explorer folder windows found to save.",
                    L"Explorer Tab Sessions", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto sessions = LoadAllSessions();
    bool found = false;
    for (auto& s : sessions) {
        if (_wcsicmp(s.name.c_str(), g_defaultSessionName.c_str()) == 0) {
            s.paths = paths;
            found = true;
            break;
        }
    }
    if (!found) {
        Session s;
        s.name = g_defaultSessionName;
        s.paths = paths;
        sessions.push_back(std::move(s));
    }
    // Trim if over max — keep most recently updated by moving current to end already;
    // drop from front if needed
    while ((int)sessions.size() > g_maxSessions)
        sessions.erase(sessions.begin());

    if (SaveAllSessions(sessions)) {
        wchar_t msg[256];
        swprintf_s(msg, L"Saved session '%s' with %d folder(s).",
                   g_defaultSessionName.c_str(), (int)paths.size());
        MessageBoxW(nullptr, msg, L"Explorer Tab Sessions", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(nullptr, L"Failed to write sessions.json.",
                    L"Explorer Tab Sessions", MB_OK | MB_ICONERROR);
    }
}

static void RestoreDefaultSession() {
    auto sessions = LoadAllSessions();
    const Session* target = nullptr;
    for (auto& s : sessions) {
        if (_wcsicmp(s.name.c_str(), g_defaultSessionName.c_str()) == 0) {
            target = &s;
            break;
        }
    }
    if (!target || target->paths.empty()) {
        wchar_t msg[256];
        swprintf_s(msg, L"Session '%s' not found or empty.", g_defaultSessionName.c_str());
        MessageBoxW(nullptr, msg, L"Explorer Tab Sessions", MB_OK | MB_ICONWARNING);
        return;
    }
    int opened = 0;
    for (auto& p : target->paths) {
        HINSTANCE hi = ShellExecuteW(nullptr, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)hi > 32) opened++;
        else {
            // Fallback: open as folder
            hi = ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)hi > 32) opened++;
        }
    }
    Wh_Log(L"Restored session '%s': opened %d/%d",
           g_defaultSessionName.c_str(), opened, (int)target->paths.size());
}

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        if (wParam == HOTKEY_SAVE) SaveCurrentSession();
        else if (wParam == HOTKEY_RESTORE) RestoreDefaultSession();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL Wh_ModInit() {
    LoadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindhawkExplorerTabSessionsMsg";
    RegisterClassExW(&wc);

    g_msgWnd = CreateWindowExW(0, L"WindhawkExplorerTabSessionsMsg", L"",
                               WS_OVERLAPPED, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_msgWnd) {
        Wh_Log(L"Failed to create message window");
        return FALSE;
    }

    if (!RegisterHotKey(g_msgWnd, HOTKEY_SAVE, MOD_CONTROL | MOD_ALT, 'S'))
        Wh_Log(L"RegisterHotKey Ctrl+Alt+S failed");
    if (!RegisterHotKey(g_msgWnd, HOTKEY_RESTORE, MOD_CONTROL | MOD_ALT, 'R'))
        Wh_Log(L"RegisterHotKey Ctrl+Alt+R failed");

    EnsureDir(GetSessionsDir());
    Wh_Log(L"explorer-tab-sessions init (default='%s')", g_defaultSessionName.c_str());
    return TRUE;
}

void Wh_ModUninit() {
    if (g_msgWnd) {
        UnregisterHotKey(g_msgWnd, HOTKEY_SAVE);
        UnregisterHotKey(g_msgWnd, HOTKEY_RESTORE);
        DestroyWindow(g_msgWnd);
        g_msgWnd = nullptr;
    }
    UnregisterClassW(L"WindhawkExplorerTabSessionsMsg", GetModuleHandleW(nullptr));
    Wh_Log(L"explorer-tab-sessions uninit");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    Wh_Log(L"Settings changed: default='%s' max=%d",
           g_defaultSessionName.c_str(), g_maxSessions);
}
