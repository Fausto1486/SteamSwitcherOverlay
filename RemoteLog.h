#pragma once
// RemoteLog.h — best-effort forwarding of this DLL's own log lines to
// SteamSwitcher.exe's optional debug-log pipe (SteamSwitcherOverlayLogPipe).
//
// Mirrors ModKit.cpp's own EnqueueLog/LogThread pattern in direction: this
// DLL connects OUT as a client, into a pipe SteamSwitcher.exe hosts as
// server (SteamSwitcher.OverlayLogMonitor) — same relationship as
// ModKitPipe, just for debug text only.
//
// Deliberately skips the queue + background-thread machinery ModKit.cpp
// uses for its own log delivery: this call is already synchronous end to
// end (Logging::LogFmt calls straight into here, no local file anymore),
// and CreateFileA against a pipe that doesn't exist fails instantly
// (ERROR_FILE_NOT_FOUND) rather than blocking — so a single inline
// best-effort attempt per call costs nothing when SteamSwitcher isn't
// running. Unlike ModKit's HOOK/POOLINFO packets, a dropped debug line here
// has no functional consequence, so no retry loop is needed either.

#include <Windows.h>
#include <string>

namespace RemoteLog {

    constexpr const char* kPipeName = "\\\\.\\pipe\\SteamSwitcherOverlayLogPipe";

    inline void Send(const std::string& msg) {
        HANDLE hPipe = CreateFileA(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) return; // nobody listening right now - fine, best-effort
        DWORD written = 0;
        WriteFile(hPipe, msg.c_str(), static_cast<DWORD>(msg.size()), &written, nullptr);
        CloseHandle(hPipe);
    }

} // namespace RemoteLog