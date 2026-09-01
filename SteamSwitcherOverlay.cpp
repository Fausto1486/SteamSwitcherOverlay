// SteamSwitcherOverlay.cpp — entry point.
//
// PRODUCTION injection: SteamSwitcher's own InjectDll (CreateRemoteThread +
// LoadLibraryA), the same mechanism already used for ModKit.dll — see
// OVERLAY-REDESIGN-RESULT.md's top-level architecture statement. Under this
// mechanism, DllMain is GUARANTEED by the OS to run in exactly the process
// SteamSwitcher already opened a handle to — there is no "wrong process"
// risk to guard against at all.
//
// TESTING HISTORY: this DLL could previously also be loaded via a
// standalone CbtInjector.exe test harness (SetWindowsHookEx/WH_CBT), used
// throughout this project's early development to validate hooking/
// rendering in isolation without needing a full SteamSwitcher session.
// That path had a REAL wrong-process risk (thread-ID reuse can fire a CBT
// hook in an unrelated process) that production injection does not share.
// Testing now happens directly through a real SteamSwitcher session
// instead — the CBT-specific export that path needed has been removed
// (see git history if it's ever needed again).
//
// PIN THIS MODULE FIRST — before anything else, including
// DisableThreadLibraryCalls. Originally added for a CBT-specific reason
// (the test injector released its hook once its own timing measurement
// finished, which could unload this module out from under a still-running
// background thread — a real bug found and fixed during testing, see
// OVERLAY-REDESIGN-RESULT.md's "The false negative" section). Kept as
// general defensive practice under production injection too — SteamSwitcher
// has its own explicit uninject path (UninjectSingleAsync/UninjectAllAsync
// in ModInjector.cs) that could plausibly race a FreeLibrary call against
// this DLL's own background threads the same way; pinning costs nothing
// and closes that class of risk regardless of which injection path is used.

#include <Windows.h>
#include <cstdint>
#include <string>
#include "Logging.h"
#include "PresentHookKit.h"
#include "OverlayPipe.h"

namespace {

    void OnHotkeyToggle() {
        PresentHookKit::RequestAttach();
        Overlay::ToggleStatusPanel();
    }

    void OnSetModChannel(bool enabled) {
        if (enabled) PresentHookKit::RequestAttach(); // channel enable is also a real trigger, not just the hotkey
        Overlay::SetChannel2Enabled(enabled);
        Logging::LogFmt("[SteamSwitcherOverlay] Channel 2 (mods) %s", enabled ? "enabled" : "disabled");
    }

    void OnToast(const std::string& text) {
        // Notifications are independent of the mods-panel toggle (INSERT)
        // and independent of SETMODCHANNEL — a TOAST| message alone must
        // be enough to bring up rendering, or it would just sit queued
        // (and potentially expire, 3s lifetime) while nothing is actually
        // drawing yet. Previously only INSERT/SETMODCHANNEL triggered
        // attach; this was a real gap.
        PresentHookKit::RequestAttach();
        Overlay::PushInfoToast(text);
        Logging::LogFmt("[SteamSwitcherOverlay] Toast queued: %s", text.c_str());
    }

    void OnCloseConfigWindows() {
        // See ModKit.h's ModKit_CloseAllConfigWindows for the full design -
        // this is the eager, mode-switch-triggered close, sent the instant
        // SteamSwitcher's NotificationMode dropdown changes (mirrors
        // _overlay.CloseOverlay()/CloseStatusPanel() firing at the same
        // moment on the C# side, see ModsPanel.cs's
        // Cmb_notificationMode_Changed).
        ModKitInterop::CloseAllConfigWindows();
        ModKitInterop::CloseAllStatsWindows();
        // Defers clearing the overlay's own g_openConfigPanels/
        // g_openStatsPanels to the render thread - see
        // g_pendingPanelListClear's own comment for why this can't just
        // clear() them directly from here (this runs on OverlayPipe's
        // dispatch thread, not the render thread that owns those lists).
        Overlay::g_pendingPanelListClear = true;
    }

    void OnGameInfo(const std::string& gameName, int64_t launchEpochMs, const std::string& profileName) {
        // No PresentHookKit::RequestAttach() here, unlike OnToast/
        // OnSetModChannel(true) above - GAMEINFO is always sent right after
        // SETMODCHANNEL|1 (see ModsPanel.cs's SendGameInfoToOverlay call
        // site), which already triggers attach on its own. This just needs
        // to update the header data itself for whenever the panel next
        // draws.
        Overlay::g_session.Set(gameName, launchEpochMs, profileName);
        Logging::LogFmt("[SteamSwitcherOverlay] GAMEINFO: %s (profile=%s)", gameName.c_str(), profileName.c_str());
    }

    void OnGameHwnd(HWND hwnd) {
        // See PresentHookKit.h's own g_confirmedGameHwnd comment for why
        // this exists at all. No RequestAttach() call needed here either,
        // same reasoning as OnGameInfo above - always sent alongside
        // SETMODCHANNEL|1, which already triggers attach.
        PresentHookKit::SetConfirmedGameHwnd(hwnd);
    }

    void OnRuntimeKind(const std::string& kind) {
        // See OverlayContent.h's own RuntimeInfo comment - this is the only
        // path feeding the status panel's MONO badge, reached over this
        // DLL's own pipe rather than through ModKit's shared memory on
        // purpose: detecting what kind of game this is shouldn't depend on
        // whether modding infrastructure happens to be present at all.
        Overlay::g_runtimeInfo.Set(kind);
    }

    DWORD WINAPI StartupWorkerThreadProc(LPVOID) {
        // No PID-guard check here — see this file's own header comment.
        // Under production (SteamSwitcher's own LoadLibraryA), the OS
        // guarantees this thread is already running in the correct
        // process; there's nothing to verify. A previous version of this
        // function DID check for a CBT-testing-specific shared-memory
        // marker here — that was a critical bug: under real production
        // injection, that marker is never created, so the check always
        // failed and this function always returned immediately without
        // doing anything at all. Found during a full pre-production
        // review — see OVERLAY-REDESIGN-RESULT.md.
        Logging::LogFmt("[SteamSwitcherOverlay] Startup worker running, pid=%lu", GetCurrentProcessId());
        HotkeyPoll::Start(&OnHotkeyToggle);
        OverlayPipe::Start(&OnSetModChannel, &OnToast, &OnCloseConfigWindows, &OnGameInfo, &OnGameHwnd, &OnRuntimeKind);
        PresentHookKit::InstallAll();
        return 0;
    }

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCSTR>(hModule), &pinned);

        DisableThreadLibraryCalls(hModule);

        // Never do real work inline in DllMain (loader-lock lesson,
        // consistent across every file in this project) — spawn a thread.
        HANDLE h = CreateThread(nullptr, 0, StartupWorkerThreadProc, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        // lpReserved is a documented Microsoft signal: non-NULL means the
        // PROCESS is terminating (game closing normally/crashing), NULL
        // means this is a live FreeLibrary-driven unload (e.g. an explicit
        // uninject while the game keeps running). These need different
        // cleanup: see PresentHookKit::UninstallAll's own comment on why
        // process-terminating DETACH must not touch the game's D3D device
        // at all — confirmed via two separate real hangs, both of which
        // stopped the whole game process from exiting until force-killed.
        OverlayPipe::Stop();
        PresentHookKit::UninstallAll(lpReserved != nullptr);
    }
    return TRUE;
}