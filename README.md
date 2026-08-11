# SteamSwitcherOverlay

`SteamSwitcherOverlay.dll` is an in-game ImGui overlay for SteamSwitcher — it renders a mods status panel, per-mod toast notifications, and a trampoline-pool indicator directly inside the target game's own window, by hooking its D3D9/D3D11/D3D12 present path.

It is not a standalone tool. It's designed to be injected by SteamSwitcher itself, alongside `ModKit.dll`, and reads ModKit's shared-memory status block to know what to draw. It has no build-time dependency on `ModKit.dll` — everything it needs from ModKit is resolved either via a shared-memory block (`SharedDataReader.h`) or `GetProcAddress` (`ModKitInterop.h`), so the two DLLs can be built, versioned, and shipped independently.

---

## Table of Contents

1. [Architecture](#architecture)
2. [Production Injection](#production-injection)
3. [Project Layers](#project-layers)
4. [Render Backend Support](#render-backend-support)
5. [Hooking Strategy](#hooking-strategy)
6. [DX12 Queue Capture](#dx12-queue-capture)
7. [Data Flow: ModKit → Overlay](#data-flow-modkit--overlay)
8. [Attach Lifecycle](#attach-lifecycle)
9. [Pipe Protocol](#pipe-protocol)
10. [Hotkey](#hotkey)
11. [Logging](#logging)
12. [Known Limitations](#known-limitations)
13. [Deployment Layout](#deployment-layout)
14. [Build Notes](#build-notes)
15. [Testing](#testing)
16. [Safety Scope](#safety-scope)

---

## Architecture

The overlay is a single DLL injected into the game process. Once loaded, it:

1. Pins itself (`GET_MODULE_HANDLE_EX_FLAG_PIN`) so it can't be unloaded out from under its own background threads.
2. Hooks the game's DXGI `Present`/`ResizeBuffers` (shared across D3D9/D3D11/D3D12 — see [Hooking Strategy](#hooking-strategy)) and, for D3D12 specifically, `ID3D12CommandQueue::ExecuteCommandLists` and `IDXGIFactory2::CreateSwapChainForHwnd`.
3. Waits for an explicit **attach** trigger (hotkey, a pipe message, or a toast) before actually creating any ImGui/device resources — hooks install early and are proven safe to leave installed, but real attach work is deliberately delayed (see [Attach Lifecycle](#attach-lifecycle)).
4. Once attached, reads ModKit's shared-memory status block every frame and renders a mods panel + toast stack via Dear ImGui.

There is no separate injector process. `CbtInjector.exe`, referenced in code comments and this project's own history, is a standalone **testing** tool only — see [Testing](#testing).

---

## Production Injection

SteamSwitcher injects this DLL the exact same way it injects `ModKit.dll` — `CreateRemoteThread` + `LoadLibraryA` (`ModInjector.cs`'s `InjectDll`), after `WaitForGameWindowAsync` has already confirmed the game is past its splash screen. Under this mechanism `DllMain` is guaranteed by the OS to run in the correct process — there is no "wrong process" risk to guard against, and no PID-verification logic exists anywhere in this DLL's production path.

`DllMain(DLL_PROCESS_ATTACH)`:
1. Pins the module.
2. Calls `DisableThreadLibraryCalls`.
3. Spawns `StartupWorkerThreadProc` on a background thread (never does real work inline in `DllMain` — loader-lock safety, consistent across this whole project).

`StartupWorkerThreadProc` starts `HotkeyPoll`, `OverlayPipe`, then calls `PresentHookKit::InstallAll()`.

`DllMain(DLL_PROCESS_DETACH)` stops the pipe server and calls `PresentHookKit::UninstallAll()`.

---

## Project Layers

| File | Purpose |
|---|---|
| `SteamSwitcherOverlay.cpp` | Entry point. `DllMain`, module pinning, wires the hotkey/pipe callbacks to `PresentHookKit::RequestAttach()` and `Overlay::*`. Exports `CBTProc` for the standalone test harness only — never called in production. |
| `PresentHookKit.h` | All D3D9/D3D11/D3D12 hooking. Installs/uninstalls hooks, drives the attach/render lifecycle, owns the DX12 queue-capture logic. The largest file in the project — see [Hooking Strategy](#hooking-strategy). |
| `OverlayContent.h` | `DrawOverlay()` — the actual pixels. Builds the per-frame `ModRow` list from `SharedDataReader`, diffs it against the previous frame to push toast notifications, and draws the status panel + toast stack via ImGui. |
| `SharedDataReader.h` | Read-only access to ModKit's `ModKitSharedData_v1` named shared-memory block. Ported from SteamSwitcher's own `ModSharedStatusReadercs.cs` — same layout, same read philosophy (open fresh every read, no locking, torn reads self-correct on next resync). No `ModKit.dll` build dependency. |
| `ModKitInterop.h` | Lazy `GetProcAddress`-based access to a handful of `ModKit.dll` exports (`ModKit_HasButton`/`ClickButton`/`IsPoolSearching`/`IsPoolClearing`) — resolved at runtime, re-attempted every call until found, since load order between the two DLLs isn't guaranteed. No static link to `ModKit.lib` at all. |
| `OverlayPipe.h` | Named-pipe server (`SteamSwitcherOverlayPipe`) — SteamSwitcher connects as client and pushes `SETMODCHANNEL|`/`TOAST|` commands. See [Pipe Protocol](#pipe-protocol). |
| `HotkeyPoll.h` | Independent `GetAsyncKeyState(VK_INSERT)` poll (50ms) to toggle the status panel. No dependency on `ModKit.dll` being loaded at all. |
| `Logging.h` / `RemoteLog.h` | `Logging::LogFmt` forwards every line to SteamSwitcher's optional debug-log pipe (`SteamSwitcherOverlayLogPipe`) via a best-effort, no-retry `CreateFileA`/`WriteFile` per call. No local log file. |
| `MinHook/` | Vendored [MinHook](https://github.com/TsudaKageyu/minhook) — the actual inline-hooking engine everything above is built on. |
| `imgui/` | Vendored [Dear ImGui](https://github.com/ocornut/imgui) with `imgui_impl_win32`/`imgui_impl_dx9`/`imgui_impl_dx11`/`imgui_impl_dx12` backends. |

---

## Render Backend Support

| Backend | Status |
|---|---|
| DX9 | Implemented, fully functional per code, but **never confirmed against a real DX9 game**. Disabled in `InstallWorkerThreadProc`. Not known broken — genuinely untested. |
| DX11 | **Shipping.** Confirmed stable across multiple real games, including Steam overlay present and fullscreen/exclusive-fullscreen transitions. |
| DX12 | Working against Steam-overlay-only scenarios after a real, confirmed fix (see [DX12 Queue Capture](#dx12-queue-capture)). Still gated off for games with NVIDIA Streamline (`sl.interposer.dll`) present — a separate, unresolved crash. See [Known Limitations](#known-limitations). |

DX9 and DX12 are both fully implemented and left in the code (not deleted) behind a shipping-configuration gate in `PresentHookKit::InstallWorkerThreadProc` — re-enabling either requires its own real, repeated test pass before being trusted, not just flipping the gate.

---

## Hooking Strategy

Every hook in this DLL is installed via MinHook, which patches the **target function's own machine code** — not a per-instance vtable slot. This has a load-bearing consequence used throughout the codebase: a hook installed once, via a throwaway dummy device/swapchain, intercepts calls from **every** real instance of that function in the process, DX9/DX11/DX12 games alike.

Concretely:

- **`IDXGISwapChain::Present`** (vtable slot 8) and **`ResizeBuffers`** (slot 13) are hooked via a dummy D3D11 device + flip-model swapchain created in `DX11::Install()`. This one hook then fires for the real game's `Present` call regardless of whether the game is actually DX9, DX11, or DX12-backed — DX11's `HookedPresent` is the **universal per-frame boundary signal** for all three backends. DX12 games don't get their own `Present` hook at all; every real test showed DX11's dummy-swapchain hook claiming the shared DXGI `Present` function first, before `DX12::Install()` even runs.
- **`ID3D12CommandQueue::ExecuteCommandLists`** is hooked via a dummy D3D12 device + command queue in `DX12::Install()`. Fires for every queue's `ExecuteCommandLists` call in the process, not just the swapchain's real presentation queue — see [DX12 Queue Capture](#dx12-queue-capture) for why that matters.
- **`IDXGIFactory2::CreateSwapChainForHwnd`** (vtable slot 15) is hooked from within `DX11::Install()` too, purely to observe swapchain creation — see below.

`DX11::HookedPresent` always attempts its own DX11 attach/render first. On a real DX12 game, `swapChain->GetDevice(ID3D11Device)` simply fails (clean, expected, not an error), and the function falls through to `DX12::TryInitAndRender()` once a command queue has been captured.

---

## DX12 Queue Capture

This is the single trickiest part of the codebase and worth understanding before touching `PresentHookKit.h`'s DX12 namespace.

A DX12 swapchain is bound to one specific `ID3D12CommandQueue` at creation time — submitting the overlay's own render commands on the wrong queue produces a cross-queue resource-state hazard (the backbuffer's PRESENT↔RENDER_TARGET transitions have no guaranteed ordering against the real presenting queue's own barriers). Two real bugs were found and fixed here, confirmed via repeated testing against RE3 (RE Engine, 3 queues: COPY/DIRECT/COMPUTE) and Daemon X Machina (UE5, 2 DIRECT-priority queues):

1. **Wrong queue type.** The original approach captured whichever queue called `ExecuteCommandLists` *first*. On RE3 that was the COPY queue — submitting DIRECT-type render commands there corrupted swapchain state and made the game's own `Present()` return `DXGI_ERROR_INVALID_CALL`. **Fixed**: `HookedExecuteCommandLists` now only captures a queue whose `D3D12_COMMAND_QUEUE_DESC.Type == D3D12_COMMAND_LIST_TYPE_DIRECT`.
2. **Wrong DIRECT queue.** DIRECT-type filtering alone isn't sufficient — some games (Daemon X Machina) have more than one DIRECT queue, and "first DIRECT queue observed" is still a guess. **Fixed properly**: `DX11::HookedCreateSwapChainForHwnd` inspects the `pDevice` parameter of every swapchain-creation call — by DXGI's own API contract, this parameter *is* the swapchain's actual presentation queue for a D3D12 swapchain, no ambiguity possible. This is now the **authoritative** capture source (`DX12::OnSwapChainCreationQueueSeen`); the original `ExecuteCommandLists`-based DIRECT-type heuristic is kept only as a fallback for the (expected-rare) case where a game creates its swapchain before this DLL's factory hook installs.

Every distinct D3D12 queue seen is logged once per session (type, priority, pointer) — cheap, and keeps every future log self-documenting without needing a special diagnostic build to re-derive which queue got captured and why.

---

## Data Flow: ModKit → Overlay

```
ModKit.dll (per-mod)          ModKitSharedData_v1          SteamSwitcherOverlay.dll
  ModKit_Startup()      ──►    named shared memory    ◄──   SharedDataReader::ReadAllModStatus()
  ModKit_NotifyHooked()        (no ModKit.dll link          called once per frame from
  ModKit_SetSharedData()        required to read it)         Overlay::ReadModRows()
```

`ReadModRows()` builds one `ModRow` per mod ModKit is currently reporting (hooked state, outdated, busy, active sub-flags/features, clickable-ness resolved via `ModKitInterop::HasButton`). `DiffAndPushToasts()` compares this frame's rows against the previous frame's snapshot and pushes a toast for every hooked/outdated/feature-active transition — this is why toggling a mod produces an "ON"/"OFF" toast without any explicit push from ModKit itself; the overlay derives it purely from state deltas.

**ModKit-only edge case.** `ModKit.dll`'s shared-memory block is created *lazily*, inside `ModKit_Startup()` — which each individual mod calls when it registers. With zero mods (SteamSwitcher's "Force Inject ModKit" with nothing enabled), no mod ever calls it, so the block genuinely never exists, even though `ModKit.dll` itself is loaded and running. `ReadModRows()` handles this by falling back to `SharedDataReader::IsModKitPresent()` — a same-process `GetModuleHandleA("ModKit.dll")` check, not a shared-memory check — and synthesizing a non-clickable "ModKit (no mods loaded)" row so the panel doesn't silently show nothing. This row (and the equivalent one in SteamSwitcher's own toast-mode panel) disappears automatically the moment ModKit.dll actually unloads, since it's polled fresh every frame.

---

## Attach Lifecycle

Hooks install early and stay installed for the life of the process — proven safe. The actual attach work (device/context refs, ImGui context creation, backbuffer/RTV setup) is deliberately delayed until one of three explicit triggers fires `PresentHookKit::RequestAttach()`:

- The hotkey (`INSERT`, via `HotkeyPoll`)
- `SETMODCHANNEL|1` over the pipe (SteamSwitcher enables Channel 2 / mod content)
- `TOAST|<text>` over the pipe (a toast alone must be enough to bring up rendering, or it would sit queued and expire before anything is drawing)

`RequestAttach()` also gates on `IsStreamlineLoaded()` (`sl.interposer.dll` presence) — see [Known Limitations](#known-limitations). Once attach is requested, `LazyInit` (DX9/DX11) or `LazyInit` (DX12, heavier — descriptor heaps, per-backbuffer RTVs, a frame-context/fence ring) runs on the next real frame, followed by `ClaimHwndAndSubclass` to intercept window input. Every attach step is SEH-wrapped; a fault permanently disables rendering for that session rather than retrying every frame (which would otherwise mean 60×/sec real GPU-adjacent resource churn on a game that's never going to succeed).

---

## Pipe Protocol

SteamSwitcher connects as client and pushes plain `KEY|value` text to `\\.\pipe\SteamSwitcherOverlayPipe` (`OverlayPipe.h`):

| Message | Effect |
|---|---|
| `SETMODCHANNEL\|1` | Enables Channel 2 (mod content) and requests attach. |
| `SETMODCHANNEL\|0` | Disables Channel 2 — closes the status panel if open, but toast rendering continues (drains naturally as toasts expire). |
| `TOAST\|<text>` | Pushes a generic, non-mod-related info toast and requests attach. |

This DLL never writes back over this pipe — logging goes out over the separate `SteamSwitcherOverlayLogPipe` instead (see [Logging](#logging)).

---

## Hotkey

Bare `INSERT` toggles the status panel — no modifier. This deliberately matches SteamSwitcher's existing toast-mode convention rather than the archive version's `RCTRL+INSERT`. Tradeoff worth knowing: bare `INSERT` is a fairly common in-game keybind (quicksave/quickload in a number of older/action titles) — if a real collision with a specific game's own keybind ever turns up, this is the choice that caused it.

`ModKit.dll`'s own mod hotkeys always require **RCTRL held simultaneously** (enforced centrally in ModKit's `HotkeyThread`), so there's no collision between a mod's own toggle and this DLL's panel toggle even though both technically listen for key state.

---

## Logging

`Logging::LogFmt` is the only logging entry point in this DLL. It never writes a local file — every line goes out over `\\.\pipe\SteamSwitcherOverlayLogPipe` as a best-effort, one-shot `CreateFileA`/`WriteFile`/`CloseHandle` per call (`RemoteLog::Send`). If nothing is listening (SteamSwitcher not running, or its "Show debug logs" toggle off), the call fails instantly and the line is silently dropped — cheap, and correct, since a dropped debug line has no functional consequence.

Under heavy concurrent load (e.g. several D3D12 queues on different engine threads calling into a hooked function within the same millisecond), it's possible for two `Send()` calls to race and one to lose — a log line can go missing even when the event it describes definitely happened. This is a known, accepted tradeoff of the best-effort design, not a bug to chase.

---

## Known Limitations

- **DX9 is untested.** Implemented, never confirmed against a real DX9 game.
- **DX12 + NVIDIA Streamline is unresolved and actively gated off.** `RequestAttach()` refuses to attach whenever `sl.interposer.dll` is loaded. This is *not* the same bug as the queue-capture issue described above — real testing confirmed a correctly-captured DIRECT queue, and a clean 18+ second attach-only survival, but real command-list submission still crashes within about a second (`GPU Crash dump Triggered`, `sl_interposer` in the stack) — an async GPU/driver-side fault, not a CPU exception this DLL's SEH wrapping can catch. Most likely cause: `sl.interposer.dll` hooks `Present`/`ExecuteCommandLists` itself, and this DLL's own MinHook patches are chaining with Streamline's interception in a way that doesn't occur without it present. The D3D12 debug validation layer can't help diagnose it either — by the time this DLL is late-injected, the game's own device already exists, undebugged. Do not bypass this gate without new diagnostic evidence.
- **32-bit games are not supported.** This DLL, MinHook, and ImGui are all architecture-agnostic C/C++ in principle, but there is no 32-bit build or 32-bit-aware injection path — SteamSwitcher falls back to its toast/notification path for 32-bit games.
- **`ModKitInterop.h`'s four resolved exports assume they might not exist.** The header comment describing this file was written when `ModKit_HasButton`/`ClickButton`/`IsPoolSearching`/`IsPoolClearing` were not yet exported from `ModKit.dll`. They are exported now (see the `ModKit` project's own README, Host Integration Helpers) — the `GetProcAddress` approach still stands (no build-time link is still the right call for two independently-versioned DLLs), but the "these don't exist yet" framing in that file's comments is stale.

---

## Deployment Layout

```
SteamSwitcher/
  SteamSwitcher.exe
  Data/
    Mods/
      ModKit.dll               ← must be injected first
      SteamSwitcherOverlay.dll ← injected if NotificationMode is Overlay
      MyMod.dll
```

The overlay DLL is only injected when SteamSwitcher's `NotificationMode` for the game is set to `Overlay` (as opposed to `Toast`, which uses SteamSwitcher's own floating WinForms panel instead — see the SteamSwitcher project itself). Switching to Overlay mode live, mid-session, is handled by `ModInjector.cs`'s `InjectOverlayIntoRunningGame` — no-ops straight to `SETMODCHANNEL|1` if this DLL is already loaded, injects it first otherwise.

---

## Build Notes

- Platform: Windows x64
- Language: C++20
- Toolchain: MSVC / Visual Studio, toolset `v145` (override to an installed version, e.g. `v143`, if needed)
- Output: `SteamSwitcherOverlay.dll` (DynamicLibrary)
- The project's post-build step copies the built DLL directly into SteamSwitcher's `Data\Mods` folder — update the hardcoded path in the `.vcxproj` if your SteamSwitcher checkout lives somewhere else.

```powershell
MSBuild.exe SteamSwitcherOverlay.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
```

---

## Testing

**`CbtInjector.exe`** (referenced in code comments, not part of this repo) is a standalone `SetWindowsHookEx(WH_CBT)`-based test harness used during this project's development to validate hooking/rendering in isolation, without needing a full SteamSwitcher session. It resolves this DLL's exported `CBTProc` by name — that export exists solely to keep this test path usable and has zero role in, and zero effect on, production injection. CBT-hook injection has a real "wrong process" risk (thread-ID reuse can fire a hook in an unrelated process) that production injection, via SteamSwitcher's own `CreateRemoteThread`/`LoadLibraryA`, does not share.

Testing this way still exercises the real production DLL and hooking logic — CbtInjector only changes *how* the DLL gets loaded, not what it does once loaded. A companion tool, `SendOverlayMessage.exe` (also not part of this repo), sends a single `SETMODCHANNEL|`/`TOAST|` message to this DLL's pipe and exits, for testing those without a running SteamSwitcher.

For any DX12-specific test session, `PresentHookKit.h` has two module-level flags worth knowing about — both should read `false` outside an active test:

- `g_bypassStreamlineGateForTesting` — bypasses the Streamline gate in `RequestAttach()` to recreate the exact crash condition described in [Known Limitations](#known-limitations).
- `g_diagnosticSkipRenderSubmission` (DX12 namespace) — builds the per-frame command list but never calls `ExecuteCommandLists` on it, isolating whether a crash comes from attach/heap setup versus real render submission. This flag is global across every DX12 game in a session, not per-game.

---

## Safety Scope

Like ModKit, this DLL is intended for personal single-player modding and debugging. Do not use it with online multiplayer games, kernel-level anti-cheat, or in any way that violates a game's terms of service.