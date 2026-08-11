#pragma once
// HotkeyPoll.h — independent INSERT poll for toggling the status panel.
// Matches SteamSwitcher's existing toast-mode convention (bare INSERT) —
// RCTRL+INSERT is reserved for mod-specific hotkeys instead, not shared
// with the overlay's own panel toggle. No dependency on ModKit.dll being
// loaded at all, per OVERLAY-DLL-BLUEPRINT.md.
//
// Tradeoff worth knowing: bare INSERT is a fairly common in-game keybind
// (quicksave/quickload in a number of older/action titles) — the archive's
// original version deliberately used RCTRL+INSERT specifically to reduce
// that collision risk. Switched to bare INSERT here for consistency with
// SteamSwitcher's own existing convention instead, per explicit choice —
// if a real collision with a specific game's own keybind ever turns up,
// this is the tradeoff that caused it.

#include <Windows.h>
#include "Logging.h"

namespace HotkeyPoll {

    inline volatile bool g_running = false;
    inline void (*g_onToggle)() = nullptr;

    inline DWORD WINAPI PollThreadProc(LPVOID) {
        bool wasDown = false;
        while (g_running) {
            bool isDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
            if (isDown && !wasDown && g_onToggle) {
                g_onToggle();
            }
            wasDown = isDown;
            Sleep(50); // coarse poll — this is a UX hotkey, not a timing-critical path
        }
        return 0;
    }

    // onToggle is called from the polling thread itself — keep it fast and
    // non-blocking, same caution as any other background-thread callback
    // in this DLL (see SteamSwitcherOverlay.cpp's own DllMain comments on
    // why nothing heavy runs inline off the entry point).
    inline void Start(void (*onToggle)()) {
        if (g_running) return;
        g_onToggle = onToggle;
        g_running = true;
        HANDLE h = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
        if (h) {
            CloseHandle(h);
            Logging::LogFmt("[HotkeyPoll] Started (INSERT).");
        }
        else {
            g_running = false;
            Logging::LogFmt("[HotkeyPoll] CreateThread failed.");
        }
    }

    inline void Stop() {
        g_running = false;
    }

} // namespace HotkeyPoll
