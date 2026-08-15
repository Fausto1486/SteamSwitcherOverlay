#pragma once
// OverlayContent.h — DrawOverlay(): the actual pixels of the in-game
// overlay. Ported from the archive's OverlayContent.h (originally a port
// of SteamSwitcher's ModsStatusPanel.cs + ToastOverlay.cs) — same colors,
// layout, and toast-diffing logic, verbatim where the underlying data
// access didn't need to change.
//
// DEVIATIONS FROM THE ARCHIVE VERSION (all data-access plumbing, zero
// visual changes):
//   - ModKit_GetSharedData("MODS"/#H/#O/#B/#F/#G) → SharedDataReader.h's
//     ReadAllModStatus(), same underlying shared-memory block, read
//     directly instead of via a ModKit.dll export call.
//   - ModKit_HasButton/ClickButton/IsPoolSearching/IsPoolClearing →
//     ModKitInterop.h (GetProcAddress-based, degrades gracefully — see
//     that file's header comment for why these don't work yet on current
//     ModKit.dll).
//   - ModKit_GetSharedData("overlay.mode",...) gating → replaced with
//     g_channel2Enabled, set by OverlayPipe's SETMODCHANNEL message (the
//     archive's OVERLAYMODE| pipe command is obsolete under this design —
//     see PresentHookKit.h's own header comment).
//   - ModKit_SetSharedData("overlay.attached","1") → dropped. This DLL
//     doesn't own ModKit's shared-memory block (SharedDataReader.h only
//     ever reads it), so writing into it without ModKit.dll's own locking
//     isn't safe. SteamSwitcher-side "is the overlay attached" detection
//     needs its own mechanism if ever wanted — out of scope here, logged
//     via Logging::LogFmt instead so it's at least visible in SteamSwitcher's
//     debug log panel.
//   - ModKit_RegisterHotkey(VK_INSERT, ...) → HotkeyPoll.h already owns
//     bare INSERT independently (matches SteamSwitcher's existing
//     toast-mode convention — see HotkeyPoll.h's own header comment on
//     the RCTRL+INSERT tradeoff this deliberately moved away from);
//     ToggleStatusPanel is wired as its
//     callback from SteamSwitcherOverlay.cpp instead.
//
// Included by PresentHookKit.h. DrawOverlay() always runs between an
// ImGui::NewFrame() and ImGui::Render() already issued by the caller.

#include "SharedDataReader.h"
#include "ModKitInterop.h"
#include "Logging.h"
#include "imgui.h"
#include <Windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace Overlay {

    // ── Colors — verbatim from the archive ────────────────────────────────────
    inline ImU32 Col(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

    namespace Colors {
        static const ImU32 BG = Col(18, 18, 22);
        static const ImU32 ACCENT = Col(150, 150, 160);
        static const ImU32 TEXT = Col(235, 235, 235);
        static const ImU32 DISABLED_TEXT = Col(130, 130, 130);
        static const ImU32 ROW_HOVER = Col(40, 40, 48);
        static const ImU32 DOT_HOOKED = Col(60, 200, 80);
        static const ImU32 DOT_HOOKED_CFG = Col(170, 110, 220);
        static const ImU32 DOT_OFF_CFG = Col(245, 140, 30);
        static const ImU32 DOT_OFF = Col(140, 120, 120);
        static const ImU32 POOL_SEARCH_BG = Col(18, 38, 70);
        static const ImU32 POOL_SEARCH_ACC = Col(80, 150, 220);
        static const ImU32 POOL_SEARCH_TXT = Col(180, 210, 245);
        static const ImU32 POOL_INFO_BG = Col(20, 20, 26);
        static const ImU32 POOL_INFO_ACC = Col(120, 120, 130);
        static const ImU32 POOL_INFO_TXT = Col(190, 190, 195);

        static const ImU32 TOAST_BG_ON = Col(20, 65, 30);
        static const ImU32 TOAST_BG_OFF = Col(65, 18, 18);
        static const ImU32 TOAST_BG_OUT = Col(65, 48, 8);
        static const ImU32 TOAST_BG_SCAN = Col(18, 38, 70);
        static const ImU32 TOAST_BG_INFO = Col(30, 30, 36);
        static const ImU32 TOAST_ACC_ON = Col(80, 200, 100);
        static const ImU32 TOAST_ACC_OFF = Col(200, 70, 70);
        static const ImU32 TOAST_ACC_OUT = Col(200, 150, 40);
        static const ImU32 TOAST_ACC_SCAN = Col(80, 150, 220);
        static const ImU32 TOAST_ACC_INFO = Col(150, 150, 160);
    }

    // ── Channel 2 (mod content) gate — set by OverlayPipe's SETMODCHANNEL.
    // Replaces the archive's "overlay.mode" shared-data key check. ───────────
    inline bool g_channel2Enabled = false;
    inline void SetChannel2Enabled(bool enabled) { g_channel2Enabled = enabled; }

    // ── Per-mod row, read fresh every frame from SharedDataReader ────────────
    struct ModRow {
        std::string modName;
        bool clickable = false;
        bool hooked = false;
        bool outdated = false;
        bool busy = false;
        bool hasActiveFlags = false;
        std::vector<std::string> activeLabels; // #F ∪ #G, merged + sorted for display
    };

    inline std::vector<ModRow> ReadModRows() {
        std::vector<ModRow> rows;
        auto statuses = SharedDataReader::ReadAllModStatus();
        for (auto& [modName, status] : statuses) {
            ModRow row;
            row.modName = modName;
            row.clickable = ModKitInterop::HasButton(modName.c_str());
            row.hooked = status.hooked;
            row.outdated = status.outdated;
            row.busy = status.busy;

            row.activeLabels = status.activeSubFlags;
            for (auto& g : status.activeFeatureLabels)
                if (std::find(row.activeLabels.begin(), row.activeLabels.end(), g) == row.activeLabels.end())
                    row.activeLabels.push_back(g);
            row.hasActiveFlags = !status.activeSubFlags.empty();
            std::sort(row.activeLabels.begin(), row.activeLabels.end());
            rows.push_back(std::move(row));
        }
        // ReadAllModStatus returns an unordered_map — sort by name for a
        // stable, deterministic row order (the archive's version got this
        // for free from MODS's own comma-separated order; that ordering
        // isn't preserved through SharedDataReader's map, so restore it
        // explicitly here instead).
        std::sort(rows.begin(), rows.end(), [](const ModRow& a, const ModRow& b) { return a.modName < b.modName; });

        // "Force Inject ModKit" can leave ModKit.dll resident with zero
        // mods reporting — nothing in SharedDataReader's per-mod entries
        // ever reflects that, so the panel showed "No mods injected" with
        // zero indication ModKit itself was there, and zero visible change
        // when it was later uninjected (same gap the C# toast-mode panel
        // had — see ModsPanel.cs's BuildStatusPanelRows). Only synthesized
        // when there are no real mod rows already; with mods present
        // ModKit's presence is implied.
        if (rows.empty() && SharedDataReader::IsModKitPresent()) {
            ModRow row;
            row.modName = "ModKit (no mods loaded)";
            row.clickable = false;
            row.hooked = true;
            row.hasActiveFlags = false;
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // ── Toast stack — verbatim from the archive ───────────────────────────────
    enum class ToastKind { On, Off, Scanning, Outdated, Info };

    struct Toast {
        std::string key, label;
        ToastKind kind;
        std::chrono::steady_clock::time_point birth;
    };

    struct ToastState {
        std::vector<Toast> toasts;
        CRITICAL_SECTION lock;

        ToastState() { InitializeCriticalSection(&lock); }
        ~ToastState() { DeleteCriticalSection(&lock); }

        void Push(const std::string& key, const std::string& label, ToastKind kind) {
            EnterCriticalSection(&lock);
            for (auto& t : toasts) {
                if (t.key == key) { t.label = label; t.kind = kind; t.birth = std::chrono::steady_clock::now(); LeaveCriticalSection(&lock); return; }
            }
            toasts.push_back({ key, label, kind, std::chrono::steady_clock::now() });
            static const size_t MAX_VISIBLE = 8;
            if (toasts.size() > MAX_VISIBLE) toasts.erase(toasts.begin());
            LeaveCriticalSection(&lock);
        }

        void Prune() {
            EnterCriticalSection(&lock);
            auto now = std::chrono::steady_clock::now();
            toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& t) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(now - t.birth).count() >= 3000;
            }), toasts.end());
            LeaveCriticalSection(&lock);
        }

        // Snapshot for rendering — DrawToastStack reads through this copy,
        // never the live vector directly, so it never holds the lock while
        // calling into ImGui (which could plausibly take a while, and
        // shouldn't be blocking the pipe thread's Push for that long).
        std::vector<Toast> Snapshot() {
            EnterCriticalSection(&lock);
            std::vector<Toast> copy = toasts;
            LeaveCriticalSection(&lock);
            return copy;
        }
    };
    inline ToastState g_toasts;

    struct PrevModState { bool hooked = false, outdated = false, busy = false; std::vector<std::string> activeLabels; bool seen = false; };
    inline std::unordered_map<std::string, PrevModState>& PrevStates() {
        static std::unordered_map<std::string, PrevModState> s;
        return s;
    }
    inline bool g_prevPoolSearching = false;

    inline void DiffAndPushToasts(const std::vector<ModRow>& rows) {
        auto& prev = PrevStates();

        for (const auto& row : rows) {
            PrevModState& p = prev[row.modName];
            // Baseline is "nothing active yet" (PrevModState's own default
            // construction — hooked/outdated/busy=false, activeLabels
            // empty), NOT the row's actual current state. Falling through
            // to the normal diff logic below means a mod that already
            // finished hooking before this DLL's first render frame (fast
            // auto-hookers like CharacterStats's ModKit_OnInjectionComplete
            // callback, combined with Daemon X Machina's ~1.5s Present-
            // correlation attach delay) still gets its "just hooked" toast
            // instead of silently swallowing it as an assumed baseline. A
            // genuinely not-yet-hooked mod still produces no toast either
            // way, since false→false is not a transition.
            if (!p.seen) p.seen = true;

            if (row.busy && !p.busy)
                g_toasts.Push(row.modName, row.modName, ToastKind::Scanning);

            if (row.hooked != p.hooked) {
                std::string label = row.modName;
                if (!row.activeLabels.empty()) {
                    label += " (";
                    for (size_t i = 0; i < row.activeLabels.size(); ++i) { if (i) label += ", "; label += row.activeLabels[i]; }
                    label += ")";
                }
                g_toasts.Push(row.modName, label, row.hooked ? ToastKind::On : ToastKind::Off);
            }

            if (row.outdated && !p.outdated)
                g_toasts.Push(row.modName, row.modName, ToastKind::Outdated);

            for (auto& lbl : row.activeLabels) {
                if (std::find(p.activeLabels.begin(), p.activeLabels.end(), lbl) == p.activeLabels.end())
                    g_toasts.Push(row.modName + ":" + lbl, row.modName + " (" + lbl + ")", ToastKind::On);
            }
            for (auto& lbl : p.activeLabels) {
                if (std::find(row.activeLabels.begin(), row.activeLabels.end(), lbl) == row.activeLabels.end())
                    g_toasts.Push(row.modName + ":" + lbl, row.modName + " (" + lbl + ")", ToastKind::Off);
            }

            p.hooked = row.hooked; p.outdated = row.outdated; p.busy = row.busy; p.activeLabels = row.activeLabels;
        }

        bool searching = ModKitInterop::IsPoolSearching();
        if (searching != g_prevPoolSearching) {
            g_toasts.Push("modkit:pool",
                searching ? "ModKit - searching for trampoline..." : "ModKit - trampoline pool ready",
                searching ? ToastKind::Scanning : ToastKind::On);
            g_prevPoolSearching = searching;
        }
    }

    // Public entry point for external code (SteamSwitcherOverlay.cpp's
    // OverlayPipe TOAST| handler) to push a generic, non-mod-related toast.
    // Keyed by an incrementing counter so distinct messages never collide
    // and overwrite each other the way same-mod toasts intentionally do.
    inline void PushInfoToast(const std::string& text) {
        static int counter = 0;
        g_toasts.Push("info:" + std::to_string(counter++), text, ToastKind::Info);
    }

    inline void DrawToastStack() {
        g_toasts.Prune();
        std::vector<Toast> toasts = g_toasts.Snapshot();
        if (toasts.empty()) return;

        const float TOAST_W = 360, TOAST_H = 36, BORDER_W = 4, MARGIN_R = 22, MARGIN_T = 56, GAP = 6;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float x0 = vp->WorkPos.x + vp->WorkSize.x - MARGIN_R - TOAST_W;
        float y = vp->WorkPos.y + MARGIN_T;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (auto& t : toasts) {
            ImU32 bg, acc; const char* badge;
            switch (t.kind) {
            case ToastKind::On:       bg = Colors::TOAST_BG_ON;   acc = Colors::TOAST_ACC_ON;   badge = "ON";       break;
            case ToastKind::Off:      bg = Colors::TOAST_BG_OFF;  acc = Colors::TOAST_ACC_OFF;  badge = "OFF";      break;
            case ToastKind::Scanning: bg = Colors::TOAST_BG_SCAN; acc = Colors::TOAST_ACC_SCAN; badge = "SCANNING"; break;
            case ToastKind::Outdated: bg = Colors::TOAST_BG_OUT;  acc = Colors::TOAST_ACC_OUT;  badge = "OUTDATED"; break;
            default:                  bg = Colors::TOAST_BG_INFO; acc = Colors::TOAST_ACC_INFO; badge = "";         break;
            }

            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + TOAST_W, y + TOAST_H), bg);
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + BORDER_W, y + TOAST_H), acc);

            ImVec2 badgeSize = ImGui::CalcTextSize(badge);
            float statW = 84;
            dl->PushClipRect(ImVec2(x0 + BORDER_W + 8, y), ImVec2(x0 + TOAST_W - statW - 6, y + TOAST_H), true);
            dl->AddText(ImVec2(x0 + BORDER_W + 8, y + (TOAST_H - ImGui::GetTextLineHeight()) * 0.5f), Colors::TEXT, t.label.c_str());
            dl->PopClipRect();

            dl->AddText(ImVec2(x0 + TOAST_W - 6 - badgeSize.x, y + (TOAST_H - ImGui::GetTextLineHeight()) * 0.5f), acc, badge);

            y += TOAST_H + GAP;
        }
    }

    inline bool g_statusPanelOpen = false;

    // Which mod's config window (if any) DrawConfigPanel below should
    // render - set on a row click regardless of whether that mod actually
    // registered with the bridge; DrawConfigPanel's own HasConfigWindow
    // check no-ops harmlessly if it didn't (legacy/unrebuilt mod, native
    // window instead - see TrackConfigWindowFor for that path). NOT cleared
    // when the in-game panel closes (INSERT) - drawing is gated on
    // g_statusPanelOpen at the call site instead (see DrawOverlay), so the
    // config window "hides" and reappears in the same spot rather than
    // resetting, same as the underlying ModKit.dll registration - only
    // actually clears on an explicit close (X / Escape) or a
    // notification-mode switch (ModKit_CloseAllConfigWindows).
    inline std::string g_openConfigModName;

    // ── Anchor window ────────────────────────────────────────────────────
    // ModConfigWindow.h (the per-mod config window, shared native C++ code)
    // finds where to place itself via FindWindowA(nullptr, "ModStatusPanel")
    // - see that file's ThreadProc. In toast mode that title belongs to a
    // real WinForms HWND (View/ModsStatusPanel.cs). This panel is drawn
    // entirely inside the game's own window via ImGui - there is no HWND
    // backing it - so that lookup always missed and fell through to
    // ModConfigWindow.h's ModInjectorWindow fallback (SteamSwitcher's own
    // window), which is why config panels always opened next to
    // SteamSwitcher instead of next to this panel.
    //
    // Fix: keep a real, invisible HWND titled "ModStatusPanel" in sync with
    // DrawStatusPanel's on-screen rect. It never needs to paint or receive
    // input - it exists purely as a FindWindowA/GetWindowRect target.
    inline HWND g_anchorHwnd = nullptr;

    // Render-thread-only bookkeeping: true while the anchor's title is set
    // to "ModStatusPanel". g_anchorHwnd is created on (and only ever safe
    // to touch from) the render thread - it never pumps its own message
    // queue, so a cross-thread SetWindowTextA/SetWindowPos call targeting
    // it would block the calling thread forever waiting for a queue that's
    // never serviced. This flag lets DrawOverlay() (always render-thread)
    // notice a close and clear the title itself, instead of the panel's
    // close path (CloseStatusPanel(), reachable from HotkeyPoll's own
    // thread) touching the HWND directly - that cross-thread call was a
    // real deadlock: it hung the hotkey-poll thread inside SetWindowTextA,
    // so INSERT stopped being detected at all after the first close.
    inline bool g_anchorTitleSet = false;

    // Cross-thread teardown handshake for g_anchorHwnd - see
    // RequestAnchorDestroyAndWait()'s comment below for why this exists
    // instead of just calling DestroyWindow() from whichever thread is
    // unloading the DLL.
    inline volatile bool g_anchorDestroyRequested = false;

    // Lazy, retried-every-frame notify to ModKit.dll that the overlay is
    // attached — moved here from PresentHookKit::InstallAll() rather than
    // firing once at attach time, for two reasons: (1) InstallAll() runs on
    // a dedicated worker thread immediately at DLL load, potentially before
    // ModKit.dll or any mod is even injected yet — a one-shot call there
    // that fails has nothing to retry it, unlike every other
    // ModKitInterop:: call in this file, which already runs every frame
    // from here and naturally retries; (2) GetModuleHandleA/GetProcAddress
    // both briefly need the loader lock, and calling them from that worker
    // thread while other mod DLLs are concurrently inside their own
    // DllMain (holding that lock, as the OS guarantees for DllMain) can
    // block this thread until injection settles - worse the earlier attach
    // happens (e.g. overlay already enabled from a previous session,
    // RequestAttach() firing almost immediately). Draw time is well past
    // that window and already proven safe for this exact pattern.
    inline HANDLE g_anchorDestroyedEvent = nullptr;

    inline LRESULT CALLBACK AnchorWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    inline void EnsureAnchorWindow() {
        if (g_anchorHwnd) return;
        static const char* kClassName = "SSOverlayStatusAnchor";
        WNDCLASSA wc = {};
        wc.lpfnWndProc = AnchorWndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = kClassName;
        RegisterClassA(&wc); // ok if already registered from a prior attach

        g_anchorHwnd = CreateWindowExA(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kClassName, "", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    }

    // Moves/resizes the anchor to match the panel's current screen rect and
    // (re)applies the "ModStatusPanel" title. Called every frame the panel
    // is open, same as the rest of DrawStatusPanel.
    inline void UpdateAnchorWindow(int x, int y, int w, int h) {
        EnsureAnchorWindow();
        if (!g_anchorHwnd) return;
        SetWindowTextA(g_anchorHwnd, "ModStatusPanel");
        SetWindowPos(g_anchorHwnd, nullptr, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
        g_anchorTitleSet = true;
    }

    // Clears the title (not destroy) when the panel closes, so a stray
    // config window opened afterward falls back to the SteamSwitcher
    // anchor instead of matching a stale/hidden panel rect - mirrors
    // ModsStatusPanel.cs's own Hide() and its header comment.
    //
    // MUST only ever be called from the render thread (see
    // g_anchorTitleSet's comment) - never call this directly from
    // CloseStatusPanel(), which can run on HotkeyPoll's own thread.
    inline void ClearAnchorWindow() {
        if (!g_anchorHwnd) return;
        SetWindowTextA(g_anchorHwnd, "");
    }

    // Must run before this DLL unloads (see PresentHookKit::UninstallAll) -
    // AnchorWndProc lives in this module, so a stale HWND left registered
    // to it would crash the next message dispatched to it after unload.
    // Only safe to call from the render thread that created g_anchorHwnd -
    // DestroyWindow silently fails (not deadlocks, unlike SetWindowText/
    // SetWindowPos) when called from any other thread, which is exactly
    // what leaked the window before: PresentHookKit::UninstallAll runs on
    // whatever thread is driving the DLL unload, essentially never the
    // render thread.
    inline void DestroyAnchorWindow() {
        if (!g_anchorHwnd) return;
        DestroyWindow(g_anchorHwnd);
        g_anchorHwnd = nullptr;
    }

    // Called from any thread (PresentHookKit::UninstallAll) to have the
    // anchor window destroyed correctly. Actual DestroyWindow() call
    // happens inside DrawOverlay() below - on the render thread - never
    // here. This just sets a flag and waits (bounded by timeoutMs, never
    // forever) for that to happen on the next Present call, which is why
    // the caller MUST issue this before removing the Present hook
    // (MH_Uninitialize) - see UninstallAll's own comment. If Present isn't
    // being called for any reason (game minimized/paused/already gone),
    // this simply times out and returns - the window is left to leak for
    // the rest of the process's life, same as before this fix, just no
    // longer the common case.
    inline void RequestAnchorDestroyAndWait(DWORD timeoutMs = 500) {
        if (!g_anchorHwnd) return;
        if (!g_anchorDestroyedEvent)
            g_anchorDestroyedEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (g_anchorDestroyedEvent) ResetEvent(g_anchorDestroyedEvent);
        g_anchorDestroyRequested = true;
        if (g_anchorDestroyedEvent)
            WaitForSingleObject(g_anchorDestroyedEvent, timeoutMs);
    }

    // ── Cursor / input arbitration ────────────────────────────────────────
    // Toast mode's ModsStatusPanel is a real top-level OS window - it gets
    // usable mouse input for free regardless of what the game does with the
    // cursor. This panel is drawn inside the game's own window instead, so
    // two separate things the game does every frame have to be actively
    // fought while the panel is open:
    //
    //  1. ClipCursor() - confines the pointer for camera look, re-asserted
    //     every frame, so releasing it once at open-time isn't enough (see
    //     ForceCursorUsable, called every DrawStatusPanel frame). Cursor
    //     VISIBILITY itself is no longer fought over via ShowCursor - see
    //     ForceCursorUsable's own comment for why that caused visible
    //     flicker and why ImGui's own drawn cursor (io.MouseDrawCursor)
    //     replaced it.
    //  2. Raw mouse input (RegisterRawInputDevices with RIDEV_NOLEGACY) -
    //     many games register this for camera look, which makes Windows
    //     stop delivering legacy WM_MOUSEMOVE/WM_LBUTTONDOWN/etc to every
    //     window in the process entirely (only WM_INPUT arrives instead).
    //     OverlayWndProc/ImGui only ever look at the legacy messages, so
    //     while raw input is registered, clicks on the panel never reach
    //     it at all even though it renders and the cursor is visible and
    //     unclipped - this is the one that actually blocks "stats"/"config"
    //     row clicks. Fixed by unregistering the game's own mouse raw-input
    //     device while the panel is open (restores legacy delivery) and
    //     re-registering it exactly as it was on close.
    inline bool g_cursorForced = false;
    inline RECT g_savedClip{};
    inline bool g_rawMouseSuppressed = false;
    inline std::vector<RAWINPUTDEVICE> g_savedRawDevices;
    // Deferred, not written to ImGui's io struct directly: ForceCursorUsable/
    // RestoreCursor below are reachable from threads other than the render
    // thread (RestoreCursor is documented as callable from HotkeyPoll's own
    // thread, via CloseStatusPanel) - writing io.MouseDrawCursor from there
    // would race ImGui::Render() running concurrently on the render thread.
    // Plain bool write/read here is safe (no tearing, no ImGui involvement);
    // DrawOverlay applies it to io.MouseDrawCursor itself, every frame, on
    // the one thread that's ever allowed to touch ImGui state.
    inline volatile bool g_wantMouseDrawCursor = false;

    inline void SuppressRawMouseCapture() {
        if (g_rawMouseSuppressed) return;
        g_rawMouseSuppressed = true; // set first - always balanced by RestoreRawMouseCapture below, even if nothing was found

        UINT count = 0;
        GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
        if (count == 0) return;

        std::vector<RAWINPUTDEVICE> devices(count);
        UINT got = GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE));
        if (got == (UINT)-1) return;
        devices.resize(got);

        for (auto& dev : devices) {
            // Generic mouse only (usage page 1, usage 2) - leave keyboard
            // and any other raw-input device (e.g. a controller) alone, the
            // panel only needs mouse messages back.
            if (dev.usUsagePage != 1 || dev.usUsage != 2) continue;
            g_savedRawDevices.push_back(dev);
            RAWINPUTDEVICE remove = dev;
            remove.dwFlags = RIDEV_REMOVE;
            remove.hwndTarget = nullptr;
            RegisterRawInputDevices(&remove, 1, sizeof(RAWINPUTDEVICE));
        }
    }

    inline void RestoreRawMouseCapture() {
        if (!g_rawMouseSuppressed) return;
        for (auto& dev : g_savedRawDevices)
            RegisterRawInputDevices(&dev, 1, sizeof(RAWINPUTDEVICE));
        g_savedRawDevices.clear();
        g_rawMouseSuppressed = false;
    }

    inline void ForceCursorUsable() {
        if (!g_cursorForced) {
            GetClipCursor(&g_savedClip); // remember pre-panel confinement, even "none" (full virtual screen)
            g_cursorForced = true;
        }
        ClipCursor(nullptr);
        SuppressRawMouseCapture();
        // Draw our OWN cursor via ImGui instead of fighting the game over
        // the OS hardware cursor (this used to call ShowCursor(TRUE) here
        // every frame). Many games touch cursor visibility/rendering
        // themselves every frame too (camera-look, or a custom UI cursor
        // sprite drawn as part of their own scene) - racing that via
        // ShowCursor is what caused the visible flicker, and a
        // game-drawn cursor sprite can never be "under" an ImGui window
        // that way since it's part of the GAME's draw calls, not ours.
        // ImGui's drawn cursor is the literal last thing in our own draw
        // list each frame (this hook runs after the game has already
        // finished rendering), so it's guaranteed on top regardless of
        // what the game does with its own cursor, and doesn't depend on
        // the OS-level ShowCursor state at all.
        //
        // Deferred via g_wantMouseDrawCursor rather than writing
        // io.MouseDrawCursor here directly - see that flag's own comment:
        // this function must stay safe to call from the render thread
        // only anyway (DrawStatusPanel/DrawConfigPanel), but keeping both
        // functions symmetric avoids a footgun if a future caller doesn't.
        g_wantMouseDrawCursor = true;
    }

    inline void RestoreCursor() {
        RestoreRawMouseCapture();
        g_wantMouseDrawCursor = false;
        if (!g_cursorForced) return;
        ClipCursor(&g_savedClip);
        g_cursorForced = false;
    }

    inline SRWLOCK g_trackedLock = SRWLOCK_INIT;
    inline std::vector<HWND> g_trackedConfigHwnds;

    inline DWORD WINAPI PollForConfigWindowThreadProc(LPVOID param) {
        std::string* className = reinterpret_cast<std::string*>(param);
        HWND hwnd = nullptr;
        for (int i = 0; i < 20 && !hwnd; ++i) {
            Sleep(100);
            hwnd = FindWindowA(className->c_str(), nullptr);
        }
        delete className;
        if (hwnd) {
            AcquireSRWLockExclusive(&g_trackedLock);
            g_trackedConfigHwnds.push_back(hwnd);
            ReleaseSRWLockExclusive(&g_trackedLock);
        }
        return 0;
    }

    inline void TrackConfigWindowFor(const std::string& modName) {
        auto* className = new std::string("CD" + modName + "CfgWnd");
        HANDLE h = CreateThread(nullptr, 0, PollForConfigWindowThreadProc, className, 0, nullptr);
        if (h) CloseHandle(h);
        else delete className;
    }

    inline void CloseTrackedConfigWindows() {
        AcquireSRWLockExclusive(&g_trackedLock);
        for (HWND h : g_trackedConfigHwnds)
            if (h && IsWindow(h)) PostMessageA(h, WM_CLOSE, 0, 0);
        g_trackedConfigHwnds.clear();
        ReleaseSRWLockExclusive(&g_trackedLock);
    }

    inline void CloseStatusPanel() {
        if (!g_statusPanelOpen) return;
        g_statusPanelOpen = false;
        CloseTrackedConfigWindows();
        RestoreCursor();
        // Anchor title is cleared from DrawOverlay() instead, on the render
        // thread - see g_anchorTitleSet's comment. This function is reachable
        // from HotkeyPoll's own thread (via ToggleStatusPanel), which must
        // never touch g_anchorHwnd directly.
    }

    inline void ToggleStatusPanel() {
        if (g_statusPanelOpen) CloseStatusPanel();
        else g_statusPanelOpen = true;
    }

    inline std::string BuildDisplayLabel(const ModRow& row, float availWidth) {
        if (row.activeLabels.empty()) return row.modName;
        std::string joined;
        for (size_t i = 0; i < row.activeLabels.size(); ++i) { if (i) joined += ", "; joined += row.activeLabels[i]; }
        std::string full = row.modName + " (" + joined + ")";
        if (ImGui::CalcTextSize(full.c_str()).x <= availWidth) return full;
        return row.modName + " (" + std::to_string(row.activeLabels.size()) + " active)";
    }

    inline std::string PoolInfoText() {
        int method = 0; long long size = 0;
        if (!SharedDataReader::TryReadPoolInfo(method, size))
            return "Trampoline: not needed (patch-only)";
        char out[96];
        switch (method) {
        case 1: snprintf(out, sizeof(out), "Trampoline: %lld B (PE cave)", size); break;
        case 2: snprintf(out, sizeof(out), "Trampoline: %lld B (VirtualAlloc)", size); break;
        case 3: snprintf(out, sizeof(out), "Trampoline: freed (idle)"); break;
        default: snprintf(out, sizeof(out), "Trampoline: not needed (patch-only)"); break;
        }
        return out;
    }

    inline void DrawStatusPanel(const std::vector<ModRow>& rows) {
        ForceCursorUsable(); // re-assert every frame - see this state's own comment above

        const float WIDTH = 280, ROW_H = 26, PAD = 10, HEADER_H = 30;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 22, vp->WorkPos.y + 56), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(WIDTH, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Colors::BG);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TEXT);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin("##ModsStatusPanel", nullptr, flags)) {

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Escape))
                CloseStatusPanel();

            ImVec2 winPos = ImGui::GetWindowPos();
            ImDrawList* winDl = ImGui::GetWindowDrawList();
            winDl->AddRectFilled(winPos, ImVec2(winPos.x + WIDTH, winPos.y + 3), Colors::ACCENT);
            winDl->AddText(ImVec2(winPos.x + PAD, winPos.y + 4 + (HEADER_H - 4 - ImGui::GetTextLineHeight()) / 2),
                Colors::ACCENT, "MODS - click to configure");
            ImGui::SetCursorPos(ImVec2(PAD, HEADER_H));

            if (rows.empty()) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::DISABLED_TEXT), "No mods injected");
            }
            else {
                for (const auto& row : rows) {
                    ImGui::SetCursorPosX(PAD);
                    ImGui::PushID(row.modName.c_str());
                    ImVec2 rowStart = ImGui::GetCursorScreenPos();
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    bool hovered = ImGui::IsMouseHoveringRect(rowStart, ImVec2(rowStart.x + WIDTH - PAD, rowStart.y + ROW_H));
                    if (row.clickable && hovered)
                        dl->AddRectFilled(rowStart, ImVec2(rowStart.x + WIDTH - PAD, rowStart.y + ROW_H), Colors::ROW_HOVER);

                    ImU32 dotColor = (row.hooked && row.hasActiveFlags) ? Colors::DOT_HOOKED_CFG
                        : row.hooked ? Colors::DOT_HOOKED
                        : row.hasActiveFlags ? Colors::DOT_OFF_CFG
                        : Colors::DOT_OFF;
                    ImVec2 dotCenter(rowStart.x + 8, rowStart.y + ROW_H / 2);
                    if (row.hooked || row.hasActiveFlags) dl->AddCircleFilled(dotCenter, 3.5f, dotColor);
                    else dl->AddCircle(dotCenter, 3.5f, dotColor);

                    std::string label = BuildDisplayLabel(row, WIDTH - PAD * 2 - 16);
                    ImU32 textColor = row.clickable ? Colors::TEXT : Colors::DISABLED_TEXT;
                    dl->AddText(ImVec2(rowStart.x + 20, rowStart.y + (ROW_H - ImGui::GetTextLineHeight()) / 2), textColor, label.c_str());

                    ImGui::InvisibleButton("row", ImVec2(WIDTH - PAD * 2, ROW_H));
                    if (row.clickable && ImGui::IsItemClicked()) {
                        // ClickButtonForOverlay, NOT plain ClickButton - see
                        // ModKit_ClickButtonForOverlay's own comment in
                        // ModKit.h: this marks the resulting Open() call (if
                        // any) as an in-game overlay click specifically, so
                        // it's routed correctly regardless of what else is
                        // happening (e.g. SteamSwitcher's own desktop
                        // "Config" button, which still uses plain
                        // ClickButton via CmdPipeThread and always means
                        // native, even if this in-game panel also happens
                        // to be open at the same time).
                        ModKitInterop::ClickButtonForOverlay(row.modName.c_str());
                        TrackConfigWindowFor(row.modName);
                        // Phase 2: draw this mod's config window ourselves if
                        // it registered with the bridge (see DrawConfigPanel
                        // below) - a legacy/unrebuilt mod that opened a
                        // native window instead just leaves HasConfigWindow
                        // false, so DrawConfigPanel's own check no-ops and
                        // TrackConfigWindowFor above stays the operative
                        // path for it, unchanged.
                        g_openConfigModName = row.modName;
                    }
                    ImGui::PopID();
                }
            }

            ImGui::SetCursorPosX(PAD);
            bool searching = ModKitInterop::IsPoolSearching();
            bool clearing = ModKitInterop::IsPoolClearing();
            if (searching || clearing) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::POOL_SEARCH_TXT), "%s",
                    searching ? "searching for trampoline..." : "clearing trampoline...");
            }
            else {
                std::string info = PoolInfoText();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::POOL_INFO_TXT), "%s", info.c_str());
            }
            ImGui::Dummy(ImVec2(0, 4));

            ImVec2 finalPos = ImGui::GetWindowPos();
            ImVec2 finalSize = ImGui::GetWindowSize();
            UpdateAnchorWindow((int)finalPos.x, (int)finalPos.y, (int)finalSize.x, (int)finalSize.y);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    // ── Config window panel (phase 2) ────────────────────────────────────────
    // Renders whichever mod's config window most recently registered with
    // the bridge (see ModKit.h's CONFIG WINDOW OVERLAY BRIDGE), one at a
    // time - matches ModConfigWindow's own singleton-per-mod behavior, so
    // there's never more than one to show anyway. Positioned to the right
    // of the status panel, the same spot the native version's own
    // FindWindowA anchor logic already places it - drawn in the SAME ImGui
    // frame here instead, so no anchor-window trick is needed for this
    // panel specifically (only the status panel above still needs one, for
    // legacy/unrebuilt mods' native config windows to anchor against).
    inline void DrawConfigPanel() {
        if (g_openConfigModName.empty()) return;
        if (!ModKitInterop::HasConfigWindow(g_openConfigModName.c_str())) {
            // Closed elsewhere (X/Escape below, or a notification-mode
            // switch via ModKit_CloseAllConfigWindows) - stop drawing.
            g_openConfigModName.clear();
            return;
        }

        char title[128] = {};
        ModKitInterop::GetConfigWindowTitle(g_openConfigModName.c_str(), title, sizeof(title));
        int rowCount = ModKitInterop::GetConfigWindowRowCount(g_openConfigModName.c_str());

        const float STATUS_WIDTH = 280; // must match DrawStatusPanel's own WIDTH above
        const float WIDTH = 280, PAD = 10, HEADER_H = 30;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 22 + STATUS_WIDTH + 8, vp->WorkPos.y + 56), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(WIDTH, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Colors::BG);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TEXT);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        std::string winId = "##ConfigWindow_" + g_openConfigModName;
        if (ImGui::Begin(winId.c_str(), nullptr, flags)) {
            bool closeRequested = false;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Escape))
                closeRequested = true;

            ImVec2 winPos = ImGui::GetWindowPos();
            ImDrawList* winDl = ImGui::GetWindowDrawList();
            winDl->AddRectFilled(winPos, ImVec2(winPos.x + WIDTH, winPos.y + 3), Colors::ACCENT);
            winDl->AddText(ImVec2(winPos.x + PAD, winPos.y + 4 + (HEADER_H - 4 - ImGui::GetTextLineHeight()) / 2),
                Colors::ACCENT, title[0] ? title : g_openConfigModName.c_str());

            // Close ("x") button, top-right of the header.
            {
                float xW = ImGui::CalcTextSize("x").x + 10;
                ImVec2 xPos(winPos.x + WIDTH - xW, winPos.y + 4);
                ImGui::SetCursorScreenPos(xPos);
                ImGui::PushID("closeConfigBtn");
                if (ImGui::InvisibleButton("close", ImVec2(xW, HEADER_H - 8)))
                    closeRequested = true;
                bool xHovered = ImGui::IsItemHovered();
                winDl->AddText(xPos, xHovered ? Colors::TEXT : Colors::DISABLED_TEXT, "x");
                ImGui::PopID();
            }

            if (closeRequested) {
                ModKitInterop::CloseConfigWindowFromOverlay(g_openConfigModName.c_str());
                g_openConfigModName.clear();
            }
            else {
                ImGui::SetCursorPos(ImVec2(PAD, HEADER_H + 6));

                if (rowCount == 0) {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::DISABLED_TEXT), "No settings");
                }

                float contentW = WIDTH - PAD * 2;
                for (int i = 0; i < rowCount; ++i) {
                    ModKitInterop::ModKitConfigRowView row = {};
                    if (!ModKitInterop::GetConfigWindowRow(g_openConfigModName.c_str(), i, &row)) continue;

                    ImGui::SetCursorPosX(PAD);
                    ImGui::PushID(i);

                    switch (row.type) {
                    case ModKitInterop::MODKIT_ROW_TOGGLE: {
                        bool on = row.valueText[0] == '1';
                        ImGui::TextUnformatted(row.label);
                        ImGui::SameLine(contentW - 30);
                        if (ImGui::Checkbox("##t", &on))
                            ModKitInterop::SetConfigWindowToggle(g_openConfigModName.c_str(), i);
                        break;
                    }
                    case ModKitInterop::MODKIT_ROW_FLOAT: {
                        // Re-read the authoritative value fresh every frame
                        // rather than keeping our own edit buffer - this
                        // only actually changes right after a successful
                        // Apply (Enter), never mid-keystroke, so it doesn't
                        // fight ImGui's own internal active-edit text state
                        // the way a value that changed every frame would.
                        float val = (float)atof(row.valueText);
                        ImGui::TextUnformatted(row.label);
                        ImGui::SetNextItemWidth(contentW);
                        if (ImGui::InputFloat("##f", &val, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
                            ModKitInterop::SetConfigWindowFloat(g_openConfigModName.c_str(), i, val);
                        break;
                    }
                    case ModKitInterop::MODKIT_ROW_INT: {
                        int val = atoi(row.valueText);
                        ImGui::TextUnformatted(row.label);
                        ImGui::SetNextItemWidth(contentW);
                        if (ImGui::InputInt("##i", &val, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
                            ModKitInterop::SetConfigWindowInt(g_openConfigModName.c_str(), i, val);
                        break;
                    }
                    case ModKitInterop::MODKIT_ROW_DROPDOWN: {
                        ImGui::TextUnformatted(row.label);
                        ImGui::SetNextItemWidth(contentW);
                        if (ImGui::BeginCombo("##d", row.valueText)) {
                            for (int oi = 0; oi < row.dropdownCount; ++oi) {
                                char opt[64] = {};
                                if (!ModKitInterop::GetConfigWindowDropdownOption(g_openConfigModName.c_str(), i, oi, opt, sizeof(opt)))
                                    continue;
                                bool selected = strcmp(opt, row.valueText) == 0;
                                if (ImGui::Selectable(opt, selected))
                                    ModKitInterop::SetConfigWindowDropdown(g_openConfigModName.c_str(), i, oi);
                            }
                            ImGui::EndCombo();
                        }
                        break;
                    }
                    case ModKitInterop::MODKIT_ROW_STATUS:
                    default:
                        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::DISABLED_TEXT), "%s", row.valueText);
                        break;
                    }

                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0, 4));
                }
                ImGui::Dummy(ImVec2(0, 4));
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    // Was a magenta "OVERLAY RENDERING OK" marker, unconditional every
    // frame — confirmed rendering works (RE3, repeatedly), removed from
    // the call below. Left defined here, unused, in case a future
    // regression needs the same quick "is anything drawing at all" check
    // again — just re-add the call in DrawOverlay() below if so.
    inline void DrawDiagnosticMarker() {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 pos(vp->WorkPos.x + 10, vp->WorkPos.y + 10);
        dl->AddRectFilled(pos, ImVec2(pos.x + 200, pos.y + 30), IM_COL32(255, 0, 255, 255));
        dl->AddText(ImVec2(pos.x + 6, pos.y + 6), IM_COL32(0, 0, 0, 255), "OVERLAY RENDERING OK");
    }

    // ── Entry point — called once per frame from PresentHookKit's hook.
    // Gated by g_channel2Enabled (SteamSwitcher's SETMODCHANNEL pipe
    // message) instead of the archive's "overlay.mode" shared-data key. ────
    inline void DrawOverlay() {
        // Teardown handshake - see RequestAnchorDestroyAndWait()'s comment.
        // Takes priority over everything else this frame: we're mid-unload.
        if (g_anchorDestroyRequested) {
            DestroyAnchorWindow();
            g_anchorDestroyRequested = false;
            if (g_anchorDestroyedEvent) SetEvent(g_anchorDestroyedEvent);
            return;
        }

        if (!g_channel2Enabled && g_statusPanelOpen) CloseStatusPanel();

        // Apply the deferred cursor-draw-mode flag here - the one place
        // it's ever safe to touch ImGui's io struct (render thread, between
        // NewFrame() and Render()) - see g_wantMouseDrawCursor's own comment.
        ImGui::GetIO().MouseDrawCursor = g_wantMouseDrawCursor;

        // Render-thread-only anchor cleanup - see g_anchorTitleSet's comment.
        // Runs regardless of g_channel2Enabled so a channel-2-disable close
        // (the line above) still gets its anchor cleared.
        if (!g_statusPanelOpen && g_anchorTitleSet) {
            ClearAnchorWindow();
            g_anchorTitleSet = false;
        }

        std::vector<ModRow> rows;
        if (g_channel2Enabled) rows = ReadModRows();

        if (g_channel2Enabled) DiffAndPushToasts(rows);
        DrawToastStack(); // safe even if g_channel2Enabled just went false — Prune() drains the stack

        if (g_channel2Enabled && g_statusPanelOpen) DrawStatusPanel(rows);
        // Gated the same as the status panel now - a config window hides
        // (stops drawing) when INSERT closes and reappears in the same
        // spot when it reopens, rather than staying independently visible.
        // Nothing about its underlying state (rows, values, which mod)
        // changes while hidden - see g_openConfigModName's own comment.
        if (g_channel2Enabled && g_statusPanelOpen) DrawConfigPanel();
    }

} // namespace Overlay