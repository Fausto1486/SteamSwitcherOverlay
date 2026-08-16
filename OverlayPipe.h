#pragma once
// OverlayPipe.h — named pipe server hosted by this DLL (mirrors ModKitCmdPipe's
// existing shape: the DLL running inside the game process hosts the server,
// SteamSwitcher connects as client and pushes messages). Pipe name:
// SteamSwitcherOverlayPipe. Owns ALL runtime configuration for this DLL —
// ModKit.cpp's OVERLAYMODE| command is obsolete under this design and
// should be removed if this ships (don't keep both mechanisms alive at
// once — see OVERLAY-DLL-BLUEPRINT.md's own note on this).
//
// Protocol: plain KEY|value text, matching every other pipe in this
// codebase for consistency.
//   SETMODCHANNEL|1 / SETMODCHANNEL|0  — enable/disable Channel 2 (mod content)
//   TOAST|<text>                        — push a generic SteamSwitcher-driven toast (Channel 1)
//   CLOSECONFIGWINDOWS                  — close every open mod config window,
//                                         native or overlay-registered alike
//                                         (see ModKit.h's ModKit_CloseAllConfigWindows).
//                                         Sent the instant SteamSwitcher's
//                                         NotificationMode dropdown changes,
//                                         same trigger as _overlay.CloseOverlay()/
//                                         CloseStatusPanel() on the C# side -
//                                         see ModsPanel.cs's
//                                         Cmb_notificationMode_Changed.
//   GAMEINFO|<name>|<epochMs>|<profile> — session info for the status
//                                         panel's header (game name, launch
//                                         time, profile/persona name), sent
//                                         from ModsPanel.cs's
//                                         SendGameInfoToOverlay right after
//                                         SETMODCHANNEL|1. Mirrors
//                                         ModsStatusPanel.cs's own
//                                         SetSessionInfo on the Toast side —
//                                         same three fields, same meaning.
//                                         epochMs of 0 means "unknown, don't
//                                         show a timer line". Parsed from
//                                         the right (last field = profile,
//                                         next = epochMs, remainder = name)
//                                         rather than naive left-to-right
//                                         splitting, so a game name that
//                                         happens to contain '|' doesn't
//                                         corrupt the fields after it — this
//                                         is not escaped on the sending side.
//
// Extend the dispatch table below as real Channel-1 use cases emerge —
// don't over-design placeholder message types now, per the blueprint.

#include <Windows.h>
#include <cstdint>
#include <string>
#include "Logging.h"

namespace OverlayPipe {

    constexpr const char* kPipeName = "\\\\.\\pipe\\SteamSwitcherOverlayPipe";
    constexpr DWORD kBufferSize = 4096;

    inline volatile bool g_running = false;
    inline void (*g_onSetModChannel)(bool enabled) = nullptr;
    inline void (*g_onToast)(const std::string& text) = nullptr;
    inline void (*g_onCloseConfigWindows)() = nullptr;
    inline void (*g_onGameInfo)(const std::string& gameName, int64_t launchEpochMs, const std::string& profileName) = nullptr;

    // StartsWith dispatch, one callback per message type — same shape as
    // ModPipeMonitor.cs/CmdPipeThread elsewhere in this codebase, per the
    // blueprint's own consistency note.
    inline void DispatchMessage(const std::string& msg) {
        if (msg.rfind("SETMODCHANNEL|", 0) == 0) {
            std::string val = msg.substr(14);
            bool enabled = !val.empty() && val[0] == '1';
            if (g_onSetModChannel) g_onSetModChannel(enabled);
            Logging::LogFmt("[OverlayPipe] SETMODCHANNEL -> %s", enabled ? "1" : "0");
        }
        else if (msg.rfind("TOAST|", 0) == 0) {
            std::string text = msg.substr(6);
            if (g_onToast) g_onToast(text);
            Logging::LogFmt("[OverlayPipe] TOAST: %s", text.c_str());
        }
        else if (msg.rfind("CLOSECONFIGWINDOWS", 0) == 0) {
            if (g_onCloseConfigWindows) g_onCloseConfigWindows();
            Logging::LogFmt("[OverlayPipe] CLOSECONFIGWINDOWS");
        }
        else if (msg.rfind("GAMEINFO|", 0) == 0) {
            std::string rest = msg.substr(9);

            // Split from the right - see this file's own header comment on
            // GAMEINFO for why (an un-escaped '|' inside the game name
            // shouldn't be able to corrupt the fields after it).
            size_t lastPipe = rest.rfind('|');
            std::string profile = (lastPipe != std::string::npos) ? rest.substr(lastPipe + 1) : "";
            std::string rest2 = (lastPipe != std::string::npos) ? rest.substr(0, lastPipe) : rest;

            size_t secondPipe = rest2.rfind('|');
            std::string epochStr = (secondPipe != std::string::npos) ? rest2.substr(secondPipe + 1) : rest2;
            std::string gameName = (secondPipe != std::string::npos) ? rest2.substr(0, secondPipe) : "";

            int64_t epochMs = 0;
            try { epochMs = std::stoll(epochStr); }
            catch (...) { epochMs = 0; } // malformed - treat as "unknown", not a crash

            if (g_onGameInfo) g_onGameInfo(gameName, epochMs, profile);
            Logging::LogFmt("[OverlayPipe] GAMEINFO: %s (profile=%s)", gameName.c_str(), profile.c_str());
        }
        else {
            Logging::LogFmt("[OverlayPipe] Unrecognized message: %s", msg.c_str());
        }
    }

    inline DWORD WINAPI ServerThreadProc(LPVOID) {
        while (g_running) {
            HANDLE hPipe = CreateNamedPipeA(
                kPipeName,
                PIPE_ACCESS_INBOUND,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                kBufferSize, kBufferSize,
                0, nullptr);

            if (hPipe == INVALID_HANDLE_VALUE) {
                Logging::LogFmt("[OverlayPipe] CreateNamedPipeA failed, error %lu", GetLastError());
                Sleep(1000);
                continue;
            }

            BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) {
                CloseHandle(hPipe);
                continue;
            }

            char buf[kBufferSize];
            DWORD bytesRead = 0;
            while (g_running && ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buf[bytesRead] = '\0';
                DispatchMessage(std::string(buf));
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
        return 0;
    }

    inline void Start(void (*onSetModChannel)(bool), void (*onToast)(const std::string&),
        void (*onCloseConfigWindows)(),
        void (*onGameInfo)(const std::string&, int64_t, const std::string&))
    {
        if (g_running) return;
        g_onSetModChannel = onSetModChannel;
        g_onToast = onToast;
        g_onCloseConfigWindows = onCloseConfigWindows;
        g_onGameInfo = onGameInfo;
        g_running = true;
        HANDLE h = CreateThread(nullptr, 0, ServerThreadProc, nullptr, 0, nullptr);
        if (h) {
            CloseHandle(h);
            Logging::LogFmt("[OverlayPipe] Server started on %s", kPipeName);
        }
        else {
            g_running = false;
            Logging::LogFmt("[OverlayPipe] CreateThread failed.");
        }
    }

    inline void Stop() {
        g_running = false;
    }

} // namespace OverlayPipe