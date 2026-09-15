// ==WindhawkMod==
// @id              disable-message-beep
// @name            Disable Message Beep
// @description     Silences the system sounds played through MessageBeep (error, warning, question and notification dings), with per-sound control.
// @version         1.0.0
// @author          Leorik69
// @github          https://github.com/Leorik69
// @include         *
// @compilerOptions -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Disable Message Beep

Silences the built-in system sounds that Windows apps trigger through the
`MessageBeep` API - the dings you hear on error dialogs, invalid keystrokes,
and other message boxes.

Every sound is enabled by default; use the settings to keep the ones you still
want and mute the rest. Toggling a setting takes effect immediately, without
restarting the affected program.

This mod only affects sounds played through `MessageBeep`. Sounds played
through other APIs (such as `PlaySound` or the audio session) are not touched.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- muteSimpleBeep: true
  $name: Mute the simple beep
  $description: >-
    The standard beep (MB_OK / the classic PC speaker style ding).
- muteError: true
  $name: Mute the error sound
  $description: Played by error message boxes (MB_ICONHAND).
- muteWarning: true
  $name: Mute the warning sound
  $description: Played by warning message boxes (MB_ICONEXCLAMATION).
- muteQuestion: true
  $name: Mute the question sound
  $description: Played by question message boxes (MB_ICONQUESTION).
- muteInformation: true
  $name: Mute the information sound
  $description: Played by information message boxes (MB_ICONASTERISK).
*/
// ==/WindhawkModSettings==

struct {
    bool muteSimpleBeep;
    bool muteError;
    bool muteWarning;
    bool muteQuestion;
    bool muteInformation;
} g_settings;

using MessageBeep_t = decltype(&MessageBeep);
MessageBeep_t MessageBeep_Original;

// Returns true when the given MessageBeep type is configured to be muted.
bool ShouldMute(UINT uType) {
    switch (uType) {
        case 0xFFFFFFFF:  // MB_OK is 0, the simple beep uses (UINT)-1.
            return g_settings.muteSimpleBeep;
        case MB_OK:
            return g_settings.muteSimpleBeep;
        case MB_ICONHAND:  // Same value as MB_ICONERROR / MB_ICONSTOP.
            return g_settings.muteError;
        case MB_ICONEXCLAMATION:  // Same value as MB_ICONWARNING.
            return g_settings.muteWarning;
        case MB_ICONQUESTION:
            return g_settings.muteQuestion;
        case MB_ICONASTERISK:  // Same value as MB_ICONINFORMATION.
            return g_settings.muteInformation;
        default:
            return false;
    }
}

BOOL WINAPI MessageBeep_Hook(UINT uType) {
    if (ShouldMute(uType)) {
        Wh_Log(L"Muting MessageBeep, type=0x%08X", uType);
        // Report success without playing the sound, matching the contract
        // callers expect from MessageBeep.
        return TRUE;
    }

    return MessageBeep_Original(uType);
}

void LoadSettings() {
    g_settings.muteSimpleBeep = Wh_GetIntSetting(L"muteSimpleBeep");
    g_settings.muteError = Wh_GetIntSetting(L"muteError");
    g_settings.muteWarning = Wh_GetIntSetting(L"muteWarning");
    g_settings.muteQuestion = Wh_GetIntSetting(L"muteQuestion");
    g_settings.muteInformation = Wh_GetIntSetting(L"muteInformation");
}

// The mod is being initialized, load settings, hook functions, and do other
// initialization stuff if required.
BOOL Wh_ModInit() {
    Wh_Log(L"Init");

    LoadSettings();

    HMODULE hUser32 =
        LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hUser32) {
        Wh_Log(L"Failed to load user32.dll");
        return FALSE;
    }

    auto pMessageBeep =
        (MessageBeep_t)GetProcAddress(hUser32, "MessageBeep");
    if (!pMessageBeep) {
        Wh_Log(L"Failed to resolve MessageBeep");
        return FALSE;
    }

    Wh_SetFunctionHook((void*)pMessageBeep, (void*)MessageBeep_Hook,
                       (void**)&MessageBeep_Original);

    return TRUE;
}

// The mod is being unloaded, free all allocated resources.
void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}

// The mod setting were changed, reload them.
void Wh_ModSettingsChanged() {
    Wh_Log(L"SettingsChanged");

    LoadSettings();
}
