// ==WindhawkMod==
// @id              hello-world-demo
// @name            Hello World Demo Mod
// @description     A minimal demonstration mod used to validate the Windhawk mods development environment.
// @version         1.0.0
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -luser32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Hello World Demo

A tiny sample mod that exists to prove the development environment works: it is
validated by `.github/pr_validation.py` and its symbol hooks are parsed by
`.github/extract_mod_symbols.py`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- greeting: Hello from Windhawk
  $name: Greeting text
  $description: Text logged when the mod initializes.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

typedef int(WINAPI* MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
MessageBoxW_t pOriginalMessageBoxW;

int WINAPI HookedMessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption,
                             UINT uType) {
    Wh_Log(L"MessageBoxW called");
    return pOriginalMessageBoxW(hWnd, lpText, lpCaption, uType);
}

// Hook a user32 export to demonstrate symbol extraction.
WindhawkUtils::SYMBOL_HOOK user32DllHooks[] = {
    {
        {L"MessageBoxW"},
        (void**)&pOriginalMessageBoxW,
        (void*)HookedMessageBoxW,
    },
};

BOOL Wh_ModInit() {
    Wh_Log(L"Init");
    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}
