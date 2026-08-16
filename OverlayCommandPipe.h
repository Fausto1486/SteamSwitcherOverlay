#pragma once
// OverlayCommandPipe.h — best-effort sending of short commands FROM this DLL
// TO SteamSwitcher.exe's command pipe (SteamSwitcherOverlayCmdPipe).
//
// Direction and shape mirror RemoteLog.h exactly (this DLL connects OUT as
// a client, into a pipe SteamSwitcher hosts as a server -
// SteamSwitcher.OverlayCommandMonitor) — the only difference is this
// carries short single-word commands instead of free-text debug lines.
// Same reasoning for the single inline CreateFileA/WriteFile attempt with
// no retry loop: a pipe that doesn't exist fails instantly
// (ERROR_FILE_NOT_FOUND) rather than blocking, and a dropped command here
// (SteamSwitcher not running, or its command monitor not started yet) has
// no functional consequence worth retrying for — the player can just click
// Logs again.
//
// Currently sends one command:
//   SHOWLOGS — requests SteamSwitcher bring its Log window to the front,
//              anchored near this overlay's own on-screen position. See
//              DrawStatusPanel's Logs button in OverlayContent.h.

#include <Windows.h>
#include <string>

namespace OverlayCommandPipe {

    constexpr const char* kPipeName = "\\\\.\\pipe\\SteamSwitcherOverlayCmdPipe";

    inline void Send(const std::string& msg) {
        HANDLE hPipe = CreateFileA(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) return; // nobody listening right now - fine, best-effort
        DWORD written = 0;
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.size()), &written, nullptr);
        CloseHandle(hPipe);
    }

    inline void SendShowLogs() {
        Send("SHOWLOGS");
    }

} // namespace OverlayCommandPipe