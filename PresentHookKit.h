#pragma once
// ═══════════════════════════════════════════════════════════════════════
// PROJECT STATUS: WORK IN PROGRESS. This is NOT the final state of the
// overlay. Current SHIPPING configuration is DX11-ONLY (see
// InstallWorkerThreadProc near the bottom of this file) — DX9 and DX12
// are both fully implemented but deliberately disabled, pending real
// testing (DX9) and a real fix for a confirmed, unresolved Steam-overlay
// crash (DX12). See OVERLAY-REDESIGN-RESULT.md's "FINAL SHIPPING
// DECISION" section for the complete, current picture before assuming
// anything about this file's scope or trustworthiness beyond DX11.
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

    inline void RequestAttach() {
        if (IsStreamlineLoaded()) {
            Logging::LogFmt("[PresentHookKit] NVIDIA Streamline (sl.interposer.dll) detected — refusing to attach overlay. Known crash risk, cause not yet understood. See OVERLAY-REDESIGN-RESULT.md.");
            return;
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

    // DX12 — DISABLED AGAIN, NOT DELETED. See InstallWorkerThreadProc near
    // the bottom of this file: DX12::Install() is commented out. Full
    // history (see OVERLAY-REDESIGN-RESULT.md for the complete version):
    // (1) disabled after Daemon X Machina/Streamline crashed; (2) traced
    // to a Streamline-detection filename typo (checking "sl_interposer.dll"
    // when the real file is "sl.interposer.dll"), fixed, briefly
    // RE-enabled believing the Streamline gate alone made DX12 safe;
    // (3) DIRECTLY DISPROVEN — RE3's DX12 mode (confirmed via module dump:
    // NO Streamline present at all) crashed against STEAM's own overlay
    // (gameoverlayrenderer64.dll, 0xC0000005) a few seconds into ordinary
    // rendering, no special trigger. This is a genuinely different,
    // previously-untested collision: every prior DX12 test had either the
    // Streamline gate blocking before attach, or render submission
    // deliberately skipped — this was the first real end-to-end DX12
    // render test with Steam overlay present, and it crashed. The
    // Streamline gate does nothing for this, since Streamline was never
    // involved. DX12 rendering itself has a real, unresolved Steam-overlay
    // bug independent of everything else investigated this session.
    //
    // Forward declaration — DX11::HookedPresent falls through to this when
    // its own D3D11 device acquisition fails, per this file's own header
    // comment on the DX12 architecture decision.
    namespace DX12 {
        inline void TryInitAndRender(IDXGISwapChain* swapChain);
        extern volatile bool g_hasCapturedQueue;

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
            // Heartbeat diagnostic (throttled, once/sec) previously lived
            // here — used to prove Present was still calling us at all
            // during the Daemon X Machina/DX12 investigation. Removed:
            // its purpose is moot now that DX12 is disabled in production
            // (see the DX12 namespace's own DISABLED marker), and leaving
            // it active would spam this DLL's log file once/sec forever
            // during normal DX11 (RE3-style) production use, for zero
            // benefit there. Re-add if DX12 debugging resumes — the
            // pattern was: log swapChain/flags/g_imguiInit/g_mainRTV/
            // g_confirmedNotD3D11/g_renderDisabledAfterFault, throttled to
            // 1/sec via a static last-logged timestamp.

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
        inline bool g_attachPermanentlyAborted = false; // set once ClaimHwndAndSubclass refuses — see DX9's identical flag. CRITICAL here specifically: without this, every frame rebuilds the FULL DX12 state (descriptor heaps, command allocators/list, fence, ImGui backend) from scratch, fails, tears it all down via Uninstall(), and repeats — 60x/sec of real GPU-adjacent resource churn, not just a log spam annoyance.
        inline ID3D12Device* g_device = nullptr; // borrowed from the swapchain, never Release()'d beyond our own AddRef

        struct FrameContext { ID3D12CommandAllocator* allocator = nullptr; UINT64 fenceValue = 0; };
        inline std::vector<FrameContext> g_frameContexts;
        inline std::vector<ID3D12Resource*> g_backBuffers;
        inline ID3D12DescriptorHeap* g_rtvHeap = nullptr;
        inline UINT g_rtvDescriptorSize = 0;
        inline ID3D12DescriptorHeap* g_srvHeap = nullptr;
        inline ID3D12GraphicsCommandList* g_commandList = nullptr;
        inline ID3D12Fence* g_fence = nullptr;
        inline UINT64 g_fenceLastSignaled = 0;
        inline HANDLE g_fenceEvent = nullptr;
        inline UINT g_bufferCount = 0;

        // ── Minimal free-list SRV descriptor allocator for ImGui_ImplDX12's
        // dynamic-texture callbacks (font atlas + any runtime textures the
        // 1.92.x backend allocates — see handover doc §4's note on this). ────
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

        inline void ReleaseBackBuffers() {
            for (auto* b : g_backBuffers) if (b) b->Release();
            g_backBuffers.clear();
        }

        inline bool CreateBackBuffersAndRTVs(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC& desc) {
            ReleaseBackBuffers();
            g_bufferCount = desc.BufferCount;
            g_backBuffers.resize(g_bufferCount, nullptr);

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
        // file's header comment. `swapChain` here is the REAL game
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

            if (!CreateBackBuffersAndRTVs(swapChain, desc)) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: CreateBackBuffersAndRTVs failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                Uninstall();
                return;
            }

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

            EnsureImGuiContext();
            if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: ImGui_ImplWin32_Init failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                Uninstall();
                return;
            }
            g_win32BackendActive = true;

            ImGui_ImplDX12_InitInfo initInfo = {};
            initInfo.Device = g_device;
            initInfo.CommandQueue = g_capturedQueue;
            initInfo.NumFramesInFlight = static_cast<int>(g_bufferCount);
            initInfo.RTVFormat = desc.BufferDesc.Format;
            initInfo.SrvDescriptorHeap = g_srvHeap;
            initInfo.SrvDescriptorAllocFn = &SrvAlloc;
            initInfo.SrvDescriptorFreeFn = &SrvFree;
            if (!ImGui_ImplDX12_Init(&initInfo)) {
                Logging::LogFmt("[PresentHookKit] DX12 LazyInit: ImGui_ImplDX12_Init failed — aborting permanently, not retrying every frame.");
                g_attachPermanentlyAborted = true;
                ImGui_ImplWin32_Shutdown(); g_win32BackendActive = false;
                Uninstall();
                return;
            }

            if (!ClaimHwndAndSubclass(desc.OutputWindow)) {
                Logging::LogFmt("[PresentHookKit] DX12 attach aborted — ClaimHwndAndSubclass refused. Not retrying this session.");
                g_attachPermanentlyAborted = true;
                Uninstall(); // full teardown — descriptor heaps, command list, allocators, fence, backbuffers, device, ImGui backends
                return;
            }
            g_imguiInit = true;
            DX11::EnsureResizeHook(*reinterpret_cast<void***>(swapChain));
            Logging::LogFmt("[PresentHookKit] Overlay attached via D3D12 (ExecuteCommandLists + DX11-Present frame signal).");
        }

        inline bool TryLazyInit(IDXGISwapChain* swapChain) {
            __try { LazyInit(swapChain); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // DIAGNOSTIC ONLY — set true to isolate whether a crash is caused by
        // capturing the queue/hooking ExecuteCommandLists at all, versus
        // specifically by submitting our own command list on it.
        inline bool g_diagnosticSkipRenderSubmission = false; // reset to real default — the Streamline gate (RequestAttach) now correctly blocks Streamline games before this point is ever reached, so any DX12 game that DOES get here should render normally

        inline void RenderFrameUnsafe(IDXGISwapChain* swapChain) {
            // GetCurrentBackBufferIndex is IDXGISwapChain3+ only.
            IDXGISwapChain3* sc3 = nullptr;
            if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return;
            UINT backBufferIndex = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
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

            if (g_diagnosticSkipRenderSubmission) {
                g_commandList->Close();
                return;
            }

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
            if (g_backBuffers.empty() && !g_rtvHeap) return; // never attached, or already cleaned up — cheap no-op

            // Wait for the GPU to actually finish with these buffers, not
            // just drop our CPU-side references — releasing while the GPU
            // still has in-flight work reading/writing them is a separate,
            // worse problem than the one this function exists to fix.
            if (g_fence && g_fenceEvent && g_fenceLastSignaled > 0 &&
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

        inline HRESULT STDMETHODCALLTYPE HookedExecuteCommandLists(
            ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists)
        {
            if (!g_capturedQueue) {
                g_capturedQueue = queue;
                g_hasCapturedQueue = true;
                Logging::LogFmt("[PresentHookKit] DX12 command queue captured.");
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
            // Wait for the GPU to finish with anything we might still own
            // before releasing it — per handover doc §6, skipping this is
            // a very reliable DX12 crash-on-unload, more so than DX9/DX11's
            // simpler (bounded-risk-but-not-eliminated) teardown.
            if (g_fence && g_fenceEvent && g_fenceLastSignaled > 0 &&
                g_fence->GetCompletedValue() < g_fenceLastSignaled) {
                g_fence->SetEventOnCompletion(g_fenceLastSignaled, g_fenceEvent);
                WaitForSingleObject(g_fenceEvent, 2000); // bounded wait — never hang teardown indefinitely
            }

            if (g_imguiInit) { ImGui_ImplDX12_Shutdown(); g_imguiInit = false; }
            if (g_commandList) { g_commandList->Release(); g_commandList = nullptr; }
            for (auto& fc : g_frameContexts) if (fc.allocator) fc.allocator->Release();
            g_frameContexts.clear();
            ReleaseBackBuffers();
            if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
            if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
            if (g_fence) { g_fence->Release(); g_fence = nullptr; }
            if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
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
        // SHIPPING CONFIGURATION: DX11 ONLY.
        //
        // DX9::Install() and DX12::Install() are both intentionally never
        // called below — not deleted, both namespaces remain fully
        // functional (see their own DISABLED markers earlier in this
        // file). Neither has passed a real, trustworthy production test:
        //
        //   DX9  — never confirmed firing on any real DX9 game this
        //          entire session. Not known broken, just genuinely
        //          untested. Needs a real DX9 game test before shipping.
        //
        //   DX12 — actively confirmed broken, twice, for two SEPARATE
        //          reasons: (1) NVIDIA Streamline present (Daemon X
        //          Machina) — mitigated via RequestAttach()'s Streamline
        //          gate, but the underlying crash cause is still not
        //          understood, just avoided; (2) Steam overlay present,
        //          NO Streamline involved (RE3's DX12 mode) — completely
        //          unmitigated, real render submission crashes reliably.
        //          See OVERLAY-REDESIGN-RESULT.md for the full history.
        //          Needs its actual root cause fixed — not just gated
        //          around — before it can ship.
        //
        // DX11 is the only backend confirmed working repeatedly, across
        // multiple real games, including Steam overlay present and
        // fullscreen transitions. Ship this configuration until DX9 and
        // DX12 each get real, passing tests of their own.
        // ═══════════════════════════════════════════════════════════════
        if (isDX11) DX11::Install();
        Logging::LogFmt("[PresentHookKit] Module detection: d3d12=%d d3d11=%d — DX11-only shipping configuration (DX9/DX12 both disabled pending real tests — see comment above).", (int)isDX12, (int)isDX11);
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