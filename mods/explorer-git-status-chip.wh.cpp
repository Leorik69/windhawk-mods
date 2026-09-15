// ==WindhawkMod==
// @id              explorer-git-status-chip
// @name            Explorer Git Status Chip
// @description     Appends [#git:branch*] to Explorer window titles when the folder is a git repo; detects dirty via porcelain or heuristics
// @version         1.0.1
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -luser32 -lshell32 -lole32 -loleaut32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer Git Status Chip

Shows the current git branch (and dirty marker `*`) in the Explorer window title
when the open folder is inside a git repository.

## How it works
- Polls Explorer windows and resolves the folder path via Shell Windows COM
- Reads `.git/HEAD` (and packed-refs if needed) for the branch name
- Detects dirty state via `git status --porcelain` when git.exe is available,
  otherwise uses simple index/worktree mtime heuristics (briefly cached)
- Appends a unique chip ` [#git:branch*]` to the title; restores on unload

## Settings
- **enabled** — master switch
- **showDirty** — append `*` when the worktree is dirty
- **maxBranchLen** — truncate long branch names
- **pollMs** — how often to refresh (milliseconds; default 9000)

## Notes / SL mitigations (v1.0.1)
Default poll is ~9s (was 2s). `CoInitializeEx` runs once per worker thread
lifetime, not every cycle. `StripChip` only removes the `#git:` marker pattern,
never arbitrary `[…]` title suffixes. Dirty status is cached briefly per path.
Best-effort; nested/worktree edge cases may not resolve.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch for the git status chip
- showDirty: true
  $name: Show dirty marker
  $description: Append * when the worktree has uncommitted changes
- maxBranchLen: 32
  $name: Max branch length
  $description: Truncate branch names longer than this
- pollMs: 9000
  $name: Poll interval (ms)
  $description: How often to refresh titles (500–30000; default 9000)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

static bool g_enabled = true;
static bool g_showDirty = true;
static int g_maxBranchLen = 32;
static int g_pollMs = 9000;

static HANDLE g_pollThread = nullptr;
static HANDLE g_stopEvent = nullptr;
static std::mutex g_titleMutex;
static std::unordered_map<HWND, std::wstring> g_originalTitles;

// Unique chip open marker — StripChip must NEVER match arbitrary "[…]" suffixes
static constexpr wchar_t kChipPrefix[] = L" [#git:";

struct DirtyCacheEntry {
    bool dirty = false;
    std::chrono::steady_clock::time_point at{};
};
static std::mutex g_dirtyCacheMutex;
static std::unordered_map<std::wstring, DirtyCacheEntry> g_dirtyCache;
static constexpr auto kDirtyCacheTtl = std::chrono::seconds(8);

static void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled") != 0;
    g_showDirty = Wh_GetIntSetting(L"showDirty") != 0;
    g_maxBranchLen = Wh_GetIntSetting(L"maxBranchLen");
    if (g_maxBranchLen < 4) g_maxBranchLen = 4;
    if (g_maxBranchLen > 128) g_maxBranchLen = 128;
    g_pollMs = Wh_GetIntSetting(L"pollMs");
    if (g_pollMs < 500) g_pollMs = 500;
    if (g_pollMs > 30000) g_pollMs = 30000;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static fs::path FindGitDir(const fs::path& start) {
    std::error_code ec;
    fs::path cur = start;
    while (!cur.empty()) {
        fs::path git = cur / ".git";
        if (fs::exists(git, ec)) {
            if (fs::is_directory(git, ec))
                return git;
            // gitfile: gitdir: <path>
            std::ifstream in(git);
            std::string line;
            if (std::getline(in, line) && line.rfind("gitdir:", 0) == 0) {
                std::string p = line.substr(7);
                while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
                while (!p.empty() && (p.back() == '\r' || p.back() == '\n' || p.back() == ' ')) p.pop_back();
                fs::path gd = fs::path(p);
                if (!gd.is_absolute()) gd = cur / gd;
                if (fs::exists(gd, ec)) return gd;
            }
        }
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return {};
}

static std::string ReadFileTrim(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

static std::string ResolvePackedRef(const fs::path& gitDir, const std::string& ref) {
    fs::path packed = gitDir / "packed-refs";
    std::ifstream in(packed);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '^') continue;
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string name = line.substr(sp + 1);
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
        if (name == ref) return line.substr(0, sp);
    }
    return {};
}

static std::string GetBranchName(const fs::path& gitDir) {
    std::string head = ReadFileTrim(gitDir / "HEAD");
    if (head.empty()) return {};
    const std::string prefix = "ref: ";
    if (head.rfind(prefix, 0) == 0) {
        std::string ref = head.substr(prefix.size());
        // Prefer short branch name
        const std::string heads = "refs/heads/";
        if (ref.rfind(heads, 0) == 0)
            return ref.substr(heads.size());
        // Verify ref exists or is packed
        if (fs::exists(gitDir / ref) || !ResolvePackedRef(gitDir, ref).empty()) {
            auto pos = ref.rfind('/');
            return pos == std::string::npos ? ref : ref.substr(pos + 1);
        }
        auto pos = ref.rfind('/');
        return pos == std::string::npos ? ref : ref.substr(pos + 1);
    }
    // Detached HEAD — show short SHA
    if (head.size() >= 7) return head.substr(0, 7);
    return head;
}

static bool FileExistsW(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES;
}

static bool GitExeAvailable() {
    wchar_t buf[MAX_PATH];
    return SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, buf, nullptr) != 0;
}

static bool IsDirtyViaGit(const fs::path& workTree) {
    std::wstring wt = workTree.wstring();
    std::wstring cmd = L"git.exe status --porcelain -uno";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, wt.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    char chunk[512];
    DWORD got = 0;
    std::string out;
    while (ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got)
        out.append(chunk, got);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    // Any non-empty porcelain line => dirty
    for (char c : out) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return true;
    }
    return false;
}

static bool IsDirtyHeuristic(const fs::path& gitDir, const fs::path& workTree) {
    std::error_code ec;
    auto indexPath = gitDir / "index";
    if (!fs::exists(indexPath, ec)) return false;
    auto indexTime = fs::last_write_time(indexPath, ec);
    if (ec) return false;
    // Sample a few top-level files; if any are newer than index, likely dirty
    int checked = 0;
    try {
        for (auto& e : fs::directory_iterator(workTree, ec)) {
            if (ec) break;
            if (e.path().filename() == ".git") continue;
            if (e.is_regular_file(ec)) {
                auto t = e.last_write_time(ec);
                if (!ec && t > indexTime) return true;
                if (++checked >= 40) break;
            } else if (e.is_directory(ec)) {
                for (auto& e2 : fs::directory_iterator(e.path(), ec)) {
                    if (ec) break;
                    if (e2.is_regular_file(ec)) {
                        auto t = e2.last_write_time(ec);
                        if (!ec && t > indexTime) return true;
                        if (++checked >= 40) break;
                    }
                }
            }
            if (checked >= 40) break;
        }
    } catch (...) {
    }
    return false;
}

static std::wstring TruncateBranch(const std::wstring& b) {
    if ((int)b.size() <= g_maxBranchLen) return b;
    if (g_maxBranchLen <= 3) return b.substr(0, g_maxBranchLen);
    return b.substr(0, g_maxBranchLen - 1) + L"…";
}

static std::wstring StripChip(const std::wstring& title) {
    // Only remove THIS mod's chip: " [#git:<branch>*]" — never arbitrary "[…]"
    auto pos = title.rfind(kChipPrefix);
    if (pos == std::wstring::npos) return title;
    if (title.back() != L']') return title;
    // Remainder after prefix must be branch[+*] then closing ']'
    const size_t prefixLen = sizeof(kChipPrefix) / sizeof(wchar_t) - 1;
    std::wstring rest = title.substr(pos + prefixLen);
    if (rest.size() < 2 || rest.back() != L']') return title;
    rest.pop_back();  // drop ']'
    if (!rest.empty() && rest.back() == L'*') rest.pop_back();
    if (rest.empty()) return title;
    // Reject if leftover still contains our marker / brackets (not our chip)
    if (rest.find(L'[') != std::wstring::npos || rest.find(L']') != std::wstring::npos)
        return title;
    return title.substr(0, pos);
}

static std::wstring MakeChip(const std::wstring& branch, bool dirty) {
    std::wstring chip = kChipPrefix;
    chip += TruncateBranch(branch);
    if (dirty) chip += L"*";
    chip += L"]";
    return chip;
}

static bool IsDirtyCached(const fs::path& gitDir, const fs::path& workTree) {
    const std::wstring key = workTree.wstring();
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_dirtyCacheMutex);
        auto it = g_dirtyCache.find(key);
        if (it != g_dirtyCache.end() && (now - it->second.at) < kDirtyCacheTtl)
            return it->second.dirty;
    }
    bool dirty = false;
    if (GitExeAvailable())
        dirty = IsDirtyViaGit(workTree);
    else
        dirty = IsDirtyHeuristic(gitDir, workTree);
    {
        std::lock_guard<std::mutex> lock(g_dirtyCacheMutex);
        g_dirtyCache[key] = DirtyCacheEntry{dirty, now};
    }
    return dirty;
}

struct ExplorerWin {
    HWND hwnd;
    std::wstring path;
};

static std::vector<ExplorerWin> EnumExplorerFolders() {
    // COM must already be initialized on this thread (once in PollThreadProc).
    std::vector<ExplorerWin> result;
    IShellWindows* psw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_IShellWindows, (void**)&psw);
    if (FAILED(hr) || !psw) {
        return result;
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
            HWND hwnd = nullptr;
            browser->get_HWND((SHANDLE_PTR*)&hwnd);
            BSTR loc = nullptr;
            if (SUCCEEDED(browser->get_LocationURL(&loc)) && loc) {
                std::wstring url(loc, SysStringLen(loc));
                SysFreeString(loc);
                // file:///C:/path -> C:\path
                std::wstring path;
                if (url.rfind(L"file:///", 0) == 0) {
                    path = url.substr(8);
                    for (auto& c : path) if (c == L'/') c = L'\\';
                    // URL-decode %20 etc. (minimal)
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
                    path = decoded;
                } else if (url.rfind(L"file://", 0) == 0) {
                    path = url.substr(7);
                    for (auto& c : path) if (c == L'/') c = L'\\';
                }
                if (!path.empty() && hwnd && IsWindow(hwnd))
                    result.push_back({hwnd, path});
            }
            browser->Release();
        }
        disp->Release();
    }
    psw->Release();
    return result;
}

static void ApplyTitles() {
    if (!g_enabled) {
        std::lock_guard<std::mutex> lock(g_titleMutex);
        for (auto& [hwnd, orig] : g_originalTitles) {
            if (IsWindow(hwnd)) SetWindowTextW(hwnd, orig.c_str());
        }
        return;
    }

    auto wins = EnumExplorerFolders();
    std::lock_guard<std::mutex> lock(g_titleMutex);
    std::unordered_map<HWND, bool> seen;

    for (auto& w : wins) {
        seen[w.hwnd] = true;
        std::error_code ec;
        fs::path folder(w.path);
        if (!fs::is_directory(folder, ec)) continue;

        fs::path gitDir = FindGitDir(folder);
        if (gitDir.empty()) {
            // Restore if we previously chipped this window
            auto it = g_originalTitles.find(w.hwnd);
            if (it != g_originalTitles.end()) {
                wchar_t cur[512];
                GetWindowTextW(w.hwnd, cur, 512);
                std::wstring stripped = StripChip(cur);
                if (stripped != cur) SetWindowTextW(w.hwnd, it->second.c_str());
                else SetWindowTextW(w.hwnd, it->second.c_str());
                g_originalTitles.erase(it);
            }
            continue;
        }

        // Work tree = parent of .git when .git is a dir under the repo root
        fs::path workTree = gitDir.parent_path();
        // If gitdir is .git under repo, parent is worktree; for separate gitdir keep folder walk
        std::string branch = GetBranchName(gitDir);
        if (branch.empty()) continue;

        bool dirty = false;
        if (g_showDirty)
            dirty = IsDirtyCached(gitDir, workTree);

        std::wstring chip = MakeChip(Utf8ToWide(branch), dirty);

        wchar_t curTitle[512];
        GetWindowTextW(w.hwnd, curTitle, 512);
        std::wstring base = StripChip(curTitle);
        if (g_originalTitles.find(w.hwnd) == g_originalTitles.end())
            g_originalTitles[w.hwnd] = base;
        else {
            // Keep first-seen clean title; strip only our marker if it leaked in
            g_originalTitles[w.hwnd] = StripChip(g_originalTitles[w.hwnd]);
        }

        std::wstring newTitle = g_originalTitles[w.hwnd] + chip;
        if (wcscmp(curTitle, newTitle.c_str()) != 0) {
            SetWindowTextW(w.hwnd, newTitle.c_str());
            Wh_Log(L"Updated title for %p: %s", w.hwnd, newTitle.c_str());
        }
    }

    // Cleanup closed windows
    for (auto it = g_originalTitles.begin(); it != g_originalTitles.end();) {
        if (!IsWindow(it->first) || !seen[it->first]) {
            if (IsWindow(it->first)) SetWindowTextW(it->first, it->second.c_str());
            it = g_originalTitles.erase(it);
        } else {
            ++it;
        }
    }
}

static void RestoreAllTitles() {
    std::lock_guard<std::mutex> lock(g_titleMutex);
    for (auto& [hwnd, orig] : g_originalTitles) {
        if (IsWindow(hwnd)) SetWindowTextW(hwnd, orig.c_str());
    }
    g_originalTitles.clear();
}

static DWORD WINAPI PollThreadProc(LPVOID) {
    // CoInitializeEx once for the worker thread lifetime (not every poll cycle)
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Wh_Log(L"Git status chip poll thread started (CoInit hr=0x%08X poll=%d)",
           (unsigned)hrInit, g_pollMs);
    while (WaitForSingleObject(g_stopEvent, g_pollMs) == WAIT_TIMEOUT) {
        try {
            ApplyTitles();
        } catch (...) {
            Wh_Log(L"Exception in ApplyTitles");
        }
    }
    if (SUCCEEDED(hrInit))
        CoUninitialize();
    Wh_Log(L"Git status chip poll thread exiting");
    return 0;
}

BOOL Wh_ModInit() {
    LoadSettings();
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_pollThread = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
    Wh_Log(L"explorer-git-status-chip init (poll=%d ms)", g_pollMs);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, 5000);
        CloseHandle(g_pollThread);
        g_pollThread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    RestoreAllTitles();
    Wh_Log(L"explorer-git-status-chip uninit");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    Wh_Log(L"Settings changed: enabled=%d showDirty=%d maxBranchLen=%d pollMs=%d",
           (int)g_enabled, (int)g_showDirty, g_maxBranchLen, g_pollMs);
    if (!g_enabled) RestoreAllTitles();
}
