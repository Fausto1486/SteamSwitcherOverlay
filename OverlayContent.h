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
#include "OverlayCommandPipe.h"
#include "imgui.h"
#include <Windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <regex>
#include <array>

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

        // Session info block (game name/profile/timer/Logs button) - mirrors
        // ModsStatusPanel.cs's own palette exactly (INFO_ACCENT, INFO_DIM,
        // LOGS_BTN_*, DIM_BACKDROP) so Toast and Overlay modes read as the
        // same feature, just rendered in two different toolkits.
        static const ImU32 INFO_ACCENT = Col(220, 220, 230);
        static const ImU32 INFO_DIM = Col(150, 150, 160);
        static const ImU32 LOGS_BTN_BG = Col(34, 34, 42);
        static const ImU32 LOGS_BTN_BG_HOVER = Col(50, 50, 62);
        static const ImU32 LOGS_BTN_BORDER = Col(90, 90, 105);
        static const ImU32 LOGS_BTN_TEXT = Col(200, 210, 230);
        static const ImU32 DIM_BACKDROP = Col(0, 0, 0, 140);
    }

    // ── Channel 2 (mod content) gate — set by OverlayPipe's SETMODCHANNEL.
    // Replaces the archive's "overlay.mode" shared-data key check. ───────────
    inline bool g_channel2Enabled = false;
    inline void SetChannel2Enabled(bool enabled) { g_channel2Enabled = enabled; }

    // ── Session info — game name/profile/launch time for the status panel's
    // header, set by OverlayPipe's GAMEINFO (SteamSwitcher-side pipe
    // thread), read every frame by DrawStatusPanel (render thread). Same
    // CRITICAL_SECTION + Snapshot pattern as ToastState below, for the same
    // reason: never hold the lock while calling into ImGui. Mirrors
    // ModsStatusPanel.cs's own _gameName/_profileName/_launchTimeUtc fields
    // set via SetSessionInfo — same three values, same meaning, just
    // arriving over a pipe here instead of a direct in-process call.
    struct SessionInfo {
        std::string gameName;
        std::string profileName;
        int64_t launchEpochMs = 0;   // 0 = unknown, see OverlayPipe.h's GAMEINFO doc
        CRITICAL_SECTION lock;

        SessionInfo() { InitializeCriticalSection(&lock); }
        ~SessionInfo() { DeleteCriticalSection(&lock); }

        void Set(const std::string& name, int64_t epochMs, const std::string& profile) {
            EnterCriticalSection(&lock);
            gameName = name;
            launchEpochMs = epochMs;
            profileName = profile;
            LeaveCriticalSection(&lock);
        }

        struct Snapshot { std::string gameName, profileName; int64_t launchEpochMs; };
        Snapshot Get() {
            EnterCriticalSection(&lock);
            Snapshot s{ gameName, profileName, launchEpochMs };
            LeaveCriticalSection(&lock);
            return s;
        }
    };
    inline SessionInfo g_session;

    inline bool HasInfoBlock(const SessionInfo::Snapshot& s) { return !s.gameName.empty(); }

    // Last width DrawStatusPanel actually rendered at, so DrawConfigPanel
    // (positioned immediately to its right) can stay correctly aligned now
    // that the status panel's width is dynamic (see DrawStatusPanel's own
    // WIDTH computation) rather than the old fixed 280. Render-thread-only,
    // same as everything else these two functions share - no locking needed.
    inline float g_lastStatusPanelWidth = 280.0f;

    // ── Draggable sub-panels ─────────────────────────────────────────────────
    // DrawConfigPanel/DrawStatsPanel draw their own header (no real ImGui
    // title bar - ImGuiWindowFlags_NoTitleBar|NoMove, same as the status
    // panel above them), so unlike Toast mode's equivalents (real win32
    // windows with a real title bar - dragging is just the OS doing its
    // normal thing there, no code needed) these need their own manual drag
    // handling to match. One of these per draggable panel.
    struct PanelDragState {
        ImVec2 pos{};       // current top-left, screen coords
        bool userMoved = false;   // true once dragged at least once since last (re)open - freezes auto-docking
        bool dragging = false;    // mouse currently held down, drag in progress
    };
    inline PanelDragState g_configPanelDrag;
    inline PanelDragState g_statsPanelDrag;

    // Called once per frame, right after Begin() (needs the window's actual
    // screen rect, which isn't known before then). headerMin/Max is the
    // draggable strip's screen rect; closeButtonHovered excludes starting a
    // drag from a click that's actually meant for the close button sharing
    // that same strip. Mutates st.pos for the NEXT frame's SetNextWindowPos
    // - one frame of lag between mouse movement and the window actually
    // moving, imperceptible at real framerates, standard for a manual drag
    // implementation in an immediate-mode GUI.
    inline void UpdatePanelDrag(PanelDragState& st, const ImVec2& headerMin, const ImVec2& headerMax, bool closeButtonHovered) {
        ImGuiIO& io = ImGui::GetIO();
        if (st.dragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.pos.x += io.MouseDelta.x;
                st.pos.y += io.MouseDelta.y;
            }
            else {
                st.dragging = false;
            }
        }
        else if (!closeButtonHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsMouseHoveringRect(headerMin, headerMax)) {
            st.dragging = true;
            st.userMoved = true;
        }
    }

    inline std::string FormatElapsed(int64_t launchEpochMs) {
        if (launchEpochMs <= 0) return "";
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t elapsedSec = (nowMs - launchEpochMs) / 1000;
        if (elapsedSec < 0) elapsedSec = 0;
        int64_t h = elapsedSec / 3600, m = (elapsedSec % 3600) / 60, s = elapsedSec % 60;
        char buf[32];
        if (h > 0) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
        else snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)m, (long long)s);
        return buf;
    }

    // Strips a leading "Run <anything> on " prefix from the profile name
    // before display, if present - see ModsStatusPanel.cs's own
    // DisplayProfileText for the full reasoning (the game name is already
    // shown as this panel's own header, so a profile/persona name that
    // itself happens to read like "Run <game> on <name>" would otherwise
    // repeat it). Same greedy-middle regex behavior: splits at the LAST
    // " on ", not the first, so a genuine name containing " on " is safe.
    inline std::string DisplayProfileText(const std::string& profileName) {
        static const std::regex kRunOnPrefix(R"(^Run\s+.+\son\s+(.+)$)");
        std::smatch m;
        if (std::regex_match(profileName, m, kRunOnPrefix)) return m[1].str();
        return profileName;
    }

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
    // window instead - see TrackConfigWindowFor for that path). Also set by
    // DrawOverlay's own auto-discovery fallback for a window opened by
    // something other than a click (a mod's own hotkey). Independent of
    // g_statusPanelOpen (the in-game INSERT panel's own visibility) - only
    // actually clears on an explicit close (X / Escape) or a
    // notification-mode switch (ModKit_CloseAllConfigWindows), same as the
    // underlying ModKit.dll registration.
    inline std::string g_openConfigModName;

    // Stats-window counterpart of g_openConfigModName above — set at the
    // same row-click site (only whichever bridge the clicked mod actually
    // registered with ends up non-empty in HasConfigWindow/HasStatsWindow,
    // so both trackers safely coexist per click), or by DrawOverlay's own
    // auto-discovery fallback for a window opened some other way (a mod's
    // own hotkey). See g_openConfigModName's own comment for clearing.
    inline std::string g_openStatsModName;

    // Per-row edit buffers for DrawStatsPanel's MODKIT_STATS_EDIT rows,
    // keyed by "modName#rowIndex" — ImGui::InputText needs a stable buffer
    // across frames while the user is typing, so this is only refreshed
    // from the live valueText when the widget isn't currently active
    // (mirrors StatsWindowKit's own focus-based RefreshEditText guard on
    // the native side).
    inline std::unordered_map<std::string, std::array<char, 64>> g_statsEditBuffers;

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
        // NOT an unconditional RestoreCursor() here anymore - a config or
        // stats window can still be independently open (see DrawOverlay's
        // own end-of-frame cursor arbitration), and releasing cursor
        // control out from under one of those while it's still visible
        // would reproduce the exact flicker this whole mechanism exists to
        // avoid. DrawOverlay's own check after every Draw*Panel call is the
        // single place that decides "nothing is open anymore, actually
        // let go" now.
        if (g_openConfigModName.empty() && g_openStatsModName.empty())
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

        const float WIDTH_BASE = 280, MAX_WIDTH = 460, ROW_H = 26, PAD = 10, HEADER_H = 30;
        const float INFO_H = 44, INFO_LINE_H = 20, LOGS_BTN_W = 52, LOGS_BTN_H = 18;

        // Dynamic width, mirroring ModsStatusPanel.cs's ComputeRequiredWidth:
        // widen past WIDTH_BASE (up to MAX_WIDTH) when the game name or
        // profile/timer line needs more room, so text doesn't overlap or
        // ellipsize unnecessarily on a panel that had space to just be
        // wider. DisplayProfileText/FormatElapsed are the exact same
        // transforms the Toast-mode panel applies, so both modes show
        // identical text for identical session data.
        auto sess = g_session.Get();
        bool hasInfo = HasInfoBlock(sess);
        std::string profileText = hasInfo ? DisplayProfileText(sess.profileName) : "";
        std::string timeText = hasInfo ? FormatElapsed(sess.launchEpochMs) : "";
        float WIDTH = WIDTH_BASE;
        if (hasInfo) {
            float line1 = PAD + ImGui::CalcTextSize(sess.gameName.c_str()).x + 8 + LOGS_BTN_W + PAD;
            WIDTH = (std::max)(WIDTH, line1);
            if (!profileText.empty() || !timeText.empty()) {
                float line2 = PAD + ImGui::CalcTextSize(profileText.c_str()).x + 8
                    + ImGui::CalcTextSize(timeText.c_str()).x + PAD;
                WIDTH = (std::max)(WIDTH, line2);
            }
            WIDTH = (std::min)(WIDTH, MAX_WIDTH);
        }
        float infoTop = hasInfo ? INFO_H : 0.0f;
        g_lastStatusPanelWidth = WIDTH;

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

            // Session info block - game name + Logs button (line 1), profile
            // name + elapsed time (line 2). Mirrors ModsStatusPanel.cs's own
            // INFO_H block layout/colors exactly - see Colors namespace.
            if (hasInfo) {
                winDl->AddText(ImVec2(winPos.x + PAD, winPos.y + 2 + (INFO_LINE_H - ImGui::GetTextLineHeight()) / 2),
                    Colors::INFO_ACCENT, sess.gameName.c_str());

                ImVec2 btnMin(winPos.x + WIDTH - PAD - LOGS_BTN_W, winPos.y + 2 + (INFO_LINE_H - LOGS_BTN_H) / 2);
                ImVec2 btnMax(btnMin.x + LOGS_BTN_W, btnMin.y + LOGS_BTN_H);
                bool logsHovered = ImGui::IsMouseHoveringRect(btnMin, btnMax);
                winDl->AddRectFilled(btnMin, btnMax, logsHovered ? Colors::LOGS_BTN_BG_HOVER : Colors::LOGS_BTN_BG);
                winDl->AddRect(btnMin, btnMax, Colors::LOGS_BTN_BORDER);
                ImVec2 logsTextSize = ImGui::CalcTextSize("Logs");
                winDl->AddText(ImVec2(btnMin.x + (LOGS_BTN_W - logsTextSize.x) / 2, btnMin.y + (LOGS_BTN_H - logsTextSize.y) / 2),
                    Colors::LOGS_BTN_TEXT, "Logs");
                ImGui::SetCursorScreenPos(btnMin);
                ImGui::InvisibleButton("##logsBtn", ImVec2(LOGS_BTN_W, LOGS_BTN_H));
                if (ImGui::IsItemClicked()) OverlayCommandPipe::SendShowLogs();

                if (!profileText.empty() || !timeText.empty()) {
                    float line2Y = winPos.y + INFO_LINE_H + 2 + (INFO_LINE_H - ImGui::GetTextLineHeight()) / 2;
                    if (!profileText.empty())
                        winDl->AddText(ImVec2(winPos.x + PAD, line2Y), Colors::INFO_DIM, profileText.c_str());
                    if (!timeText.empty()) {
                        float tw = ImGui::CalcTextSize(timeText.c_str()).x;
                        winDl->AddText(ImVec2(winPos.x + WIDTH - PAD - tw, line2Y), Colors::INFO_DIM, timeText.c_str());
                    }
                }
            }

            winDl->AddRectFilled(ImVec2(winPos.x, winPos.y + infoTop), ImVec2(winPos.x + WIDTH, winPos.y + infoTop + 3), Colors::ACCENT);
            winDl->AddText(ImVec2(winPos.x + PAD, winPos.y + infoTop + 4 + (HEADER_H - 4 - ImGui::GetTextLineHeight()) / 2),
                Colors::ACCENT, "MODS - click to configure");
            ImGui::SetCursorPos(ImVec2(PAD, infoTop + HEADER_H));

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
                        g_openStatsModName = row.modName;
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
            g_configPanelDrag.userMoved = false;   // next open re-docks fresh
            return;
        }
        // Only from here on are we committed to actually drawing this frame
        // - see ForceCursorUsable's own comment for why DrawStatusPanel
        // gets to call this unconditionally at its own top and this one
        // doesn't (this function, unlike that one, is no longer gated by
        // the caller on whether there's actually anything to show).
        ForceCursorUsable();

        char title[128] = {};
        ModKitInterop::GetConfigWindowTitle(g_openConfigModName.c_str(), title, sizeof(title));
        int rowCount = ModKitInterop::GetConfigWindowRowCount(g_openConfigModName.c_str());

        const float STATUS_WIDTH = g_lastStatusPanelWidth; // stays aligned with DrawStatusPanel's own dynamic WIDTH
        const float WIDTH = 280, PAD = 10, HEADER_H = 30;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        // Auto-docked next to the status panel every frame until the user
        // drags it away (UpdatePanelDrag, called below once the header's
        // screen rect is known) - once that happens this stops overwriting
        // g_configPanelDrag.pos, so it stays wherever they left it until
        // the panel is closed and reopened fresh.
        if (!g_configPanelDrag.userMoved)
            g_configPanelDrag.pos = ImVec2(vp->WorkPos.x + 22 + STATUS_WIDTH + 8, vp->WorkPos.y + 56);
        ImGui::SetNextWindowPos(g_configPanelDrag.pos, ImGuiCond_Always);
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
            bool xHovered = false;
            {
                float xW = ImGui::CalcTextSize("x").x + 10;
                ImVec2 xPos(winPos.x + WIDTH - xW, winPos.y + 4);
                ImGui::SetCursorScreenPos(xPos);
                ImGui::PushID("closeConfigBtn");
                if (ImGui::InvisibleButton("close", ImVec2(xW, HEADER_H - 8)))
                    closeRequested = true;
                xHovered = ImGui::IsItemHovered();
                winDl->AddText(xPos, xHovered ? Colors::TEXT : Colors::DISABLED_TEXT, "x");
                ImGui::PopID();
            }

            // Drag the whole colored header strip (minus the close button,
            // excluded above via xHovered) to move the panel - see
            // UpdatePanelDrag's own comment.
            UpdatePanelDrag(g_configPanelDrag, winPos, ImVec2(winPos.x + WIDTH, winPos.y + HEADER_H), xHovered);

            if (closeRequested) {
                ModKitInterop::CloseConfigWindowFromOverlay(g_openConfigModName.c_str());
                g_openConfigModName.clear();
                g_configPanelDrag.userMoved = false;   // next open re-docks fresh, matching a freshly-created native window
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

    // ── Stats window panel ───────────────────────────────────────────────────
    // StatsWindowKit counterpart of DrawConfigPanel above — same position,
    // same singleton-per-mod shape, same bridge pattern (see ModKit.h's
    // "STATS WINDOW OVERLAY BRIDGE"). Skips drawing if the clicked mod's
    // config-window bridge is the one that actually registered instead (see
    // g_openStatsModName's own comment) — the two never legitimately overlap
    // in this codebase, but this keeps that assumption from silently
    // double-drawing if it ever changes.
    inline void DrawStatsPanel() {
        if (g_openStatsModName.empty()) return;
        if (ModKitInterop::HasConfigWindow(g_openStatsModName.c_str())) return;
        if (!ModKitInterop::HasStatsWindow(g_openStatsModName.c_str())) {
            g_openStatsModName.clear();
            g_statsPanelDrag.userMoved = false;   // next open re-docks fresh
            return;
        }
        // See DrawConfigPanel's identical call for why this has to happen
        // here rather than unconditionally at the top of this function.
        ForceCursorUsable();

        char title[128] = {};
        ModKitInterop::GetStatsWindowTitle(g_openStatsModName.c_str(), title, sizeof(title));
        int rowCount = ModKitInterop::GetStatsWindowRowCount(g_openStatsModName.c_str());

        // Fetched upfront (not lazily per-row like before) so a
        // MODKIT_STATS_COLUMN_BREAK anywhere in the list can decide the
        // window's WIDTH before Begin() - ImGui needs the size up front,
        // not something discoverable mid-draw.
        std::vector<ModKitInterop::ModKitStatsRowView> rows(rowCount);
        bool hasColumnBreak = false;
        for (int i = 0; i < rowCount; ++i) {
            ModKitInterop::GetStatsWindowRow(g_openStatsModName.c_str(), i, &rows[i]);
            if (rows[i].type == ModKitInterop::MODKIT_STATS_COLUMN_BREAK) hasColumnBreak = true;
        }

        const float STATUS_WIDTH = g_lastStatusPanelWidth;
        const float PAD = 10, HEADER_H = 30, GUTTER = 16, COL_W = 300;
        // A mod that emits a column break wants a wide-but-short panel
        // instead of the default narrow-but-tall one - see this row type's
        // own comment in ModKit.h. Everything else about the panel (anchor
        // position, header, close button) stays identical either way.
        const float WIDTH = hasColumnBreak ? (PAD * 2 + COL_W * 2 + GUTTER) : 320;
        const float contentW = hasColumnBreak ? COL_W : (WIDTH - PAD * 2);
        const float col2X = PAD + COL_W + GUTTER;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        // See DrawConfigPanel's identical block for the auto-dock-until-
        // dragged reasoning.
        if (!g_statsPanelDrag.userMoved)
            g_statsPanelDrag.pos = ImVec2(vp->WorkPos.x + 22 + STATUS_WIDTH + 8, vp->WorkPos.y + 56);
        ImGui::SetNextWindowPos(g_statsPanelDrag.pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(WIDTH, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Colors::BG);
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TEXT);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        std::string winId = "##StatsWindow_" + g_openStatsModName;
        if (ImGui::Begin(winId.c_str(), nullptr, flags)) {
            bool closeRequested = false;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Escape))
                closeRequested = true;

            ImVec2 winPos = ImGui::GetWindowPos();
            ImDrawList* winDl = ImGui::GetWindowDrawList();
            winDl->AddRectFilled(winPos, ImVec2(winPos.x + WIDTH, winPos.y + 3), Colors::ACCENT);
            winDl->AddText(ImVec2(winPos.x + PAD, winPos.y + 4 + (HEADER_H - 4 - ImGui::GetTextLineHeight()) / 2),
                Colors::ACCENT, title[0] ? title : g_openStatsModName.c_str());

            bool xHovered = false;
            {
                float xW = ImGui::CalcTextSize("x").x + 10;
                ImVec2 xPos(winPos.x + WIDTH - xW, winPos.y + 4);
                ImGui::SetCursorScreenPos(xPos);
                ImGui::PushID("closeStatsBtn");
                if (ImGui::InvisibleButton("close", ImVec2(xW, HEADER_H - 8)))
                    closeRequested = true;
                xHovered = ImGui::IsItemHovered();
                winDl->AddText(xPos, xHovered ? Colors::TEXT : Colors::DISABLED_TEXT, "x");
                ImGui::PopID();
            }

            // See DrawConfigPanel's identical call for the drag mechanics.
            UpdatePanelDrag(g_statsPanelDrag, winPos, ImVec2(winPos.x + WIDTH, winPos.y + HEADER_H), xHovered);

            if (closeRequested) {
                ModKitInterop::CloseStatsWindowFromOverlay(g_openStatsModName.c_str());
                g_openStatsModName.clear();
                g_statsPanelDrag.userMoved = false;   // next open re-docks fresh
            }
            else {
                const float contentStartY = HEADER_H + 6;
                ImGui::SetCursorPos(ImVec2(PAD, contentStartY));

                if (rowCount == 0) {
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::DISABLED_TEXT), "No data");
                }

                // Tracks the taller of the two columns so the window's
                // auto-fit height (SetNextWindowSize's height=0 above)
                // covers whichever one actually ends up longer, regardless
                // of draw order - ImGui's own auto-fit follows the cursor,
                // and the loop below jumps the cursor back to the top of
                // column 2 on a break, so without this the auto-fit would
                // only ever reflect column 2's height.
                float maxYReached = contentStartY;
                bool inColumn2 = false;

                for (int i = 0; i < rowCount; ++i) {
                    const ModKitInterop::ModKitStatsRowView& row = rows[i];

                    if (row.type == ModKitInterop::MODKIT_STATS_COLUMN_BREAK) {
                        maxYReached = (std::max)(maxYReached, ImGui::GetCursorPosY());
                        inColumn2 = true;
                        ImGui::SetCursorPos(ImVec2(col2X, contentStartY));
                        continue;
                    }

                    const float colX = inColumn2 ? col2X : PAD;
                    ImGui::SetCursorPosX(colX);
                    ImGui::PushID(i);

                    switch (row.type) {
                    case ModKitInterop::MODKIT_STATS_DIVIDER:
                        ImGui::Dummy(ImVec2(0, 2));
                        ImGui::SeparatorText(row.label);
                        break;
                    case ModKitInterop::MODKIT_STATS_BAR: {
                        ImGui::TextUnformatted(row.label);
                        ImGui::SameLine(colX + contentW - 110);
                        ImGui::TextColored(row.valid
                            ? ImGui::ColorConvertU32ToFloat4(Colors::TEXT)
                            : ImGui::ColorConvertU32ToFloat4(Colors::DISABLED_TEXT), "%s", row.valueText);
                        ImVec4 barCol = ImGui::ColorConvertU32ToFloat4(
                            IM_COL32((row.barColor) & 0xFF, (row.barColor >> 8) & 0xFF, (row.barColor >> 16) & 0xFF, 255));
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
                        ImGui::SetCursorPosX(colX);
                        ImGui::SetNextItemWidth(contentW);
                        ImGui::ProgressBar(row.valid ? row.barFrac : 0.0f, ImVec2(contentW, 10), "");
                        ImGui::PopStyleColor();
                        break;
                    }
                    case ModKitInterop::MODKIT_STATS_TEXT:
                        ImGui::TextUnformatted(row.label);
                        ImGui::SameLine(colX + contentW * 0.5f);
                        ImGui::TextColored(row.valid
                            ? ImGui::ColorConvertU32ToFloat4(Colors::TEXT)
                            : ImGui::ColorConvertU32ToFloat4(Colors::DISABLED_TEXT), "%s", row.valueText);
                        break;
                    case ModKitInterop::MODKIT_STATS_EDIT: {
                        ImGui::TextUnformatted(row.label);
                        ImGui::BeginDisabled(!row.enabled);
                        std::string key = g_openStatsModName + "#" + std::to_string(i);
                        auto& buf = g_statsEditBuffers[key];
                        ImGui::SetCursorPosX(colX);
                        ImGui::SetNextItemWidth(contentW);
                        bool changed = ImGui::InputText("##e", buf.data(), buf.size());
                        // Only clobber the box with the live value while the
                        // user isn't actively typing in it - same reasoning
                        // as RefreshEditText's focus/dirty guard natively.
                        if (!ImGui::IsItemActive() && !changed)
                            strncpy_s(buf.data(), buf.size(), row.valueText, _TRUNCATE);
                        if (changed)
                            ModKitInterop::ChangeStatsWindowEdit(g_openStatsModName.c_str(), i, buf.data());
                        ImGui::EndDisabled();
                        break;
                    }
                    case ModKitInterop::MODKIT_STATS_BUTTON:
                        ImGui::BeginDisabled(!row.enabled);
                        if (ImGui::Button(row.label, ImVec2(contentW, 0)))
                            ModKitInterop::ClickStatsWindowButton(g_openStatsModName.c_str(), i);
                        ImGui::EndDisabled();
                        break;
                    case ModKitInterop::MODKIT_STATS_CHECKBOX: {
                        bool checked = row.checked;
                        ImGui::BeginDisabled(!row.enabled);
                        if (ImGui::Checkbox(row.label, &checked))
                            ModKitInterop::ToggleStatsWindowCheckbox(g_openStatsModName.c_str(), i);
                        ImGui::EndDisabled();
                        break;
                    }
                    case ModKitInterop::MODKIT_STATS_DROPDOWN: {
                        ImGui::TextUnformatted(row.label);
                        ImGui::BeginDisabled(!row.enabled);
                        ImGui::SetCursorPosX(colX);
                        ImGui::SetNextItemWidth(contentW);
                        if (ImGui::BeginCombo("##d", row.valueText)) {
                            for (int oi = 0; oi < row.dropdownCount; ++oi) {
                                char opt[64] = {};
                                if (!ModKitInterop::GetStatsWindowDropdownOption(g_openStatsModName.c_str(), i, oi, opt, sizeof(opt)))
                                    continue;
                                bool selected = strcmp(opt, row.valueText) == 0;
                                if (ImGui::Selectable(opt, selected))
                                    ModKitInterop::SelectStatsWindowDropdown(g_openStatsModName.c_str(), i, oi);
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::EndDisabled();
                        break;
                    }
                    }

                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0, 4));
                }

                maxYReached = (std::max)(maxYReached, ImGui::GetCursorPosY());
                ImGui::SetCursorPosY(maxYReached);
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
    // Dimmed backdrop behind the status panel (and config panel, when open) -
    // mirrors ModsStatusPanel.cs's own DimBackdrop Form exactly in effect,
    // just via ImGui's background draw list instead of a second Win32
    // window. GetBackgroundDrawList() content always renders at the very
    // bottom of the frame regardless of call order, before any ImGui
    // window's own content - toasts (drawn as normal ImGui windows via
    // DrawToastStack) are therefore always layered above this dim
    // unconditionally, by construction, not because of where this function
    // happens to be called from in DrawOverlay below.
    inline void DrawDarkBackdrop() {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), Colors::DIM_BACKDROP);
    }

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

        // Keep ModKit.dll's persistent overlay-mode flag in sync every frame
        // (not just on change) - see ModKit_IsOverlayModeActive's own
        // comment in ModKit.h for why a push-on-change-only design isn't
        // reliable here.
        ModKitInterop::SetOverlayModeActive(g_channel2Enabled);

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
        if (g_channel2Enabled) {
            rows = ReadModRows();

            // A stats/config window opened by something other than a row
            // click (e.g. a mod's own keyboard hotkey, like CharacterStats's
            // RCTRL+NUMPAD-DOT) never runs through DrawStatusPanel's click
            // handler below, which is the only other place these get set -
            // catch that here once a frame instead, so the panel still
            // shows up. Only fires while empty: a click always wins
            // immediately, this is purely a fallback for whatever a click
            // hasn't already claimed.
            if (g_openConfigModName.empty()) {
                for (auto& r : rows) {
                    if (ModKitInterop::HasConfigWindow(r.modName.c_str())) { g_openConfigModName = r.modName; break; }
                }
            }
            if (g_openStatsModName.empty()) {
                for (auto& r : rows) {
                    if (ModKitInterop::HasStatsWindow(r.modName.c_str())) { g_openStatsModName = r.modName; break; }
                }
            }
        }

        if (g_channel2Enabled) DiffAndPushToasts(rows);
        DrawToastStack(); // safe even if g_channel2Enabled just went false — Prune() drains the stack

        if (g_channel2Enabled && g_statusPanelOpen) DrawDarkBackdrop();
        if (g_channel2Enabled && g_statusPanelOpen) DrawStatusPanel(rows);
        // Independently visible regardless of the main INSERT panel's own
        // open/closed state - a mod's own hotkey (CharacterStats's
        // RCTRL+NUMPAD-DOT) can open its stats window without the main
        // panel being open at all, mirroring the native Toast-mode hotkey
        // opening a standalone win32 window with no dependency on
        // ModsStatusPanel being shown. Each panel already no-ops
        // immediately if its own g_open*ModName is empty (see the
        // auto-discovery block above and g_openConfigModName's own
        // comment), so this is safe to leave ungated beyond
        // g_channel2Enabled - no config/stats-window mod currently has a
        // hotkey of its own, but there's no reason to gate this one
        // differently from DrawStatsPanel below it now that the
        // click-only assumption behind the old gating no longer holds.
        if (g_channel2Enabled) DrawConfigPanel();
        if (g_channel2Enabled) DrawStatsPanel();

        // Counterpart to each Draw*Panel's own ForceCursorUsable() call -
        // release cursor control the moment NONE of the three overlay
        // surfaces (status/config/stats) are still visible, regardless of
        // which one(s) were open a moment ago. Safe/idempotent to call
        // every frame nothing is open - see RestoreCursor's own early
        // returns. g_open*ModName reflect this frame's actual outcome by
        // now, not last frame's - each panel already self-clears its own
        // when it stops drawing (closed via X/Escape, mode switch, or the
        // provider simply vanishing), so checking them here rather than
        // g_statusPanelOpen alone correctly covers a config/stats window
        // left open on its own after the main panel closes.
        if (!g_statusPanelOpen && g_openConfigModName.empty() && g_openStatsModName.empty())
            RestoreCursor();
    }

} // namespace Overlay