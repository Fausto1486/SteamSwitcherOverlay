#pragma once
// ═══════════════════════════════════════════════════════════════════════
// PROJECT STATUS: WORK IN PROGRESS. This is NOT the final state of the
// overlay. Last SHIPPING configuration was DX11-ONLY (see
// OVERLAY-REDESIGN-RESULT.md's "FINAL SHIPPING DECISION" section). DX12
// testing resumed in InstallWorkerThreadProc near the bottom of this
// file. Progress since:
//   - RE3-DX12 (no Streamline): FIXED and CONFIRMED STABLE, repeatedly,
//     including real ResizeBuffers events. Original crash
//     (swapChain::Present() DXGI_ERROR_INVALID_CALL) was
//     HookedExecuteCommandLists capturing the WRONG D3D12 queue — RE3
//     uses 3 (COPY/DIRECT/COMPUTE), COPY called first and got captured,
//     but only DIRECT can execute render commands. Fixed via an
//     authoritative capture from IDXGIFactory2::CreateSwapChainForHwnd's
//     pDevice parameter (see DX11::HookedCreateSwapChainForHwnd), with
//     the old DIRECT-type-filtered ExecuteCommandLists heuristic kept as
//     a fallback only.
//   - Daemon X Machina-DX12 + Streamline: STILL BROKEN, separate cause.
//     The queue-capture fix does NOT resolve it — a confirmed-correct
//     queue is captured every time, and attach-only (heap/allocator/
//     fence/ImGui backend/ClaimHwndAndSubclass) survives 18+s reliably.
//     Real command-list submission crashes it in ~1s, reproducibly
//     (GPU Crash dump Triggered, sl_interposer in stack — an async
//     GPU/driver-side fault, not a CPU exception our SEH wrapping can
//     catch). Likely cause: sl.interposer.dll's own Present/
//     ExecuteCommandLists hooks chaining with ours — genuinely different,
//     harder problem than queue selection. The D3D12 debug layer can't
//     help diagnose it either, since by the time this DLL is
//     late-injected the game's own device already exists, undebugged.
//     Streamline gate (RequestAttach's IsStreamlineLoaded check) restored
//     to active — do NOT bypass it again without new diagnostic evidence,
//     not another guess.
// DX9 remains disabled, untested, unrelated to this push. Still open
// before DX12/RE3-class games are shipping-ready: an explicit
// fullscreen-transition retest under DX12, and mid-session overlay
// toggle (INSERT) a few times to rule out a reattach-time regression of
// the queue-capture fix.
// ═══════════════════════════════════════════════════════════════════════
//
// PresentHookKit.h — D3D9/D3D11/D3D12 hooking + real ImGui rendering for
// this DLL's in-game overlay.
//
// ARCHITECTURE NOTE — read before touching DX12 code in this file:
// DX12 does NOT get its own Present hook. Every real test tonight (3 for 3)
// showed DX11's dummy-swapchain hook claiming the shared DXGI Present
// function FIRST, before DX12::Install() even runs — on BOTH DX11 and DX12
// games (see OVERLAY-REDESIGN-RESULT.md's "MinHook hooks the shared
// function" finding). DX12's own Present-hook attempt has never once
// succeeded in testing. Rather than keep dead code around, DX11's
// HookedPresent is now the universal per-frame boundary signal for BOTH
// backends: it always attempts its own DX11 LazyInit/render first (which
// cleanly fails and no-ops on a real DX12 game — swapChain->GetDevice()
// requesting ID3D11Device simply fails when the swapchain is actually
// D3D12-backed), and falls through to DX12::TryInitAndRender() when that
// happens and a DX12 command queue has been captured. DX12's own
// Install() only ever hooks ExecuteCommandLists now (genuinely
// DX12-specific, no shared-function conflict) — it no longer attempts a
// Present hook of its own at all.
//
// DEVIATION #2 — TRIED AND REVERTED: Present/ResizeBuffers hooking was
// switched from MinHook to vtable-swap (per-array pointer overwrite) on
// the theory that MinHook's inline function-code patching was colliding
// with Steam overlay's own hook installer during RE Engine's swapchain
// recreation for exclusive fullscreen. Direct A/B test result: vtable-swap
// did NOT fix the fullscreen-transition issue, and additionally
// reintroduced the ORIGINAL 0xC00000FD stack-overflow crash inside
// gameoverlayrenderer64.dll on ordinary startup — the exact crash this
// whole redesign exists to avoid, and the reason MinHook was adopted in
// the first place (see OVERLAY-ATTEMPT-STATUS.md). MinHook never produced
// that crash across many hours of testing; vtable-swap reproduced it on
// the very first RE3 launch after switching. REVERTED back to MinHook for
// Present/ResizeBuffers. The fullscreen-transition crash (a DIFFERENT,
// narrower issue — see OVERLAY-REDESIGN-RESULT.md) remains open, on top
// of a working foundation rather than instead of one.
//
// See OVERLAY-REDESIGN-RESULT.md for the full test history.

#include <Windows.h>
#include <TlHelp32.h>
#include <cctype>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <vector>
#include <chrono>
#include "./MinHook/minhook.h"
#include "Logging.h"
#include "HotkeyPoll.h"
#include "OverlayContent.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace PresentHookKit {

    // Kept for logging/diagnostics only — does NOT gate installation. See
    // OVERLAY-REDESIGN-RESULT.md: coexistence with Steam overlay is
    // validated, not just theorized. Still narrow evidence (a handful of
    // games/GPUs/Steam versions) — re-add a skip path here if wider
    // testing finds a real counterexample.
    inline bool IsKnownConflictingOverlayLoaded() {
        return GetModuleHandleA("gameoverlayrenderer64.dll") != nullptr ||
            GetModuleHandleA("gameoverlayrenderer.dll") != nullptr;
    }

    // ── Shared state across all backends ──────────────────────────────────────
    inline volatile LONG g_attachGate = 0;
    inline bool g_imguiContextCreated = false;
    inline bool g_win32BackendActive = false; // tracks Init/Shutdown across DX9/DX11/DX12 — prevents double-shutdown when any one of them tears down

    // Real attachment (device refs, backbuffers, ImGui init) no longer
    // happens automatically on the first frame — per direct user request,
    // delayed the same way SteamSwitcher itself delays mod injection past
    // splash screens. Set true by either the hotkey or a SETMODCHANNEL|1
    // pipe message. Hooks still install early (proven safe); only the
    // actual attach work waits.
    inline volatile bool g_attachRequested = false;
    // Detected in two separate Daemon X Machina crash reports
    // (GPU Crash dump Triggered, sl_interposer in the stack) — disabling
    // DLSS in-game did NOT fix it, and the actual interaction with our
    // hook isn't understood yet (see OVERLAY-REDESIGN-RESULT.md). Refusing
    // to attach when this is loaded is a pragmatic stopgap, not a real
    // fix — revisit once the actual cause is found. sl.interposer.dll
    // stays loaded once a game links against Streamline regardless of
    // whether DLSS/Frame Generation is toggled on, so this check doesn't
    // need the feature to be actively in use to catch the risk.
    inline bool IsStreamlineLoaded() {
        return GetModuleHandleA("sl.interposer.dll") != nullptr;
    }

    // Safety switch only — flip true to instantly restore the old
    // unconditional refuse-on-Streamline behavior if the D3D12 D3D11On12
    // branch (DX12::g_usingD3D11On12, decided per-attach in
    // DX12::LazyInit — see that namespace's own header comment) turns out
    // not to fix the Daemon X Machina crash after all and needs isolating
    // again. Default false: DX12 now selects the D3D11On12 render path
    // automatically whenever Streamline is loaded, so there's no known
    // reason left to refuse attaching outright.
    inline bool g_refuseAttachOnStreamlineForTesting = false;

    inline void RequestAttach() {
        // Idempotent — g_attachRequested never resets once true (nothing
        // in this file ever sets it back to false), so every call after
        // the first is a pure no-op. Called from every INSERT press,
        // every SETMODCHANNEL enable, and every toast — without this
        // early return the Streamline log line below fired once per call
        // instead of once per session.
        if (g_attachRequested) return;
        if (IsStreamlineLoaded()) {
            if (g_refuseAttachOnStreamlineForTesting) {
                Logging::LogFmt("[PresentHookKit] NVIDIA Streamline (sl.interposer.dll) detected — g_refuseAttachOnStreamlineForTesting=true, refusing to attach.");
                return;
            }
            Logging::LogFmt("[PresentHookKit] NVIDIA Streamline (sl.interposer.dll) detected — attaching anyway. DX12 will select its D3D11On12 render path instead of the native ImGui_ImplDX12 path once it attaches (see DX12::LazyInit).");
        }
        g_attachRequested = true;
    }
    inline HWND g_gameHwnd = nullptr;
    inline WNDPROC g_originalWndProc = nullptr;
    inline bool g_minHookInitialized = false;

    inline bool EnsureMinHookInitialized() {
        if (g_minHookInitialized) return true;
        if (MH_Initialize() != MH_OK) { Logging::LogFmt("[PresentHookKit] MH_Initialize FAILED"); return false; }
        g_minHookInitialized = true;
        Logging::LogFmt("[PresentHookKit] MH_Initialize OK");
        return true;
    }

    inline LRESULT CALLBACK OverlayWndProcUnsafe(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        if (g_imguiContextCreated) {
            ImGuiIO& io = ImGui::GetIO();
            bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL;
            bool isKeyMsg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR;
            if ((isMouseMsg && io.WantCaptureMouse) || (isKeyMsg && io.WantCaptureKeyboard))
                return TRUE;
        }
        return CallWindowProcA(g_originalWndProc, hWnd, msg, wParam, lParam);
    }

    // Previously the only unprotected path in this file — every render-
    // thread path (LazyInit/RenderFrame/ResizeBuffers) is SEH-wrapped and
    // logs on fault, but this runs on the WINDOW'S OWNING THREAD and races
    // unsynchronized against the render thread creating/tearing down the
    // global ImGui context (EnsureImGuiContext/TearDown/DestroyContext).
    // A fullscreen toggle floods this with messages at the exact moment
    // that reattach logic may run — leading suspect for a crash that left
    // zero log trace (nothing else in this file fails silently).
    inline LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        __try {
            return OverlayWndProcUnsafe(hWnd, msg, wParam, lParam);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Logging::LogFmt("[PresentHookKit] OverlayWndProc faulted on msg=0x%X — caught, falling back to real wndproc.", msg);
            return g_originalWndProc ? CallWindowProcA(g_originalWndProc, hWnd, msg, wParam, lParam) : DefWindowProcA(hWnd, msg, wParam, lParam);
        }
    }

    // Returns false if the attach should be aborted (cross-thread subclass
    // detected — see the check below). Previously this only LOGGED the
    // cross-thread condition and proceeded anyway; observed twice now on
    // Daemon X Machina's GPU-crash-dump failures, always present when the
    // crash happens. Not proven as the root cause, but real and cheap to
    // gate on directly, unlike trying to detect a third-party module
    // (sl.interposer.dll) that may load asynchronously after our own
    // one-time check runs — this check is synchronous and always accurate
    // at the moment it matters.
    inline bool ClaimHwndAndSubclassUnsafe(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return false;
        if (hwnd == g_gameHwnd) return true; // already attached to this exact window

        // Logged only, NOT a reason to abort — tried aborting on this
        // condition, then found RE3 hits it too and never crashes,
        // falsifying it as a Daemon X Machina-specific crash predictor.
        // It's an ordinary, harmless timing quirk (which thread happens to
        // own the window vs. which thread Present fires on) present on
        // multiple games, not a real signal on its own. Kept as a log
        // line only in case it becomes useful context alongside some
        // other, real differentiator later.
        DWORD ownerTid = GetWindowThreadProcessId(hwnd, nullptr);
        DWORD callerTid = GetCurrentThreadId();
        if (ownerTid != callerTid) {
            Logging::LogFmt("[PresentHookKit] ClaimHwndAndSubclass: calling SetWindowLongPtrA from tid=%lu but window owned by tid=%lu — cross-thread subclass (informational only, not a crash predictor — see this line's own comment).", callerTid, ownerTid);
        }

        if (g_gameHwnd && IsWindow(g_gameHwnd) && g_originalWndProc) {
            SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        }

        g_gameHwnd = hwnd;
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OverlayWndProc)));
        // HotkeyPoll no longer started here — it needs to run BEFORE
        // attach now (it's the trigger for RequestAttach(), not something
        // attach itself sets up). Started independently from
        // SteamSwitcherOverlay.cpp's own startup instead.
        Logging::LogFmt("[PresentHookKit] Overlay input attached to HWND 0x%p.", hwnd);
        return true;
    }

    // Previously had ZERO fault protection despite manipulating window
    // procs during exactly the transition window where crashes keep
    // occurring (identical ntdll.dll fault offset across two separate
    // runs — a real HWND that gets destroyed/recreated mid-call here,
    // between the IsWindow() check and the actual SetWindowLongPtrA call,
    // is a plausible TOCTOU-style cause). Wrapping doesn't yet PROVE this
    // was the cause — just closes a real, previously-open gap.
    inline bool ClaimHwndAndSubclass(HWND hwnd) {
        __try { return ClaimHwndAndSubclassUnsafe(hwnd); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Logging::LogFmt("[PresentHookKit] ClaimHwndAndSubclass faulted on hwnd=0x%p — caught, treating as attach failure.", hwnd);
            return false;
        }
    }

    inline HWND CreateDummyWindow() {
        static const char* kClassName = "SteamSwitcherOverlayDummyWnd";
        WNDCLASSEXA wc = { sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = kClassName;
        RegisterClassExA(&wc);
        return CreateWindowExA(0, kClassName, "SteamSwitcherOverlayDummy", WS_POPUP,
            0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    }

    inline void EnsureImGuiContext() {
        if (g_imguiContextCreated) return;
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr; // never write imgui.ini into the game's own folder
        g_imguiContextCreated = true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // DX9 — DISABLED IN PRODUCTION, NOT DELETED. See InstallWorkerThreadProc
    // near the bottom of this file: DX9::Install() is never called. All the
    // code below is fully functional and was working logic as of the last
    // test pass (SEH-wrapped, permanent-abort-flag protected, same as
    // DX11/DX12) — it was disabled purely because it was never confirmed
    // firing on a real DX9 game across this whole session, so there's no
    // positive evidence it's actually safe in practice, only that it never
    // got a chance to prove itself either way. To re-enable: uncomment/
    // re-add the DX9::Install() call in InstallWorkerThreadProc, then
    // actually test against a real DX9 game before trusting it again.
    // ═══════════════════════════════════════════════════════════════════════
    namespace DX9 {
        typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
        inline EndScene_t g_origEndScene = nullptr;
        inline bool g_installed = false;
        inline bool g_imguiInit = false;
        inline bool g_deviceLost = false;
        inline bool g_renderDisabledAfterFault = false;
        inline bool g_attachPermanentlyAborted = false; // set once ClaimHwndAndSubclass refuses — without this, LazyInit retried the ENTIRE init sequence every frame forever, 60x/sec of real device/ImGui work, on the same guaranteed-to-fail path

        inline void LazyInit(IDirect3DDevice9* device) {
            if (g_imguiInit || g_attachPermanentlyAborted) return;
            if (!g_attachRequested) return; // delayed attach — see g_attachRequested's own comment
            D3DDEVICE_CREATION_PARAMETERS cp = {};
            if (FAILED(device->GetCreationParameters(&cp)) || !cp.hFocusWindow) {
                Logging::LogFmt("[PresentHookKit] DX9 LazyInit: GetCreationParameters failed or no hFocusWindow — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                return;
            }

            EnsureImGuiContext();
            if (!ImGui_ImplWin32_Init(cp.hFocusWindow)) {
                Logging::LogFmt("[PresentHookKit] DX9 LazyInit: ImGui_ImplWin32_Init failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                return;
            }
            g_win32BackendActive = true;
            if (!ImGui_ImplDX9_Init(device)) {
                Logging::LogFmt("[PresentHookKit] DX9 LazyInit: ImGui_ImplDX9_Init failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                ImGui_ImplWin32_Shutdown(); g_win32BackendActive = false;
                return;
            }

            if (!ClaimHwndAndSubclass(cp.hFocusWindow)) {
                Logging::LogFmt("[PresentHookKit] DX9 attach aborted — ClaimHwndAndSubclass refused. Not retrying this session.");
                g_attachPermanentlyAborted = true;
                ImGui_ImplDX9_Shutdown();
                ImGui_ImplWin32_Shutdown(); g_win32BackendActive = false;
                return;
            }
            g_imguiInit = true;
            Logging::LogFmt("[PresentHookKit] Overlay attached via Direct3D9 EndScene.");
        }

        inline bool TryLazyInit(IDirect3DDevice9* device) {
            __try { LazyInit(device); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        inline void RenderFrameUnsafe() {
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            Overlay::DrawOverlay();
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }

        inline bool TryRenderFrame() {
            __try { RenderFrameUnsafe(); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        inline HRESULT STDMETHODCALLTYPE HookedEndScene(IDirect3DDevice9* device) {
            if (!g_renderDisabledAfterFault) {
                if (!TryLazyInit(device)) {
                    g_renderDisabledAfterFault = true;
                    Logging::LogFmt("[PresentHookKit] Overlay init faulted on this game and has been disabled for the session (D3D9).");
                }
                else if (g_imguiInit) {
                    HRESULT coop = device->TestCooperativeLevel();
                    if (coop == D3D_OK) {
                        if (g_deviceLost) { ImGui_ImplDX9_CreateDeviceObjects(); g_deviceLost = false; }
                        if (!TryRenderFrame()) {
                            g_renderDisabledAfterFault = true;
                            Logging::LogFmt("[PresentHookKit] Overlay render faulted on this game and has been disabled for the session (D3D9).");
                        }
                    }
                    else if (coop == D3DERR_DEVICENOTRESET && !g_deviceLost) {
                        ImGui_ImplDX9_InvalidateDeviceObjects();
                        g_deviceLost = true;
                    }
                    else if (coop == D3DERR_DEVICELOST) {
                        g_deviceLost = true;
                    }
                }
            }
            return g_origEndScene(device);
        }

        inline bool Install() {
            if (!EnsureMinHookInitialized()) return false;
            HWND dummyWnd = CreateDummyWindow();
            if (!dummyWnd) return false;

            bool ok = false;
            IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
            if (d3d9) {
                D3DPRESENT_PARAMETERS pp = {};
                pp.Windowed = TRUE;
                pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                pp.BackBufferFormat = D3DFMT_UNKNOWN;
                pp.hDeviceWindow = dummyWnd;

                IDirect3DDevice9* dummyDevice = nullptr;
                HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyWnd,
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dummyDevice);
                if (SUCCEEDED(hr) && dummyDevice) {
                    void** vtable = *reinterpret_cast<void***>(dummyDevice);
                    void* targetFn = vtable[42]; // IDirect3DDevice9::EndScene
                    if (MH_CreateHook(targetFn, reinterpret_cast<void*>(&HookedEndScene),
                        reinterpret_cast<void**>(&g_origEndScene)) == MH_OK &&
                        MH_EnableHook(targetFn) == MH_OK) {
                        g_installed = true;
                        ok = true;
                    }
                    dummyDevice->Release();
                }
                d3d9->Release();
            }
            DestroyWindow(dummyWnd);
            Logging::LogFmt(ok ? "[PresentHookKit] DX9 installed." : "[PresentHookKit] DX9 not installed (game likely doesn't use it).");
            return ok;
        }

        inline void Uninstall() {
            if (g_imguiInit) { ImGui_ImplDX9_Shutdown(); g_imguiInit = false; }
            g_installed = false;
        }
    } // namespace DX9

    // DX12 has TWO render paths, selected per-attach (DX12::g_usingD3D11On12,
    // decided in DX12::LazyInit): native ImGui_ImplDX12 by default (RE3-
    // proven, unchanged), and D3D11On12 only when Streamline is loaded.
    // See the DX12 namespace's own header comment for the full technical
    // reasoning on why the native path collides with Streamline (confirmed:
    // GPU crash dump, sl_interposer.dll in the stack, attach-only survived
    // 18+s but real ExecuteCommandLists submission crashed in ~1s) and why
    // D3D11On12 avoids it (confirmed via disassembly of
    // gameoverlayrenderer64.dll that Steam's own D3D12 overlay uses the
    // same technique, and never hooks ExecuteCommandLists at all).
    //
    // Forward declaration — DX11::HookedPresent falls through to this when
    // its own D3D11 device acquisition fails, per this file's own header
    // comment on the DX12 architecture decision.
    namespace DX12 {
        inline void TryInitAndRender(IDXGISwapChain* swapChain);
        extern volatile bool g_hasCapturedQueue;
        // Authoritative queue capture — see HookedCreateSwapChainForHwnd's
        // own comment for why this replaced the old "first queue to call
        // ExecuteCommandLists" heuristic.
        inline void OnSwapChainCreationQueueSeen(IUnknown* pDevice);

        // Called from the SHARED HookedResizeBuffers (installed via DX11's
        // vtable patch, but — same as Present — fires for DX12 games too).
        // Real bug found via Daemon X Machina's own UE crash reporter:
        // "SwapChain1->ResizeBuffers(...) failed with error
        // DXGI_ERROR_INVALID_CALL" — DXGI requires ALL outstanding
        // references to a swapchain's backbuffers released before resize
        // succeeds. DX12::g_backBuffers[] holds live ID3D12Resource* refs
        // that were NEVER released on resize, because DX12 previously had
        // no resize handling of its own at all — this is that missing
        // handling, not a guess.
        inline void PrepareForResize();
        inline void RecreateAfterResize(IDXGISwapChain* swapChain);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // DX11 — also the universal frame-boundary signal for DX12 (see this
    // file's header comment).
    // ═══════════════════════════════════════════════════════════════════════
    namespace DX11 {
        typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
        typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        // MinHook, not vtable-swap — see this file's own header comment:
        // vtable-swap avoided the fullscreen-transition ntdll.dll crash but
        // reintroduced the ORIGINAL 0xC00000FD stack-overflow crash inside
        // gameoverlayrenderer64.dll on startup — the exact crash this whole
        // redesign exists to avoid, and the reason MinHook was adopted in
        // the first place. Confirmed via direct A/B test: MinHook never
        // produced this crash across many hours of testing; vtable-swap
        // reproduced it on the very first RE3 launch. MinHook patches the
        // function once, centrally — no per-vtable-instance tracking needed.
        inline Present_t g_origPresent = nullptr;
        inline ResizeBuffers_t g_origResizeBuffers = nullptr;
        inline bool g_resizeHooked = false;
        inline bool g_installed = false;
        inline bool g_imguiInit = false;
        inline bool g_renderDisabledAfterFault = false;
        inline bool g_attachPermanentlyAborted = false; // set once ClaimHwndAndSubclass refuses — see DX9's identical flag for why this matters

        // Forward declaration — defined later (near PatchIfNew/Install),
        // but called from LazyInit, which appears earlier in this
        // namespace. Unlike class member functions, free functions in a
        // namespace need a prior declaration before use in a single-pass
        // compile.
        inline void EnsureResizeHook(void** vtable);

        inline ID3D11Device* g_device = nullptr;
        inline ID3D11DeviceContext* g_context = nullptr;
        inline ID3D11RenderTargetView* g_mainRTV = nullptr;
        inline bool g_confirmedNotD3D11 = false; // set once GetDevice(ID3D11Device) fails — avoids retrying every frame forever
        inline BOOL g_lastKnownWindowed = TRUE; // updated every frame in HookedPresent, ahead of any ResizeBuffers call

        // Proactive avoidance instead of purely reactive SEH — per user's
        // own theory, the window/swapchain may be transiently invalid for
        // a couple seconds during an exclusive-fullscreen switch. Rather
        // than keep trying to touch it immediately (SEH-wrapped or not)
        // and hoping to survive whatever state it's in, back off entirely
        // for a full second after any detected resize before attempting
        // ANYTHING again — no render, no RTV recreation, nothing.
        inline std::chrono::steady_clock::time_point g_transitionCooldownUntil{};

        inline bool InTransitionCooldown() {
            return std::chrono::steady_clock::now() < g_transitionCooldownUntil;
        }

        inline void StartTransitionCooldown() {
            g_transitionCooldownUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        }

        inline void CreateRenderTarget(IDXGISwapChain* swapChain) {
            ID3D11Texture2D* backBuffer = nullptr;
            HRESULT hrGetBuffer = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
            if (SUCCEEDED(hrGetBuffer)) {
                HRESULT hrCreateRTV = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_mainRTV);
                backBuffer->Release();
                if (FAILED(hrCreateRTV)) {
                    Logging::LogFmt("[PresentHookKit] CreateRenderTargetView FAILED, hr=0x%08X — g_mainRTV will stay null, rendering silently stops.", hrCreateRTV);
                }
            }
            else {
                Logging::LogFmt("[PresentHookKit] swapChain->GetBuffer(0) FAILED, hr=0x%08X — g_mainRTV will stay null, rendering silently stops.", hrGetBuffer);
            }
        }

        // Retry path needs SEH — unlike LazyInit/RenderFrameUnsafe (already
        // wrapped), the retry call site in HookedPresent called this
        // directly, unprotected. If the swapchain is genuinely invalid
        // mid-transition (the fullscreen-switch crash this was added to
        // investigate), GetBuffer/CreateRenderTargetView faulting here
        // would previously take the whole game down uncaught.
        inline bool TryCreateRenderTarget(IDXGISwapChain* swapChain) {
            __try { CreateRenderTarget(swapChain); return g_mainRTV != nullptr; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        inline HWND g_attachedWindow = nullptr; // which window we're CURRENTLY rendering against — may change mid-session

        inline void TearDown() {
            if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
            if (g_imguiInit) {
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                g_win32BackendActive = false;
                if (g_context) { g_context->Release(); g_context = nullptr; }
                if (g_device) { g_device->Release(); g_device = nullptr; }
                g_imguiInit = false;
            }
            g_attachedWindow = nullptr;
        }

        // Returns false (cleanly, not an error) on a real DX12 game — the
        // swapchain isn't actually D3D11-backed, so GetDevice() requesting
        // ID3D11Device simply fails. That's the expected, correct signal
        // this file's DX12 fallback logic relies on.
        //
        // Also re-checks the window every call, even once already attached
        // — some games (confirmed real: RE3 windowed→fullscreen transition
        // during startup) recreate their swapchain/window after this DLL's
        // first successful attach. Without this check, HookedPresent would
        // keep firing (MinHook patches the shared FUNCTION's own code, so
        // ANY swapchain instance calling through it triggers our hook,
        // regardless of which specific swapchain object called Present)
        // while silently rendering into an orphaned render target from the
        // OLD, no-longer-displayed swapchain — no crash, no error, just
        // nothing visible, which matches exactly what testing found.
        inline bool LazyInitUnsafe(IDXGISwapChain* swapChain) {
            if (g_attachPermanentlyAborted) return false;
            if (!g_attachRequested) return g_imguiInit; // delayed attach — see g_attachRequested's own comment; also avoids permanently latching g_confirmedNotD3D11 before we've even tried
            ID3D11Device* device = nullptr;
            if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
                // Only a PERMANENT "not D3D11" signal if we've never
                // successfully attached before. Once we know this IS a
                // D3D11 game, a later transient GetDevice failure (very
                // plausible mid windowed<->fullscreen transition — a real,
                // observed cause of exactly this) must NOT permanently
                // disable rendering forever. Previously it did — a real
                // regression that looked like "rendering silently stops
                // after a few seconds," found via the diagnostic snapshot
                // log going quiet and never resuming.
                if (!g_imguiInit) g_confirmedNotD3D11 = true;
                return g_imguiInit; // keep using the existing attachment if we have one; retry next frame either way
            }

            DXGI_SWAP_CHAIN_DESC desc = {};
            if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow) { device->Release(); return g_imguiInit; }

            if (g_imguiInit && desc.OutputWindow == g_attachedWindow) {
                device->Release(); // already attached to the correct, current window — nothing to do
                return true;
            }

            if (g_imguiInit) {
                Logging::LogFmt("[PresentHookKit] DX11 swapchain/window changed (0x%p -> 0x%p) — reattaching.",
                    g_attachedWindow, desc.OutputWindow);
                TearDown();
            }

            EnsureImGuiContext();
            if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
                Logging::LogFmt("[PresentHookKit] DX11 LazyInit: ImGui_ImplWin32_Init failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                device->Release(); return false;
            }
            g_win32BackendActive = true;

            g_device = device;
            g_device->GetImmediateContext(&g_context);

            if (!ImGui_ImplDX11_Init(g_device, g_context)) {
                Logging::LogFmt("[PresentHookKit] DX11 LazyInit: ImGui_ImplDX11_Init failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                ImGui_ImplWin32_Shutdown();
                g_win32BackendActive = false;
                if (g_context) { g_context->Release(); g_context = nullptr; }
                g_device->Release(); g_device = nullptr;
                return false;
            }
            CreateRenderTarget(swapChain);

            if (!ClaimHwndAndSubclass(desc.OutputWindow)) {
                Logging::LogFmt("[PresentHookKit] DX11 attach aborted — ClaimHwndAndSubclass refused. Not retrying this session.");
                g_attachPermanentlyAborted = true;
                if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown(); g_win32BackendActive = false;
                if (g_context) { g_context->Release(); g_context = nullptr; }
                if (g_device) { g_device->Release(); g_device = nullptr; }
                return false;
            }
            g_attachedWindow = desc.OutputWindow;
            g_imguiInit = true;
            EnsureResizeHook(*reinterpret_cast<void***>(swapChain));
            swapChain->GetFullscreenState(&g_lastKnownWindowed, nullptr);
            Logging::LogFmt("[PresentHookKit] Overlay attached via DXGI Present (D3D11).");
            return true;
        }

        inline void RenderFrameUnsafe() {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            Overlay::DrawOverlay();
            ImGui::EndFrame();
            ImGui::Render();
            g_context->OMSetRenderTargets(1, &g_mainRTV, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        inline bool TryRenderFrame() {
            __try { RenderFrameUnsafe(); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        inline bool TryLazyInit(IDXGISwapChain* swapChain) {
            __try { return LazyInitUnsafe(swapChain); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return g_imguiInit; }
        }

        inline HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
            // Heartbeat diagnostic (throttled, once/sec) was here during
            // the DX12 queue-capture investigation — removed now that the
            // root cause (wrong queue captured) is found and fixed (see
            // HookedExecuteCommandLists's own comment) and confirmed
            // stable across repeated real tests. Was pure log volume with
            // no further diagnostic value once the bug was understood; re-add
            // if DX12 debugging resumes on a NEW issue — pattern was: log
            // swapChain/flags/g_imguiInit/g_mainRTV/g_confirmedNotD3D11/
            // g_renderDisabledAfterFault (DX11) plus the equivalent DX12
            // fields, throttled to 1/sec via a static last-logged timestamp.

            if (!g_renderDisabledAfterFault && !InTransitionCooldown()) {
                if (g_confirmedNotD3D11) {
                    if (DX12::g_hasCapturedQueue && !(flags & DXGI_PRESENT_TEST))
                        DX12::TryInitAndRender(swapChain);
                }
                else {
                    bool isD3D11 = TryLazyInit(swapChain);

                    if (isD3D11 && g_imguiInit && g_mainRTV) {
                        BOOL windowedNow = TRUE;
                        bool gotState = false;
                        __try { gotState = SUCCEEDED(swapChain->GetFullscreenState(&windowedNow, nullptr)); }
                        __except (EXCEPTION_EXECUTE_HANDLER) { gotState = false; }
                        if (gotState && windowedNow != g_lastKnownWindowed) {
                            Logging::LogFmt("[PresentHookKit] DX11 windowed state changed (%d -> %d) ahead of any ResizeBuffers call — releasing RTV and entering cooldown before rendering this frame.",
                                (int)g_lastKnownWindowed, (int)windowedNow);
                            g_lastKnownWindowed = windowedNow;
                            __try { if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; } }
                            __except (EXCEPTION_EXECUTE_HANDLER) { g_mainRTV = nullptr; }
                            StartTransitionCooldown();
                        }
                    }

                    if (isD3D11 && g_imguiInit && !g_mainRTV && !(flags & DXGI_PRESENT_TEST)) {
                        // Attached, but the render target is missing — most
                        // likely CreateRenderTarget failed after a real
                        // ResizeBuffers call (see that function's own
                        // logging for the actual HRESULT). Retry, but
                        // throttled — not every single frame — since
                        // hammering a possibly-still-invalid swapchain at
                        // 60+/sec during a fragile transition window (e.g.
                        // fullscreen switch) is itself a real crash risk,
                        // now SEH-wrapped either way.
                        static int retryCooldown = 0;
                        if (retryCooldown <= 0) {
                            if (!TryCreateRenderTarget(swapChain)) retryCooldown = 30; // ~0.5s at 60fps before trying again
                        }
                        else {
                            --retryCooldown;
                        }
                    }
                    if (isD3D11 && g_imguiInit && g_mainRTV && !(flags & DXGI_PRESENT_TEST)) {
                        if (!TryRenderFrame()) {
                            g_renderDisabledAfterFault = true;
                            Logging::LogFmt("[PresentHookKit] Overlay render faulted on this game and has been disabled for the session (D3D11).");
                        }
                    }
                    else if (!isD3D11 && DX12::g_hasCapturedQueue && !(flags & DXGI_PRESENT_TEST)) {
                        DX12::TryInitAndRender(swapChain);
                    }
                }
            }

            return g_origPresent ? g_origPresent(swapChain, syncInterval, flags) : DXGI_ERROR_INVALID_CALL;
        }

        // Was MISSING from this file's original rewrite — the archive's
        // version had this, dropped when porting to MinHook, only caught
        // as a documented "known gap," not fixed, until a real crash
        // (dxgi.dll, 0xC0000005, after a long session that very likely
        // included a window resize/minimize/alt-tab) confirmed it matters.
        // Without this, g_mainRTV keeps pointing at a backbuffer the real
        // ResizeBuffers call has already destroyed — using it in the next
        // frame's OMSetRenderTargets/RenderDrawData is exactly the kind of
        // thing that corrupts DXGI/driver state rather than failing cleanly.
        inline HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
            UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags)
        {
            // SEH around our own pre/post logic only — never around the
            // real origResizeBuffers call itself, which must always run
            // regardless of whether our own bookkeeping succeeds, or the
            // game's actual resize would silently never happen. Added
            // after a real crash (ntdll.dll, 0xC0000005) during a
            // borderless->fullscreen switch, at exactly the moment this
            // function runs — previously had zero fault protection despite
            // executing synchronously inside the game's own resize call
            // chain during what's already a fragile transition window.
            __try {
                if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Logging::LogFmt("[PresentHookKit] HookedResizeBuffers: faulted releasing old RTV, continuing to real resize anyway.");
                g_mainRTV = nullptr;
            }

            // THE actual fix for the DXGI_ERROR_INVALID_CALL confirmed via
            // Daemon X Machina's own UE crash reporter — release DX12's
            // outstanding backbuffer references BEFORE the real resize
            // call, or it fails exactly as observed. Cheap no-op if DX12
            // never attached (g_backBuffers empty). Must run before
            // origResizeBuffers, not after — see forward-declaration
            // comment for the full explanation.
            __try {
                DX12::PrepareForResize();
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Logging::LogFmt("[PresentHookKit] HookedResizeBuffers: faulted in DX12::PrepareForResize, continuing to real resize anyway.");
            }

            if (!g_origResizeBuffers) return DXGI_ERROR_INVALID_CALL;
            HRESULT hr = g_origResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags);
            Logging::LogFmt("[PresentHookKit] HookedResizeBuffers: real resize returned hr=0x%08X", hr);

            // Back off entirely for a full second instead of immediately
            // trying to recreate the RTV right here — per the cooldown
            // mechanism's own comment. HookedPresent's retry picks this up
            // automatically once the cooldown expires; no need to attempt
            // anything from this function at all.
            StartTransitionCooldown();
            Logging::LogFmt("[PresentHookKit] HookedResizeBuffers: resize detected, entering 1s transition cooldown before touching anything again.");
            return hr;
        }

        inline void PatchIfNew(void** vtable) {
            if (!vtable || g_origPresent) return; // MinHook only needs one install for the whole shared function
            if (vtable[8] == reinterpret_cast<void*>(&HookedPresent)) {
                Logging::LogFmt("[PresentHookKit] DXGI vtable already points at our own Present hook — skipped re-patch to avoid self-recursion.");
                return;
            }
            MH_STATUS createStatus = MH_CreateHook(vtable[8], reinterpret_cast<void*>(&HookedPresent),
                reinterpret_cast<void**>(&g_origPresent));
            if (createStatus == MH_OK && MH_EnableHook(vtable[8]) == MH_OK) {
                g_installed = true;
                Logging::LogFmt("[PresentHookKit] DX11 Present hooked via MinHook.");
            }
            else {
                Logging::LogFmt("[PresentHookKit] DX11 PatchIfNew: MH_CreateHook status=%d on vtable[8]=0x%p", (int)createStatus, vtable[8]);
            }
        }

        // Hooking ResizeBuffers only provides value once we're actually
        // holding resources that need releasing before a resize — before
        // that point it's pure risk for zero benefit. Real crash evidence
        // (Daemon X Machina, UE's own crash reporter: "SwapChain1->
        // ResizeBuffers(...) failed with DXGI_ERROR_INVALID_CALL") kept
        // happening on the VERY FIRST resize — banner-to-game-window —
        // BEFORE this DLL had ever attached at all (g_backBuffers still
        // empty, PrepareForResize a guaranteed no-op at that point). Only
        // install it once we've actually attached via Present — called
        // from DX11's and DX12's own LazyInit, right after a successful
        // attach. MinHook, same as Present — see this namespace's own
        // header comment for why.
        inline void EnsureResizeHook(void** vtable) {
            if (!vtable || g_resizeHooked) return;
            if (vtable[13] == reinterpret_cast<void*>(&HookedResizeBuffers)) return;
            if (MH_CreateHook(vtable[13], reinterpret_cast<void*>(&HookedResizeBuffers),
                reinterpret_cast<void**>(&g_origResizeBuffers)) == MH_OK &&
                MH_EnableHook(vtable[13]) == MH_OK) {
                g_resizeHooked = true;
                Logging::LogFmt("[PresentHookKit] ResizeBuffers hooked via MinHook post-attach.");
            }
        }

        // Daemon X Machina (UE5) confirmed to have TWO distinct D3D12
        // DIRECT-type queues (priority 0 and priority 100) — the old
        // "first queue to call ExecuteCommandLists" heuristic, even after
        // being filtered to DIRECT-type-only, can still grab the WRONG one
        // when a game has more than one. Real DX12 attach still crashed
        // (GPU Crash dump, sl_interposer in stack) even with a confirmed
        // DIRECT queue captured — this is why: DIRECT-type alone doesn't
        // guarantee it's the swapchain's actual presentation queue.
        //
        // The reliable source: `pDevice` in
        // IDXGIFactory2::CreateSwapChainForHwnd IS the presentation queue
        // for a D3D12 swapchain, by API contract — no ambiguity, no
        // guessing. Same shared-function-patch approach as Present/
        // ResizeBuffers (MinHook patches the function's own code, so this
        // fires for every swapchain creation in the process, D3D9/11/12
        // alike — harmless no-op QueryInterface failure for non-D3D12
        // callers, including our own dummy DX11 swapchain below).
        typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainForHwnd_t)(
            IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
        inline CreateSwapChainForHwnd_t g_origCreateSwapChainForHwnd = nullptr;
        inline bool g_factoryPatched = false;

        inline HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
            IDXGIFactory2* factory, IUnknown* pDevice, HWND hWnd,
            const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
            IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
        {
            Logging::LogFmt("[PresentHookKit] HookedCreateSwapChainForHwnd called. factory=0x%p pDevice=0x%p hWnd=0x%p", factory, pDevice, hWnd);
            __try { DX12::OnSwapChainCreationQueueSeen(pDevice); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Logging::LogFmt("[PresentHookKit] HookedCreateSwapChainForHwnd: faulted inspecting pDevice, continuing to real call anyway.");
            }
            if (!g_origCreateSwapChainForHwnd) return DXGI_ERROR_INVALID_CALL;
            return g_origCreateSwapChainForHwnd(factory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
        }

        inline void PatchFactoryIfNew(void** vtable) {
            if (!vtable || g_factoryPatched) return;
            if (vtable[15] == reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd)) { g_factoryPatched = true; return; }
            MH_STATUS st = MH_CreateHook(vtable[15], reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd),
                reinterpret_cast<void**>(&g_origCreateSwapChainForHwnd));
            if (st == MH_OK && MH_EnableHook(vtable[15]) == MH_OK) {
                g_factoryPatched = true;
                Logging::LogFmt("[PresentHookKit] IDXGIFactory2::CreateSwapChainForHwnd hooked via MinHook (vtable[15]) — authoritative DX12 presentation-queue capture enabled.");
            }
            else {
                Logging::LogFmt("[PresentHookKit] PatchFactoryIfNew: MH_CreateHook status=%d on vtable[15]=0x%p", (int)st, vtable[15]);
            }
        }

        inline bool Install() {
            if (!EnsureMinHookInitialized()) return false;
            IDXGIFactory1* factory1 = nullptr;
            HRESULT hrFactory = CreateDXGIFactory1(IID_PPV_ARGS(&factory1));
            if (FAILED(hrFactory) || !factory1) {
                Logging::LogFmt("[PresentHookKit] DX11 Install: CreateDXGIFactory1 failed, hr=0x%08X", hrFactory);
                return false;
            }
            IDXGIFactory2* factory2 = nullptr;
            factory1->QueryInterface(IID_PPV_ARGS(&factory2));
            if (!factory2) Logging::LogFmt("[PresentHookKit] DX11 Install: QueryInterface(IDXGIFactory2) failed — flip-model swapchain unavailable.");
            else PatchFactoryIfNew(*reinterpret_cast<void***>(factory2));

            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
            IDXGIAdapter1* adapter = nullptr;
            UINT adapterCount = 0;
            for (UINT i = 0; factory1->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                ++adapterCount;
                ID3D11Device* device = nullptr;
                ID3D11DeviceContext* context = nullptr;
                HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                    levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
                if (SUCCEEDED(hr) && device) {
                    if (factory2) {
                        HWND flipWnd = CreateDummyWindow();
                        if (flipWnd) {
                            DXGI_SWAP_CHAIN_DESC1 desc1 = {};
                            desc1.BufferCount = 2;
                            desc1.Width = 4; desc1.Height = 4;
                            desc1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                            desc1.SampleDesc.Count = 1;
                            desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                            IDXGISwapChain1* sc1 = nullptr;
                            HRESULT hrSc = factory2->CreateSwapChainForHwnd(device, flipWnd, &desc1, nullptr, nullptr, &sc1);
                            if (SUCCEEDED(hrSc) && sc1) {
                                void** vtable = *reinterpret_cast<void***>(sc1);
                                PatchIfNew(vtable);
                                sc1->Release();
                            }
                            else {
                                Logging::LogFmt("[PresentHookKit] DX11 Install: CreateSwapChainForHwnd failed on adapter %u, hr=0x%08X", i, hrSc);
                            }
                            DestroyWindow(flipWnd);
                        }
                    }
                    if (context) context->Release();
                    device->Release();
                }
                else {
                    Logging::LogFmt("[PresentHookKit] DX11 Install: D3D11CreateDevice failed on adapter %u, hr=0x%08X", i, hr);
                }
                if (adapter) adapter->Release();
            }
            if (adapterCount == 0) Logging::LogFmt("[PresentHookKit] DX11 Install: EnumAdapters1 returned zero adapters.");
            if (factory2) factory2->Release();
            factory1->Release();

            Logging::LogFmt(g_installed ? "[PresentHookKit] DX11 installed." : "[PresentHookKit] DX11 not installed.");
            return g_installed;
        }

        inline void Uninstall() {
            TearDown();
            // MH_Uninitialize() (called from the shared UninstallAll())
            // handles unhooking now — back to MinHook-managed.
            g_installed = false;
        }
    } // namespace DX11

    // ═══════════════════════════════════════════════════════════════════════
    // DX12 — DISABLED AGAIN (see the forward-decl block above for the full
    // history). NO Present hook of its own. ExecuteCommandLists is the
    // only hook (MinHook) — previously described as "not implicated in the
    // Steam-overlay collision, no swapchain involved," but that claim no
    // longer holds: the RE3-DX12 crash happened with this exact hook
    // actively firing (full attach succeeded, no Streamline gate blocking
    // it), so ExecuteCommandLists can no longer be confidently ruled out
    // as a contributor — genuinely unknown, not cleared. Rendering (when
    // enabled) is driven from DX11's HookedPresent (universal
    // frame-boundary signal) via TryInitAndRender(). Genuinely more
    // involved territory (handover doc §4) — descriptor heap,
    // per-backbuffer RTVs, a frame-context ring (allocator + fence value
    // per swapchain buffer), and explicit resource-barrier/fence
    // synchronization DX9/DX11 never needed at all — any of which could
    // plausibly be the actual Steam-overlay collision point, untested
    // individually.
    // ═══════════════════════════════════════════════════════════════════════
    namespace DX12 {
        typedef HRESULT(STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
        inline ExecuteCommandLists_t g_origExecuteCommandLists = nullptr;
        inline bool g_installed = false;
        inline ID3D12CommandQueue* g_capturedQueue = nullptr;
        inline volatile bool g_hasCapturedQueue = false; // matches the extern declared up in DX11's forward-decl block

        inline bool g_imguiInit = false;
        inline bool g_renderDisabledAfterFault = false;
        inline bool g_attachPermanentlyAborted = false; // set once ClaimHwndAndSubclass refuses — see DX9's identical flag.
        inline ID3D12Device* g_device = nullptr; // borrowed from the swapchain, never Release()'d beyond our own AddRef

        // Decided ONCE per attach, in LazyInit — never changes mid-session.
        // false (native ImGui_ImplDX12 path) is the default and is what RE3
        // uses in both DX11 and DX12 mode, proven stable — this path is
        // UNCHANGED from before the Daemon X Machina investigation. true
        // (D3D11On12 path, below) only engages when Streamline is actually
        // loaded, since that's the one case the native path's direct
        // ExecuteCommandLists submission collides with (confirmed: attach-
        // only survived 18+s, real submission crashed in ~1s on Daemon X
        // Machina specifically). See this namespace's two Uninstall/Render
        // branches — both paths are compiled in and coexist; only one runs
        // per session.
        inline bool g_usingD3D11On12 = false;

        // Set true only by OnSwapChainCreationQueueSeen (the authoritative
        // capture) — used by LazyInit to decide whether it's safe to trust
        // g_capturedQueue for D3D11On12, or whether it's still just the
        // ExecuteCommandLists first-caller heuristic guess (ambiguous when
        // multiple real DIRECT-type queues exist, as on Daemon X Machina).
        // CONFIRMED never fires for Daemon X Machina — its real swapchain
        // is created before this DLL is injected, so HookedCreateSwapChainForHwnd
        // only ever sees unrelated (Steam-internal) swapchain creations.
        // See g_queueResolutionAttempted below for the fallback this feeds.
        inline bool g_queueCaptureIsAuthoritative = false;

        // ── Present-correlation queue resolution (Streamline-only fallback) ─
        // When authoritative capture is structurally unavailable (confirmed
        // above), the real presentation queue still has one observable
        // property the wrong queue doesn't: its ExecuteCommandLists call
        // lands immediately before each real Present, every frame. Track
        // every distinct DIRECT-type queue's last ExecuteCommandLists
        // timestamp; on each real Present, whichever queue fired most
        // recently gets a point. After enough frames, the queue with the
        // most points is almost certainly the real one — a real signal,
        // not a guess, but still probabilistic, unlike authoritative
        // capture. If no candidate ever correlates, refuse rather than
        // pick arbitrarily.
        struct DirectQueueCandidate {
            ID3D12CommandQueue* queue = nullptr;
            std::chrono::steady_clock::time_point lastExecTime{};
            int correlationScore = 0;
        };
        inline std::vector<DirectQueueCandidate> g_directQueueCandidates;
        inline int g_correlationFramesObserved = 0;
        constexpr int kCorrelationFrameTarget = 90; // ~1.5s at 60fps — generous enough to be confident, short enough not to feel stuck
        inline bool g_queueResolutionAttempted = false;  // true once correlation has either succeeded or given up
        inline bool g_queueResolvedSuccessfully = false; // true only if a real candidate won

        inline void RecordExecuteCommandListsForCorrelation(ID3D12CommandQueue* queue) {
            auto now = std::chrono::steady_clock::now();
            for (auto& c : g_directQueueCandidates) {
                if (c.queue == queue) { c.lastExecTime = now; return; }
            }
            g_directQueueCandidates.push_back({ queue, now, 0 });
        }

        // Called once per real Present (from TryInitAndRender, every frame,
        // only while resolution is still pending) — this IS the correlation
        // signal itself: whichever queue's ExecuteCommandLists landed most
        // recently before THIS Present gets a point.
        inline void RecordPresentForCorrelation() {
            if (g_queueResolutionAttempted) return;
            auto now = std::chrono::steady_clock::now();
            DirectQueueCandidate* best = nullptr;
            std::chrono::steady_clock::duration bestDelta{};
            for (auto& c : g_directQueueCandidates) {
                if (c.lastExecTime.time_since_epoch().count() == 0) continue; // never fired yet
                auto delta = now - c.lastExecTime;
                if (delta < std::chrono::milliseconds(0)) continue; // clock oddity guard
                if (!best || delta < bestDelta) { best = &c; bestDelta = delta; }
            }
            if (best && bestDelta < std::chrono::milliseconds(8)) {
                best->correlationScore++;
            }

            if (++g_correlationFramesObserved >= kCorrelationFrameTarget) {
                DirectQueueCandidate* winner = nullptr;
                for (auto& c : g_directQueueCandidates) {
                    if (!winner || c.correlationScore > winner->correlationScore) winner = &c;
                }
                g_queueResolutionAttempted = true;
                if (winner && winner->correlationScore > 0) {
                    Logging::LogFmt("[PresentHookKit] DX12 queue resolved via Present-correlation: 0x%p score=%d/%d across %zu candidate(s).",
                        winner->queue, winner->correlationScore, g_correlationFramesObserved, g_directQueueCandidates.size());
                    g_capturedQueue = winner->queue;
                    g_queueResolvedSuccessfully = true;
                }
                else {
                    Logging::LogFmt("[PresentHookKit] DX12 queue correlation inconclusive after %d frames (%zu candidate(s), no queue ever correlated with Present) — refusing to attach rather than guess.",
                        g_correlationFramesObserved, g_directQueueCandidates.size());
                }
            }
        }

        inline std::vector<ID3D12Resource*> g_backBuffers; // raw D3D12 backbuffers — shared by both paths (native path RTVs them directly; D3D11On12 path wraps them)
        inline UINT g_bufferCount = 0;

        // ── Native ImGui_ImplDX12 path state (default; RE3-proven) ─────────
        inline ID3D12DescriptorHeap* g_rtvHeap = nullptr;
        inline UINT g_rtvDescriptorSize = 0;
        inline ID3D12DescriptorHeap* g_srvHeap = nullptr;
        inline ID3D12GraphicsCommandList* g_commandList = nullptr;
        inline ID3D12Fence* g_fence = nullptr;
        inline UINT64 g_fenceLastSignaled = 0;
        inline HANDLE g_fenceEvent = nullptr;

        struct FrameContext { ID3D12CommandAllocator* allocator = nullptr; UINT64 fenceValue = 0; };
        inline std::vector<FrameContext> g_frameContexts;

        // Minimal free-list SRV descriptor allocator for ImGui_ImplDX12's
        // dynamic-texture callbacks (font atlas + any runtime textures the
        // 1.92.x backend allocates).
        inline std::vector<UINT> g_srvFreeList;
        inline UINT g_srvDescriptorSize = 0;
        constexpr UINT kSrvHeapCapacity = 64;

        inline void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
            if (g_srvFreeList.empty()) { *outCpu = {}; *outGpu = {}; return; } // out of slots — ImGui backend handles a null handle gracefully
            UINT index = g_srvFreeList.back();
            g_srvFreeList.pop_back();
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(index) * g_srvDescriptorSize;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(index) * g_srvDescriptorSize;
            *outCpu = cpu; *outGpu = gpu;
        }

        inline void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
            D3D12_CPU_DESCRIPTOR_HANDLE base = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            UINT index = static_cast<UINT>((cpu.ptr - base.ptr) / g_srvDescriptorSize);
            g_srvFreeList.push_back(index);
        }

        // ── D3D11On12 path state (Streamline-only) ──────────────────────────
        // See g_usingD3D11On12's own comment above for when this engages.
        // Confirmed via disassembly of gameoverlayrenderer64.dll that
        // Steam's own D3D12 overlay uses exactly this technique
        // (CD3D12Renderer::ValveTakeScreenshot's D3D11On12CreateDevice call)
        // and never hooks ExecuteCommandLists at all — command-list
        // submission happens inside Microsoft's D3D11on12 runtime, not via
        // a direct call of ours into the function Streamline also hooks.
        inline ID3D11On12Device* g_d3d11on12Device = nullptr;
        inline ID3D11Device* g_d3d11Device = nullptr;
        inline ID3D11DeviceContext* g_d3d11Context = nullptr;
        inline std::vector<ID3D11Resource*> g_wrappedBackBuffers; // D3D11On12-wrapped view of g_backBuffers
        inline std::vector<ID3D11RenderTargetView*> g_rtvsD3D11On12;

        inline void ReleaseBackBuffers() {
            for (auto* rtv : g_rtvsD3D11On12) if (rtv) rtv->Release();
            g_rtvsD3D11On12.clear();
            for (auto* wb : g_wrappedBackBuffers) if (wb) wb->Release();
            g_wrappedBackBuffers.clear();
            for (auto* b : g_backBuffers) if (b) b->Release();
            g_backBuffers.clear();
        }

        inline bool CreateBackBuffersAndRTVs(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC& desc) {
            ReleaseBackBuffers();
            g_bufferCount = desc.BufferCount;
            g_backBuffers.resize(g_bufferCount, nullptr);

            if (g_usingD3D11On12) {
                g_wrappedBackBuffers.resize(g_bufferCount, nullptr);
                g_rtvsD3D11On12.resize(g_bufferCount, nullptr);
                D3D11_RESOURCE_FLAGS resFlags = { D3D11_BIND_RENDER_TARGET };
                for (UINT i = 0; i < g_bufferCount; ++i) {
                    if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i])))) return false;
                    if (FAILED(g_d3d11on12Device->CreateWrappedResource(
                        g_backBuffers[i], &resFlags,
                        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                        IID_PPV_ARGS(&g_wrappedBackBuffers[i])))) return false;
                    if (FAILED(g_d3d11Device->CreateRenderTargetView(g_wrappedBackBuffers[i], nullptr, &g_rtvsD3D11On12[i]))) return false;
                }
                return true;
            }

            // Native path — unchanged from the RE3-proven original.
            if (!g_rtvHeap) {
                D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
                rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                rtvDesc.NumDescriptors = g_bufferCount;
                rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap)))) return false;
                g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < g_bufferCount; ++i) {
                if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i])))) return false;
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvStart;
                rtvHandle.ptr += static_cast<SIZE_T>(i) * g_rtvDescriptorSize;
                g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, rtvHandle);
            }
            return true;
        }

        // Called only from DX11::HookedPresent's fallback path — see this
        // file's own header comment. `swapChain` here is the REAL game
        // swapchain, not a dummy; Install() below never touches it.
        inline void Uninstall(); // forward decl — LazyInit reuses this for cleanup on a ClaimHwndAndSubclass abort

        inline void LazyInit(IDXGISwapChain* swapChain) {
            if (g_imguiInit || g_attachPermanentlyAborted) return;
            if (!g_capturedQueue) {
                static bool loggedOnce = false;
                if (!loggedOnce) { Logging::LogFmt("[PresentHookKit] DX12 LazyInit: no command queue captured yet."); loggedOnce = true; }
                return;
            }

            ID3D12Device* device = nullptr;
            if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: swapChain->GetDevice(ID3D12Device) failed.");
                return; // NOT a permanent abort — same reasoning as DX11's g_confirmedNotD3D11 gate: this can be a genuinely transient state early in a game's own startup, unlike the failures below which are real setup problems once we're already holding a valid device.
            }

            DXGI_SWAP_CHAIN_DESC desc = {};
            if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: GetDesc failed or no OutputWindow.");
                device->Release(); return;
            }

            g_device = device; // keep our own ref; released in Uninstall

            // Streamline needs the D3D11On12 path, which needs the EXACT
            // real presentation queue — a wrong queue is the same class of
            // bug that already caused a GPU crash once (RE3's original
            // wrong-queue bug). Authoritative CreateSwapChainForHwnd
            // capture is CONFIRMED unavailable for this game (its real
            // swapchain is created before injection) — RecordPresentForCorrelation
            // (called every frame from TryInitAndRender while this is
            // pending) resolves the real queue via Present-timing
            // correlation instead. Wait for that to finish rather than
            // guess; if it can't resolve anything, refuse rather than pick
            // arbitrarily.
            if (IsStreamlineLoaded() && !g_queueCaptureIsAuthoritative) {
                if (!g_queueResolutionAttempted) {
                    static bool loggedWaitOnce = false;
                    if (!loggedWaitOnce) {
                        Logging::LogFmt("[PresentHookKit] DX12 LazyInit: Streamline present, queue not yet resolved via Present-correlation — waiting rather than guessing.");
                        loggedWaitOnce = true;
                    }
                    Uninstall(); // releases g_device (just assigned above) and resets state; not a permanent abort — retry next frame
                    return;
                }
                if (!g_queueResolvedSuccessfully) {
                    Logging::LogFmt("[PresentHookKit] DX12 LazyInit: Streamline present, Present-correlation could not resolve the real queue — refusing to attach rather than guess a possibly-wrong queue into D3D11On12CreateDevice.");
                    g_attachPermanentlyAborted = true;
                    Uninstall();
                    return;
                }
                // else: RecordPresentForCorrelation already set g_capturedQueue
                // to the resolved winner — fall through and use it below.
            }

            g_usingD3D11On12 = IsStreamlineLoaded();

            if (g_usingD3D11On12) {
                IUnknown* queues[] = { g_capturedQueue };
                if (FAILED(D3D11On12CreateDevice(g_device, 0, nullptr, 0, queues, 1, 0,
                    &g_d3d11Device, &g_d3d11Context, nullptr))) {
                    Logging::LogFmt("[PresentHookKit] DX12 LazyInit: D3D11On12CreateDevice failed — aborting permanently, not retrying every frame.");
                    g_attachPermanentlyAborted = true;
                    Uninstall();
                    return;
                }
                if (FAILED(g_d3d11Device->QueryInterface(IID_PPV_ARGS(&g_d3d11on12Device)))) {
                    Logging::LogFmt("[PresentHookKit] DX12 LazyInit: QueryInterface(ID3D11On12Device) failed — aborting permanently, not retrying every frame.");
                    g_attachPermanentlyAborted = true;
                    Uninstall();
                    return;
                }
            }

            if (!CreateBackBuffersAndRTVs(swapChain, desc)) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: CreateBackBuffersAndRTVs failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                Uninstall();
                return;
            }

            if (!g_usingD3D11On12) {
                D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
                srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                srvDesc.NumDescriptors = kSrvHeapCapacity;
                srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)))) {
                    Logging::LogFmt("[PresentHookKit] DX12 LazyInit: CreateDescriptorHeap (SRV) failed — aborting permanently, not retrying every frame.");
                    g_attachPermanentlyAborted = true;
                    Uninstall();
                    return;
                }
                g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                g_srvFreeList.clear();
                for (UINT i = 0; i < kSrvHeapCapacity; ++i) g_srvFreeList.push_back(i);

                g_frameContexts.resize(g_bufferCount);
                bool allocatorsOk = true;
                for (auto& fc : g_frameContexts) {
                    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fc.allocator)))) {
                        Logging::LogFmt("[PresentHookKit] DX12 LazyInit: CreateCommandAllocator failed — aborting permanently, not retrying every frame.");
                        allocatorsOk = false;
                        break;
                    }
                }
                if (!allocatorsOk) { g_attachPermanentlyAborted = true; Uninstall(); return; }

                if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    g_frameContexts[0].allocator, nullptr, IID_PPV_ARGS(&g_commandList)))) {
                    Logging::LogFmt("[PresentHookKit] DX12 LazyInit: CreateCommandList failed — aborting permanently, not retrying every frame.");
                    g_attachPermanentlyAborted = true;
                    Uninstall();
                    return;
                }
                g_commandList->Close();

                if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
                    Logging::LogFmt("[PresentHookKit] DX12 LazyInit: CreateFence failed — aborting permanently, not retrying every frame.");
                    g_attachPermanentlyAborted = true;
                    Uninstall();
                    return;
                }
                g_fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                if (!g_fenceEvent) {
                    Logging::LogFmt("[PresentHookKit] DX12 LazyInit: CreateEventA failed — aborting permanently, not retrying every frame.");
                    g_attachPermanentlyAborted = true;
                    Uninstall();
                    return;
                }
            }

            EnsureImGuiContext();
            if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: ImGui_ImplWin32_Init failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                Uninstall();
                return;
            }
            g_win32BackendActive = true;

            bool imguiBackendOk;
            if (g_usingD3D11On12) {
                imguiBackendOk = ImGui_ImplDX11_Init(g_d3d11Device, g_d3d11Context);
            }
            else {
                ImGui_ImplDX12_InitInfo initInfo = {};
                initInfo.Device = g_device;
                initInfo.CommandQueue = g_capturedQueue;
                initInfo.NumFramesInFlight = static_cast<int>(g_bufferCount);
                initInfo.RTVFormat = desc.BufferDesc.Format;
                initInfo.SrvDescriptorHeap = g_srvHeap;
                initInfo.SrvDescriptorAllocFn = &SrvAlloc;
                initInfo.SrvDescriptorFreeFn = &SrvFree;
                imguiBackendOk = ImGui_ImplDX12_Init(&initInfo);
            }
            if (!imguiBackendOk) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: ImGui backend init failed (%s) — aborting permanently, not retrying every frame.", g_usingD3D11On12 ? "DX11" : "DX12");
                g_attachPermanentlyAborted = true;
                ImGui_ImplWin32_Shutdown(); g_win32BackendActive = false;
                Uninstall();
                return;
            }

            if (!ClaimHwndAndSubclass(desc.OutputWindow)) {
                Logging::LogFmt("[PresentHookKit] DX12 attach aborted — ClaimHwndAndSubclass refused. Not retrying this session.");
                g_attachPermanentlyAborted = true;
                Uninstall(); // full teardown — heaps/command list/fence (native) or wrapped resources/D3D11On12 device (Streamline), backbuffers, device, ImGui backends
                return;
            }
            g_imguiInit = true;
            DX11::EnsureResizeHook(*reinterpret_cast<void***>(swapChain));
            Logging::LogFmt("[PresentHookKit] Overlay attached via D3D12 (%s render path + DX11-Present frame signal).",
                g_usingD3D11On12 ? "D3D11On12, Streamline detected" : "native ImGui_ImplDX12");
        }

        inline bool TryLazyInit(IDXGISwapChain* swapChain) {
            __try { LazyInit(swapChain); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        inline void RenderFrameUnsafe(IDXGISwapChain* swapChain) {
            // GetCurrentBackBufferIndex is IDXGISwapChain3+ only.
            IDXGISwapChain3* sc3 = nullptr;
            if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
            UINT backBufferIndex = sc3->GetCurrentBackBufferIndex();
            sc3->Release();

            if (g_usingD3D11On12) {
                if (backBufferIndex >= g_wrappedBackBuffers.size()) return;

                ID3D11Resource* wrapped = g_wrappedBackBuffers[backBufferIndex];
                g_d3d11on12Device->AcquireWrappedResources(&wrapped, 1);
                g_d3d11Context->OMSetRenderTargets(1, &g_rtvsD3D11On12[backBufferIndex], nullptr);

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                Overlay::DrawOverlay();
                ImGui::EndFrame();
                ImGui::Render();
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

                // ReleaseWrappedResources + Flush submits through
                // D3D11on12's own internal queue submission — inside
                // Microsoft's runtime code, not a direct call of ours into
                // the hooked ExecuteCommandLists.
                g_d3d11on12Device->ReleaseWrappedResources(&wrapped, 1);
                g_d3d11Context->Flush();
                return;
            }

            // Native path — unchanged from the RE3-proven original.
            if (backBufferIndex >= g_frameContexts.size()) return;

            FrameContext& fc = g_frameContexts[backBufferIndex];
            if (fc.fenceValue != 0 && g_fence->GetCompletedValue() < fc.fenceValue) {
                g_fence->SetEventOnCompletion(fc.fenceValue, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, INFINITE);
            }
            fc.allocator->Reset();
            g_commandList->Reset(fc.allocator, nullptr);

            D3D12_RESOURCE_BARRIER toRT = {};
            toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRT.Transition.pResource = g_backBuffers[backBufferIndex];
            toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_commandList->ResourceBarrier(1, &toRT);

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtvHandle.ptr += static_cast<SIZE_T>(backBufferIndex) * g_rtvDescriptorSize;
            g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
            g_commandList->SetDescriptorHeaps(1, heaps);

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            Overlay::DrawOverlay();
            ImGui::EndFrame();
            ImGui::Render();

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList);

            D3D12_RESOURCE_BARRIER toPresent = toRT;
            toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_commandList->ResourceBarrier(1, &toPresent);
            g_commandList->Close();

            // Call the ORIGINAL trampoline directly, not queue->ExecuteCommandLists —
            // the latter would re-enter our own hook (MinHook patches the
            // function's own code, not just the vtable slot, so any call
            // reaching that address goes through our hook regardless of how
            // it's invoked). Harmless either way, but this is more direct.
            // Only reached when g_usingD3D11On12 is false, i.e. Streamline
            // is not loaded — this is the exact call that collides with
            // Streamline's own hook when it IS loaded, which is why that
            // case takes the D3D11On12 branch above instead.
            ID3D12CommandList* lists[] = { g_commandList };
            g_origExecuteCommandLists(g_capturedQueue, 1, lists);

            fc.fenceValue = ++g_fenceLastSignaled;
            g_capturedQueue->Signal(g_fence, fc.fenceValue);
        }

        inline bool TryRenderFrame(IDXGISwapChain* swapChain) {
            __try { RenderFrameUnsafe(swapChain); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        inline void PrepareForResize() {
            if (g_backBuffers.empty() && !g_rtvHeap && g_wrappedBackBuffers.empty()) return; // never attached, or already cleaned up — cheap no-op

            // Wait for the GPU to actually finish with these buffers, not
            // just drop our CPU-side references — releasing while the GPU
            // still has in-flight work reading/writing them is a separate,
            // worse problem than the one this function exists to fix.
            // Only the native path owns a fence; D3D11On12's Flush() already
            // handles its own synchronization per-frame.
            if (!g_usingD3D11On12 && g_fence && g_fenceEvent && g_fenceLastSignaled > 0 &&
                g_fence->GetCompletedValue() < g_fenceLastSignaled) {
                g_fence->SetEventOnCompletion(g_fenceLastSignaled, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, 2000); // bounded — never hang the game's own resize call indefinitely
            }

            ReleaseBackBuffers();
            if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; } // buffer count/size may change across the resize — force a clean recreate
            Logging::LogFmt("[PresentHookKit] DX12::PrepareForResize: released outstanding backbuffer references before real resize.");
        }

        inline void RecreateAfterResize(IDXGISwapChain* swapChain) {
            if (!g_imguiInit || !g_device) return;
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (FAILED(swapChain->GetDesc(&desc))) return;
            if (!CreateBackBuffersAndRTVs(swapChain, desc)) {
                Logging::LogFmt("[PresentHookKit] DX12::RecreateAfterResize: CreateBackBuffersAndRTVs failed.");
            }
        }

        inline void TryInitAndRender(IDXGISwapChain* swapChain) {
            if (g_renderDisabledAfterFault || g_attachPermanentlyAborted) return;
            if (!PresentHookKit::g_attachRequested) return; // delayed attach — see g_attachRequested's own comment

            // Feeds the Present-correlation queue resolution used by
            // LazyInit's Streamline branch (see that block's own comment).
            // No-ops once resolution has already succeeded or given up, or
            // if this isn't a Streamline session at all — cheap to call
            // unconditionally every frame otherwise.
            if (!g_imguiInit && IsStreamlineLoaded() && !g_queueCaptureIsAuthoritative) {
                RecordPresentForCorrelation();
            }

            if (!g_imguiInit) {
                if (!TryLazyInit(swapChain)) {
                    g_renderDisabledAfterFault = true;
                    Logging::LogFmt("[PresentHookKit] Overlay init faulted on this game and has been disabled for the session (D3D12).");
                }
                return; // render on the NEXT frame, once init has actually completed
            }
            if (g_backBuffers.empty()) {
                __try { RecreateAfterResize(swapChain); }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    Logging::LogFmt("[PresentHookKit] DX12::RecreateAfterResize faulted.");
                }
                return; // render on the NEXT frame, once recreation has actually completed
            }
            if (!TryRenderFrame(swapChain)) {
                g_renderDisabledAfterFault = true;
                Logging::LogFmt("[PresentHookKit] Overlay render faulted on this game and has been disabled for the session (D3D12).");
            }
        }

        // Authoritative queue capture, called from
        // DX11::HookedCreateSwapChainForHwnd (see that function's own
        // comment for the full "why"). pDevice IS the presentation queue
        // by DXGI's own API contract for a D3D12 swapchain — no first-
        // caller guessing needed. Always wins over the ExecuteCommandLists
        // heuristic below if both fire.
        inline void OnSwapChainCreationQueueSeen(IUnknown* pDevice) {
            ID3D12CommandQueue* queue = nullptr;
            if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
                Logging::LogFmt("[PresentHookKit] DX12 OnSwapChainCreationQueueSeen: pDevice (0x%p) did NOT QueryInterface to ID3D12CommandQueue — hook fired, but this call's pDevice isn't the queue we expected.", pDevice);
                return;
            }
            if (g_capturedQueue && g_capturedQueue != queue) {
                Logging::LogFmt("[PresentHookKit] DX12 authoritative queue (0x%p, from CreateSwapChainForHwnd) DIFFERS from previously heuristic-captured queue (0x%p) — overriding with the authoritative one.", queue, g_capturedQueue);
            }
            else if (!g_capturedQueue) {
                Logging::LogFmt("[PresentHookKit] DX12 command queue captured authoritatively via CreateSwapChainForHwnd: 0x%p", queue);
            }
            g_capturedQueue = queue;
            g_hasCapturedQueue = true;
            g_queueCaptureIsAuthoritative = true;
            queue->Release(); // QueryInterface AddRef'd it; g_capturedQueue is a borrowed, non-owning pointer same as everywhere else in this file
        }

        inline HRESULT STDMETHODCALLTYPE HookedExecuteCommandLists(
            ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists)
        {
            // Shared-function patch (MinHook patches the function's own
            // code, not a per-instance vtable slot) — fires for EVERY
            // ID3D12CommandQueue::ExecuteCommandLists call in the process,
            // on ANY queue, not just the swapchain's real presentation
            // queue. RE3-DX12 confirmed to use 3 separate queues (COPY,
            // DIRECT, COMPUTE) — a DXGI swapchain's presentation queue is
            // always CreateCommandQueue'd as D3D12_COMMAND_LIST_TYPE_DIRECT,
            // so reject anything else. Every distinct queue seen is logged
            // once per session.
            {
                static std::vector<ID3D12CommandQueue*> seenQueues;
                bool alreadySeen = false;
                for (auto* q : seenQueues) if (q == queue) { alreadySeen = true; break; }
                if (!alreadySeen) {
                    seenQueues.push_back(queue);
                    D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
                    const char* typeName = qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "DIRECT" :
                        qd.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "COMPUTE" :
                        qd.Type == D3D12_COMMAND_LIST_TYPE_COPY ? "COPY" : "OTHER";
                    Logging::LogFmt("[PresentHookKit] DX12 ExecuteCommandLists: new distinct queue seen: 0x%p type=%s priority=%d (total distinct queues so far: %zu)",
                        queue, typeName, qd.Priority, seenQueues.size());
                }
            }

            // FALLBACK ONLY — see OnSwapChainCreationQueueSeen above, which
            // is authoritative and always wins when it fires.
            bool isDirectQueue = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (isDirectQueue) {
                // Feeds RecordPresentForCorrelation's real-queue resolution
                // — safe/cheap to call unconditionally even once a queue is
                // already captured, since correlation runs independently
                // and can still override a wrong fallback guess.
                RecordExecuteCommandListsForCorrelation(queue);
            }
            if (!g_capturedQueue && isDirectQueue) {
                g_capturedQueue = queue;
                g_hasCapturedQueue = true;
                Logging::LogFmt("[PresentHookKit] DX12 command queue captured via fallback (ExecuteCommandLists first-caller heuristic): 0x%p type=DIRECT", queue);
            }
            return g_origExecuteCommandLists(queue, numLists, lists);
        }

        inline bool Install() {
            if (!EnsureMinHookInitialized()) return false;

            ID3D12Device* dummyDevice = nullptr;
            if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice))) || !dummyDevice)
                return false;

            D3D12_COMMAND_QUEUE_DESC qDesc = {};
            qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* dummyQueue = nullptr;
            if (FAILED(dummyDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&dummyQueue))) || !dummyQueue) {
                dummyDevice->Release();
                return false;
            }

            void** queueVtable = *reinterpret_cast<void***>(dummyQueue);
            void* execTargetFn = queueVtable[10];
            bool execOk = MH_CreateHook(execTargetFn, reinterpret_cast<void*>(&HookedExecuteCommandLists),
                reinterpret_cast<void**>(&g_origExecuteCommandLists)) == MH_OK &&
                MH_EnableHook(execTargetFn) == MH_OK;

            dummyQueue->Release();
            dummyDevice->Release();

            // No Present/ResizeBuffers hook here — DX11's vtable-swapped
            // Present is the universal frame-boundary signal for both
            // backends (see DX11::HookedPresent's fallback branch calling
            // DX12::TryInitAndRender). DX12 games still need DX11::Install()
            // to also run — see InstallWorkerThreadProc.
            g_installed = execOk;
            Logging::LogFmt("[PresentHookKit] DX12 ExecuteCommandLists hook: %s", execOk ? "OK" : "FAILED");
            return g_installed;
        }

        inline void Uninstall() {
            // Wait for the GPU to finish with anything the native path
            // might still own before releasing it — skipping this is a
            // very reliable DX12 crash-on-unload. D3D11On12's Flush() has
            // already synchronized its own per-frame work by this point.
            if (!g_usingD3D11On12 && g_fence && g_fenceEvent && g_fenceLastSignaled > 0 &&
                g_fence->GetCompletedValue() < g_fenceLastSignaled) {
                g_fence->SetEventOnCompletion(g_fenceLastSignaled, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, 2000); // bounded wait — never hang teardown indefinitely
            }

            if (g_imguiInit) {
                if (g_usingD3D11On12) ImGui_ImplDX11_Shutdown();
                else ImGui_ImplDX12_Shutdown();
                g_imguiInit = false;
            }

            if (g_commandList) { g_commandList->Release(); g_commandList = nullptr; }
            for (auto& fc : g_frameContexts) if (fc.allocator) fc.allocator->Release();
            g_frameContexts.clear();

            ReleaseBackBuffers();
            if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
            if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
            if (g_fence) { g_fence->Release(); g_fence = nullptr; }
            if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }

            if (g_d3d11on12Device) { g_d3d11on12Device->Release(); g_d3d11on12Device = nullptr; }
            if (g_d3d11Context) { g_d3d11Context->Release(); g_d3d11Context = nullptr; }
            if (g_d3d11Device) { g_d3d11Device->Release(); g_d3d11Device = nullptr; }

            if (g_device) { g_device->Release(); g_device = nullptr; }
            g_installed = false;
        }
    } // namespace DX12


    // Ported directly from current SteamSwitcher's ModInjector.cs
    // (WaitForGameWindowAsync/HasVisibleUntitledWindow/HasVisibleTitleWindow)
    // — same detection logic, same window-title-empty-vs-non-empty
    // heuristic, native C++ instead of C#. This DLL is already running
    // INSIDE the target process, so it can enumerate its own process's
    // windows directly via EnumWindows, matching by GetCurrentProcessId()
    // instead of a pid parameter.
    struct EnumWindowsCtx { DWORD targetPid; bool found; };

    inline BOOL CALLBACK EnumUntitledWindowProc(HWND hwnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<EnumWindowsCtx*>(lParam);
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == ctx->targetPid && IsWindowVisible(hwnd)) {
            char title[256] = { 0 };
            GetWindowTextA(hwnd, title, sizeof(title));
            if (title[0] == '\0') { ctx->found = true; return FALSE; }
        }
        return TRUE;
    }

    inline BOOL CALLBACK EnumTitledWindowProc(HWND hwnd, LPARAM lParam) {
        auto* ctx = reinterpret_cast<EnumWindowsCtx*>(lParam);
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == ctx->targetPid && IsWindowVisible(hwnd)) {
            char title[256] = { 0 };
            GetWindowTextA(hwnd, title, sizeof(title));
            if (title[0] != '\0') { ctx->found = true; return FALSE; }
        }
        return TRUE;
    }

    inline bool HasVisibleUntitledWindow() {
        EnumWindowsCtx ctx{ GetCurrentProcessId(), false };
        EnumWindows(&EnumUntitledWindowProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    inline bool HasVisibleTitleWindow() {
        EnumWindowsCtx ctx{ GetCurrentProcessId(), false };
        EnumWindows(&EnumTitledWindowProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.found;
    }

    // Same state machine as ModInjector.cs's WaitForGameWindowAsync: wait
    // for a splash (untitled) window, then wait for it to be replaced by
    // a titled window; if no splash ever appears, just wait for a titled
    // window directly. 1s poll interval, ~5min max wait, 20s post-splash
    // grace period — same constants as the C# original.
    inline bool WaitForSplashScreenToEnd() {
        const int kMaxWaitSeconds = 300;
        const int kPostSplashGraceSeconds = 20;
        bool splashWasSeen = false;
        int postSplashTicks = 0;

        for (int i = 0; i < kMaxWaitSeconds; ++i) {
            bool splashFound = HasVisibleUntitledWindow();
            bool mainFound = HasVisibleTitleWindow();

            if (!splashWasSeen) {
                if (splashFound) {
                    splashWasSeen = true;
                    Logging::LogFmt("[PresentHookKit] Splash screen detected. Waiting for game to finish loading...");
                }
                else if (i >= 3 && mainFound) {
                    Logging::LogFmt("[PresentHookKit] Game already running (no splash). Waiting 2s for initialization...");
                    Sleep(2000);
                    return true;
                }
            }
            else {
                if (!splashFound) {
                    if (mainFound) {
                        Logging::LogFmt("[PresentHookKit] Game fully loaded (splash gone, main window present). Waiting 2s for initialization...");
                        Sleep(2000);
                        return true;
                    }
                    else {
                        if (++postSplashTicks >= kPostSplashGraceSeconds) {
                            Logging::LogFmt("[PresentHookKit] WARNING: no game window found %ds after splash disappeared — proceeding anyway.", kPostSplashGraceSeconds);
                            return false;
                        }
                    }
                }
            }
            Sleep(1000);
        }
        Logging::LogFmt("[PresentHookKit] WARNING: splash-screen wait timed out after %ds — proceeding anyway.", kMaxWaitSeconds);
        return false;
    }

    inline void InstallWorkerThreadProc() {
        // Testing-harness splash-wait/Streamline-poll block REMOVED (was
        // here, marked "DO NOT CARRY INTO PRODUCTION" - see
        // OVERLAY-REDESIGN-RESULT.md's own explicit instruction to remove
        // this, not tune it, once wired into real SteamSwitcher-driven
        // injection). It existed only because CbtInjector.exe injects
        // immediately with no splash awareness. Under real production
        // injection, SteamSwitcher's own WaitForGameWindowAsync already
        // confirms the game is past its splash screen (and gives its own
        // 2s post-load grace) BEFORE ever calling InjectDll - by the time
        // this DLL's DllMain runs at all, that's already guaranteed. The
        // removed block was adding a redundant ~5s splash re-check plus a
        // ~15s Streamline poll/safety-buffer AFTER that, delaying
        // DX11::Install() (and therefore the first point DrawOverlay() can
        // ever run) by ~20s for no benefit under production timing.
        //
        // The actual Streamline gate (IsStreamlineLoaded() inside
        // RequestAttach()) is untouched and still runs at attach time,
        // exactly as before - only this redundant pre-install wait is gone.
        Logging::LogFmt("[PresentHookKit] Steam overlay present: %s (informational only — not skipping)",
            IsKnownConflictingOverlayLoaded() ? "yes" : "no");

        bool isDX12 = GetModuleHandleA("d3d12.dll") != nullptr;
        bool isDX11 = GetModuleHandleA("d3d11.dll") != nullptr;

        // ═══════════════════════════════════════════════════════════════
        // DX12 has two render paths, selected per-attach in DX12::LazyInit
        // based on whether Streamline is loaded (see DX12 namespace's own
        // header comment): native ImGui_ImplDX12 by default (RE3-proven,
        // unchanged — submits its own command list via
        // DX12::g_origExecuteCommandLists), or D3D11On12 only when
        // Streamline is present (submission happens inside Microsoft's own
        // D3D11on12 runtime instead, removing the direct-call collision
        // that caused the Daemon X Machina crash).
        //
        // DX12::Install()'s ExecuteCommandLists hook installs unconditionally
        // either way — it's always needed for queue capture, and the native
        // path additionally reuses it as the real submission trampoline.
        //
        // DX9 remains disabled — untested, unrelated to this DX12 work.
        //
        // DX11 remains installed unconditionally — still the only
        // confirmed-stable backend and DX12 games rely on DX11's Present
        // hook as their shared frame-boundary signal (see this file's own
        // header comment).
        // ═══════════════════════════════════════════════════════════════
        if (isDX11) DX11::Install();
        if (isDX12) DX12::Install();
        Logging::LogFmt("[PresentHookKit] Module detection: d3d12=%d d3d11=%d — DX11 always installed; DX12 re-enabled (native ImGui_ImplDX12 path, or D3D11On12 if Streamline is present — decided per-attach); DX9 still disabled.",
            (int)isDX12, (int)isDX11);
    }

    inline void InstallAll() {
        InstallWorkerThreadProc();
    }

    inline void UninstallAll() {
        // MH_Uninitialize() handles all hooks now — Present/ResizeBuffers
        // are back to MinHook-managed (see this file's own header comment
        // on why vtable-swap was reverted), same as ExecuteCommandLists.
        MH_Uninitialize();

        DX9::Uninstall();
        DX11::Uninstall();
        DX12::Uninstall(); // waits on its own fence internally — see its own comment

        Sleep(50); // residual-in-flight-call safety margin, same as the archive's version

        if (g_gameHwnd && IsWindow(g_gameHwnd) && g_originalWndProc) {
            SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        }
        if (g_imguiContextCreated) {
            if (g_win32BackendActive) {
                ImGui_ImplWin32_Shutdown();
                g_win32BackendActive = false;
            }
            ImGui::DestroyContext();
            g_imguiContextCreated = false;
        }
        HotkeyPoll::Stop();
        g_gameHwnd = nullptr;
        g_originalWndProc = nullptr;
    }

} // namespace PresentHookKit