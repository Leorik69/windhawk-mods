// ==WindhawkMod==
// @id              dev-environment-example
// @name            Dev Environment Example
// @description     A minimal example mod used to verify the Windhawk mods development environment
// @version         1.0.0
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         explorer.exe
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Dev Environment Example

A tiny sample mod that demonstrates both styles of hooking used by Windhawk
mods: a direct API hook (`SetWindowTextW`) and a symbol hook in `explorer.exe`.

It exists to exercise the local development tooling (metadata validation, symbol
extraction, formatting and catalog generation) and is not meant to be a useful
end-user mod.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- greeting: Hello, Windhawk
  $name: Greeting text
  $description: Text written to the log when the mod initializes.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

using SetWindowTextW_t = decltype(&SetWindowTextW);
SetWindowTextW_t SetWindowTextW_Original;

BOOL WINAPI SetWindowTextW_Hook(HWND hWnd, LPCWSTR lpString) {
    Wh_Log(L"SetWindowTextW called with: %s", lpString ? lpString : L"(null)");
    return SetWindowTextW_Original(hWnd, lpString);
}

using CTray_WndProc_t = LRESULT(
    WINAPI*)(void* pThis, HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
CTray_WndProc_t CTray_WndProc_Original;

LRESULT WINAPI CTray_WndProc_Hook(void* pThis,
                                  HWND hWnd,
                                  UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam) {
    return CTray_WndProc_Original(pThis, hWnd, uMsg, wParam, lParam);
}

BOOL Wh_ModInit() {
    PCWSTR greeting = Wh_GetStringSetting(L"greeting");
    Wh_Log(L"Init: %s", greeting);
    Wh_FreeStringSetting(greeting);

    Wh_SetFunctionHook((void*)SetWindowTextW, (void*)SetWindowTextW_Hook,
                       (void**)&SetWindowTextW_Original);

    WindhawkUtils::SYMBOL_HOOK explorerExeHooks[] = {
        {
            {LR"(private: __int64 __cdecl CTray::_WndProc(struct HWND__ *,unsigned int,unsigned __int64,__int64))"},
            &CTray_WndProc_Original,
            CTray_WndProc_Hook,
        },
    };

    if (!WindhawkUtils::HookSymbols(GetModuleHandle(nullptr), explorerExeHooks,
                                    ARRAYSIZE(explorerExeHooks))) {
        Wh_Log(L"Failed to hook CTray::_WndProc");
    }

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}
