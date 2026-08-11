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
            if (!p.seen) {
                p = { row.hooked, row.outdated, row.busy, row.activeLabels, true };
                continue;
            }

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
                        ModKitInterop::ClickButton(row.modName.c_str());
                        TrackConfigWindowFor(row.modName);
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
        if (!g_channel2Enabled && g_statusPanelOpen) CloseStatusPanel();

        std::vector<ModRow> rows;
        if (g_channel2Enabled) rows = ReadModRows();

        if (g_channel2Enabled) DiffAndPushToasts(rows);
        DrawToastStack(); // safe even if g_channel2Enabled just went false — Prune() drains the stack

        if (g_channel2Enabled && g_statusPanelOpen) DrawStatusPanel(rows);
    }

} // namespace Overlay