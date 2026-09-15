// ==WindhawkMod==
// @id              hide-copilot-recall-click-to-do
// @name            Hide Copilot, Recall, and Click to Do
// @description     Hides the Copilot, Recall, and Click to Do entry points from the Windows 11 24H2/25H2 taskbar
// @version         1.0.0
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lversion
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Hide Copilot, Recall, and Click to Do

Removes the AI-related chrome that Microsoft added to the Windows 11 taskbar in
24H2/25H2 by collapsing the corresponding taskbar buttons:

- **Copilot** — the Copilot taskbar button / augmented entry point.
- **Recall** — the Recall taskbar button (on Copilot+ PCs).
- **Click to Do** — the Click to Do taskbar entry point.

The mod hooks the taskbar's XAML button classes in `Taskbar.View.dll`
(`AugmentedEntryPointButton`, `ExperienceToggleButton`, `TaskListButton`, and
`SearchBoxButton`). When a button is created it inspects the button's class name,
`AutomationId`, and accessible name; if it matches one of the enabled targets the
button's `Visibility` is set to `Collapsed`, so it disappears from the taskbar
without changing any system settings.

Everything is fully reversible: disabling the mod restores every button it hid.

## Notes and limitations

- This hides the **taskbar entry points / chrome**. Click to Do can also be
  invoked contextually (e.g. from Recall or a keyboard shortcut); this mod hides
  its taskbar button, it does not disable the underlying feature.
- Recall/Click to Do only exist on supported (Copilot+) hardware and recent
  builds; on other builds those toggles simply have nothing to hide.
- Matching against the accessible name uses the brand names "Copilot" and
  "Recall". Use **Extra keywords** below if your build labels a button
  differently.

## Options

Use the settings to toggle each target individually, optionally hide the Search
box, and add extra case-insensitive keywords to match against a button's class,
`AutomationId`, and name.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideCopilot: true
  $name: Hide Copilot
  $description: Hide the Copilot taskbar button / augmented entry point.
- hideRecall: true
  $name: Hide Recall
  $description: Hide the Recall taskbar button (Copilot+ PCs).
- hideClickToDo: true
  $name: Hide Click to Do
  $description: Hide the Click to Do taskbar entry point.
- hideSearch: false
  $name: Hide the Search box/button
  $description: Also hide the taskbar search box (which hosts a Copilot entry point on some builds).
- extraKeywords: ""
  $name: Extra keywords
  $description: Comma-separated, case-insensitive substrings matched against a taskbar button's class, AutomationId, and name.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::UI::Xaml::Automation::AutomationProperties;

struct {
    std::atomic<bool> hideCopilot;
    std::atomic<bool> hideRecall;
    std::atomic<bool> hideClickToDo;
    std::atomic<bool> hideSearch;
} g_settings;

std::mutex g_keywordsMutex;
std::vector<std::wstring> g_extraKeywords;

std::atomic<bool> g_unloading;
std::atomic<bool> g_taskbarViewModuleHooked;

using FrameworkElementLoadedEventRevoker = winrt::impl::event_revoker<
    IFrameworkElement,
    &winrt::impl::abi<IFrameworkElement>::type::remove_Loaded>;

// Loaded-event revokers for elements we're still waiting on. Only touched on the
// UI thread (button constructors and their Loaded handlers both run there).
std::list<FrameworkElementLoadedEventRevoker> g_loadedRevokers;

// Buttons we've seen, so a settings change can re-evaluate them.
std::mutex g_trackedMutex;
std::vector<winrt::weak_ref<FrameworkElement>> g_trackedElements;

std::wstring ToLower(std::wstring_view s) {
    std::wstring result(s);
    for (auto& c : result) {
        c = towlower(c);
    }
    return result;
}

bool ContainsAny(const std::wstring& haystackLower,
                 std::initializer_list<const wchar_t*> needlesLower) {
    for (const wchar_t* needle : needlesLower) {
        if (haystackLower.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool ShouldHideElement(FrameworkElement const& element) {
    std::wstring blob;
    try {
        blob += winrt::get_class_name(element);
        blob += L'\n';
        blob += AutomationProperties::GetAutomationId(element);
        blob += L'\n';
        blob += element.Name();
        blob += L'\n';
        blob += AutomationProperties::GetName(element);
    } catch (...) {
        return false;
    }

    blob = ToLower(blob);

    // The Copilot augmented entry point is a dedicated class, treat it as Copilot
    // even if its name/id is empty on some builds.
    bool isAugmentedEntry = blob.find(L"augmentedentrypoint") != std::wstring::npos;

    if (g_settings.hideCopilot &&
        (isAugmentedEntry || ContainsAny(blob, {L"copilot"}))) {
        return true;
    }
    if (g_settings.hideRecall && ContainsAny(blob, {L"recall"})) {
        return true;
    }
    if (g_settings.hideClickToDo &&
        ContainsAny(blob, {L"clicktodo", L"click to do"})) {
        return true;
    }
    if (g_settings.hideSearch &&
        ContainsAny(blob, {L"searchbox", L"searchboxtaskbarbutton"})) {
        return true;
    }

    {
        std::lock_guard<std::mutex> guard(g_keywordsMutex);
        for (const auto& keyword : g_extraKeywords) {
            if (!keyword.empty() &&
                blob.find(keyword) != std::wstring::npos) {
                return true;
            }
        }
    }

    return false;
}

// Must run on the taskbar UI thread.
void ApplyToElement(FrameworkElement const& element) {
    if (!element) {
        return;
    }

    bool hide = !g_unloading && ShouldHideElement(element);
    auto desired = hide ? Visibility::Collapsed : Visibility::Visible;

    try {
        if (element.Visibility() != desired) {
            element.Visibility(desired);
        }
    } catch (...) {
        // Element may have been torn down; ignore.
    }
}

// Must run on the taskbar UI thread.
void ApplyToAllTracked() {
    std::lock_guard<std::mutex> guard(g_trackedMutex);

    std::vector<winrt::weak_ref<FrameworkElement>> alive;
    alive.reserve(g_trackedElements.size());

    for (auto& weak : g_trackedElements) {
        if (auto element = weak.get()) {
            ApplyToElement(element);
            alive.push_back(std::move(weak));
        }
    }

    g_trackedElements = std::move(alive);
}

void OnTaskbarButtonConstructed(void* pThis) {
    FrameworkElement element = nullptr;
    ((IUnknown**)pThis)[1]->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(element));
    if (!element) {
        return;
    }

    g_loadedRevokers.emplace_back();
    auto revokerIt = g_loadedRevokers.end();
    --revokerIt;

    *revokerIt = element.Loaded(
        winrt::auto_revoke_t{},
        [revokerIt](winrt::Windows::Foundation::IInspectable const& sender,
                    RoutedEventArgs const&) {
            g_loadedRevokers.erase(revokerIt);

            auto element = sender.try_as<FrameworkElement>();
            if (!element) {
                return;
            }

            {
                std::lock_guard<std::mutex> guard(g_trackedMutex);
                g_trackedElements.push_back(winrt::make_weak(element));
            }

            ApplyToElement(element);
        });
}

using ButtonConstructor_t = void*(WINAPI*)(void* pThis);

ButtonConstructor_t AugmentedEntryPointButton_Construct_Original;
void* WINAPI AugmentedEntryPointButton_Construct_Hook(void* pThis) {
    void* ret = AugmentedEntryPointButton_Construct_Original(pThis);
    OnTaskbarButtonConstructed(pThis);
    return ret;
}

ButtonConstructor_t ExperienceToggleButton_Construct_Original;
void* WINAPI ExperienceToggleButton_Construct_Hook(void* pThis) {
    void* ret = ExperienceToggleButton_Construct_Original(pThis);
    OnTaskbarButtonConstructed(pThis);
    return ret;
}

ButtonConstructor_t TaskListButton_Construct_Original;
void* WINAPI TaskListButton_Construct_Hook(void* pThis) {
    void* ret = TaskListButton_Construct_Original(pThis);
    OnTaskbarButtonConstructed(pThis);
    return ret;
}

ButtonConstructor_t SearchBoxButton_Construct_Original;
void* WINAPI SearchBoxButton_Construct_Hook(void* pThis) {
    void* ret = SearchBoxButton_Construct_Original(pThis);
    OnTaskbarButtonConstructed(pThis);
    return ret;
}

bool HookTaskbarViewSymbols(HMODULE module) {
    // Taskbar.View.dll
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: __cdecl winrt::Taskbar::implementation::AugmentedEntryPointButton::AugmentedEntryPointButton(void))"},
            &AugmentedEntryPointButton_Construct_Original,
            AugmentedEntryPointButton_Construct_Hook,
            true,
        },
        {
            {LR"(public: __cdecl winrt::Taskbar::implementation::ExperienceToggleButton::ExperienceToggleButton(void))"},
            &ExperienceToggleButton_Construct_Original,
            ExperienceToggleButton_Construct_Hook,
            true,
        },
        {
            {LR"(public: __cdecl winrt::Taskbar::implementation::TaskListButton::TaskListButton(void))"},
            &TaskListButton_Construct_Original,
            TaskListButton_Construct_Hook,
            true,
        },
        {
            {LR"(public: __cdecl winrt::Taskbar::implementation::SearchBoxButton::SearchBoxButton(void))"},
            &SearchBoxButton_Construct_Original,
            SearchBoxButton_Construct_Hook,
            true,
        },
    };

    if (!HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks))) {
        Wh_Log(L"HookSymbols failed");
        return false;
    }

    return true;
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

void ApplySettings() {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        return;
    }

    RunFromWindowThread(
        hTaskbarWnd, [](void*) { ApplyToAllTracked(); }, nullptr);
}

void LoadSettings() {
    g_settings.hideCopilot = Wh_GetIntSetting(L"hideCopilot");
    g_settings.hideRecall = Wh_GetIntSetting(L"hideRecall");
    g_settings.hideClickToDo = Wh_GetIntSetting(L"hideClickToDo");
    g_settings.hideSearch = Wh_GetIntSetting(L"hideSearch");

    std::vector<std::wstring> keywords;
    PCWSTR extraKeywords = Wh_GetStringSetting(L"extraKeywords");
    if (extraKeywords) {
        std::wstring current;
        for (PCWSTR p = extraKeywords;; p++) {
            if (*p == L',' || *p == L'\0') {
                size_t start = current.find_first_not_of(L" \t");
                if (start != std::wstring::npos) {
                    size_t end = current.find_last_not_of(L" \t");
                    keywords.push_back(
                        ToLower(current.substr(start, end - start + 1)));
                }
                current.clear();
                if (*p == L'\0') {
                    break;
                }
            } else {
                current += *p;
            }
        }
        Wh_FreeStringSetting(extraKeywords);
    }

    {
        std::lock_guard<std::mutex> guard(g_keywordsMutex);
        g_extraKeywords = std::move(keywords);
    }
}

HMODULE GetTaskbarViewModuleHandle() {
    return GetModuleHandle(L"Taskbar.View.dll");
}

void HandleLoadedModuleIfTaskbarView(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_taskbarViewModuleHooked && GetTaskbarViewModuleHandle() == module &&
        !g_taskbarViewModuleHooked.exchange(true)) {
        Wh_Log(L"Loaded %s", lpLibFileName);

        if (HookTaskbarViewSymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModuleIfTaskbarView(module, lpLibFileName);
    }

    return module;
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle()) {
        g_taskbarViewModuleHooked = true;
        if (!HookTaskbarViewSymbols(taskbarViewModule)) {
            return FALSE;
        }
    } else {
        Wh_Log(L"Taskbar.View.dll not loaded yet");

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        auto pKernelBaseLoadLibraryExW =
            (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule,
                                                      "LoadLibraryExW");
        WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                       LoadLibraryExW_Hook,
                                       &LoadLibraryExW_Original);
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    if (!g_taskbarViewModuleHooked) {
        if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle()) {
            if (!g_taskbarViewModuleHooked.exchange(true)) {
                Wh_Log(L"Got Taskbar.View.dll");

                if (HookTaskbarViewSymbols(taskbarViewModule)) {
                    Wh_ApplyHookOperations();
                }
            }
        }
    }

    ApplySettings();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");

    g_unloading = true;

    ApplySettings();
}

void Wh_ModUninit() {
    Wh_Log(L">");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();

    ApplySettings();
}
