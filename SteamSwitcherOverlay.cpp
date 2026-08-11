// SteamSwitcherOverlay.cpp — entry point.
//
// PRODUCTION injection: SteamSwitcher's own InjectDll (CreateRemoteThread +
// LoadLibraryA), the same mechanism already used for ModKit.dll — see
// OVERLAY-REDESIGN-RESULT.md's top-level architecture statement. Under this
// mechanism, DllMain is GUARANTEED by the OS to run in exactly the process
// SteamSwitcher already opened a handle to — there is no "wrong process"
// risk to guard against at all.
//
// TESTING ONLY, separately: this DLL can also be loaded via the standalone
// CbtInjector.exe test harness (SetWindowsHookEx/WH_CBT), used throughout
// this project's development to validate hooking/rendering in isolation
// without needing a full SteamSwitcher session. That path has a REAL
// wrong-process risk (thread-ID reuse can fire a CBT hook in an unrelated
// process) that production injection does not share — CbtProc export at
// the bottom of this file exists solely to keep that test path usable.
//
// PIN THIS MODULE FIRST — before anything else, including
// DisableThreadLibraryCalls. Originally added for a CBT-specific reason
// (the test injector releases its hook once its own timing measurement
// finishes, which could unload this module out from under a still-running
// background thread — a real bug found and fixed during testing, see
// OVERLAY-REDESIGN-RESULT.md's "The false negative" section). Kept as
// general defensive practice under production injection too — SteamSwitcher
// has its own explicit uninject path (UninjectSingleAsync/UninjectAllAsync
// in ModInjector.cs) that could plausibly race a FreeLibrary call against
// this DLL's own background threads the same way; pinning costs nothing
// and closes that class of risk regardless of which injection path is used.

#include <Windows.h>
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
        OverlayPipe::Start(&OnSetModChannel, &OnToast);
        PresentHookKit::InstallAll();
        return 0;
    }

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
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
        OverlayPipe::Stop();
        PresentHookKit::UninstallAll();
    }
    return TRUE;
}

// TESTING-HARNESS EXPORT ONLY — required by CbtInjector.exe's
// SetWindowsHookEx(WH_CBT, ...) call, which resolves this export by name
// before installing its hook. NEVER called in production (SteamSwitcher's
// own InjectDll never touches this). Kept so the standalone test harness
// remains usable for future isolated hooking/rendering validation without
// needing a rebuild — has zero effect otherwise, this function is never
// invoked by anything in the real production path.
extern "C" __declspec(dllexport) LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
