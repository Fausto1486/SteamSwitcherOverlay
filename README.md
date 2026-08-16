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
8. [In-Game Config Windows](#in-game-config-windows)
9. [Session Header, Dim Backdrop, and Logs Button](#session-header-dim-backdrop-and-logs-button)
10. [Attach Lifecycle](#attach-lifecycle)
11. [Pipe Protocol](#pipe-protocol)
12. [Hotkey](#hotkey)
13. [Logging](#logging)
14. [Known Limitations](#known-limitations)
15. [Deployment Layout](#deployment-layout)
16. [Build Notes](#build-notes)
17. [Safety Scope](#safety-scope)

---

## Architecture

The overlay is a single DLL injected into the game process. Once loaded, it:

1. Pins itself (`GET_MODULE_HANDLE_EX_FLAG_PIN`) so it can't be unloaded out from under its own background threads.
2. Hooks the game's DXGI `Present`/`ResizeBuffers` (shared across D3D9/D3D11/D3D12 — see [Hooking Strategy](#hooking-strategy)) and, for D3D12 specifically, `ID3D12CommandQueue::ExecuteCommandLists` and `IDXGIFactory2::CreateSwapChainForHwnd`.
3. Waits for an explicit **attach** trigger (hotkey, a pipe message, or a toast) before actually creating any ImGui/device resources — hooks install early and are proven safe to leave installed, but real attach work is deliberately delayed (see [Attach Lifecycle](#attach-lifecycle)).
4. Once attached, reads ModKit's shared-memory status block every frame and renders a mods panel + toast stack via Dear ImGui.

There is no separate injector process — SteamSwitcher itself injects this DLL directly (see [Production Injection](#production-injection)).

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
| `SteamSwitcherOverlay.cpp` | Entry point. `DllMain`, module pinning, wires the hotkey/pipe callbacks to `PresentHookKit::RequestAttach()` and `Overlay::*`. |
| `PresentHookKit.h` | All D3D9/D3D11/D3D12 hooking. Installs/uninstalls hooks, drives the attach/render lifecycle, owns the DX12 queue-capture logic. The largest file in the project — see [Hooking Strategy](#hooking-strategy). |
| `OverlayContent.h` | `DrawOverlay()` — the actual pixels. Builds the per-frame `ModRow` list from `SharedDataReader`, diffs it against the previous frame to push toast notifications, and draws the status panel, toast stack, and (see [In-Game Config Windows](#in-game-config-windows)) a mod's config-window panel via ImGui. |
| `SharedDataReader.h` | Read-only access to ModKit's `ModKitSharedData_v1` named shared-memory block. Ported from SteamSwitcher's own `ModSharedStatusReadercs.cs` — same layout, same read philosophy (open fresh every read, no locking, torn reads self-correct on next resync). No `ModKit.dll` build dependency. |
| `ModKitInterop.h` | Lazy `GetProcAddress`-based access to `ModKit.dll` exports — the original four (`ModKit_HasButton`/`ClickButton`/`IsPoolSearching`/`IsPoolClearing`) plus the full config-window overlay bridge (`ModKit_ClickButtonForOverlay`, `ModKit_HasConfigWindow`, `ModKit_GetConfigWindowTitle`/`RowCount`/`Row`/`DropdownOption`, `ModKit_SetConfigWindowToggle`/`Float`/`Int`/`Dropdown`, `ModKit_CloseConfigWindowFromOverlay`/`CloseAllConfigWindows` — see [In-Game Config Windows](#in-game-config-windows)) — resolved at runtime, re-attempted every call until found, since load order between the two DLLs isn't guaranteed. `ModKitConfigRowView`/`ModKitConfigRowType` are deliberately redefined here (byte-for-byte copy of `ModKit.h`'s versions) rather than included, per this file's no-header-include/no-static-link philosophy. No static link to `ModKit.lib` at all. |
| `OverlayPipe.h` | Named-pipe server (`SteamSwitcherOverlayPipe`) — SteamSwitcher connects as client and pushes `SETMODCHANNEL|`/`TOAST|` commands. See [Pipe Protocol](#pipe-protocol). |
| `HotkeyPoll.h` | Independent `GetAsyncKeyState(VK_INSERT)` poll (50ms) to toggle the status panel. No dependency on `ModKit.dll` being loaded at all. |
| `Logging.h` / `RemoteLog.h` | `Logging::LogFmt` forwards every line to SteamSwitcher's optional debug-log pipe (`SteamSwitcherOverlayLogPipe`) via a best-effort, no-retry `CreateFileA`/`WriteFile` per call. No local log file. |
| `MinHook/` | Vendored [MinHook](https://github.com/TsudaKageyu/minhook) — the actual inline-hooking engine everything above is built on. |
| `imgui/` | Vendored [Dear ImGui](https://github.com/ocornut/imgui) with `imgui_impl_win32`/`imgui_impl_dx9`/`imgui_impl_dx11`/`imgui_impl_dx12` backends. |

---

## Render Backend Support

| Backend | Status |
|---|---|
| DX9 | **Shipping.** Confirmed working against a real DX9 game — installed unconditionally in `InstallWorkerThreadProc`, same as DX11. |
| DX11 | **Shipping.** Confirmed stable across multiple real games, including Steam overlay present and fullscreen/exclusive-fullscreen transitions. |
| DX12 | **Shipping**, on either of two render paths selected per-attach (`DX12::g_usingD3D11On12`, decided in `LazyInit`): a native `ImGui_ImplDX12` path by default, or a `D3D11On12` interop path specifically when NVIDIA Streamline (`sl.interposer.dll`) is present — see [DX12 Queue Capture](#dx12-queue-capture). |

DX9 installs the same way DX11 does — a dummy-device hook attempt at startup that harmlessly no-ops (`DX9::Install()` logs "not installed (game likely doesn't use it)") if the process isn't actually using Direct3D9. No module-detection gate needed for it, matching DX11's own unconditional install.

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

A DX12 swapchain is bound to one specific `ID3D12CommandQueue` at creation time — submitting the overlay's own render commands on the wrong queue produces a cross-queue resource-state hazard (the backbuffer's PRESENT↔RENDER_TARGET transitions have no guaranteed ordering against the real presenting queue's own barriers). Three queue-capture problems exist, each with its own fix:

1. **Wrong queue type.** Capturing whichever queue calls `ExecuteCommandLists` *first* is unsound — non-DIRECT queues (COPY/COMPUTE) exist on the same device and can win that race, and submitting DIRECT-type render commands on one corrupts swapchain state (the game's own `Present()` starts returning `DXGI_ERROR_INVALID_CALL`). **Fixed**: `HookedExecuteCommandLists` only captures a queue whose `D3D12_COMMAND_QUEUE_DESC.Type == D3D12_COMMAND_LIST_TYPE_DIRECT`.
2. **Wrong DIRECT queue.** DIRECT-type filtering alone isn't sufficient — some games have more than one DIRECT queue, and "first DIRECT queue observed" is still a guess. **Fixed properly**: `DX11::HookedCreateSwapChainForHwnd` inspects the `pDevice` parameter of every swapchain-creation call — by DXGI's own API contract, this parameter *is* the swapchain's actual presentation queue for a D3D12 swapchain, no ambiguity possible. This is now the **authoritative** capture source (`DX12::OnSwapChainCreationQueueSeen`); the original `ExecuteCommandLists`-based DIRECT-type heuristic is kept only as a fallback for the case where a game creates its swapchain before this DLL's factory hook installs.
3. **Authoritative capture can be structurally unavailable.** A game whose real swapchain is created before this DLL is injected will never trigger `HookedCreateSwapChainForHwnd` for it — only unrelated (e.g. Steam-internal) swapchain creations get observed. With multiple real DIRECT queues and no authoritative source, the fallback heuristic is a guess — the same class of bug as #2. **Fixed via Present-correlation** (`RecordExecuteCommandListsForCorrelation`/`RecordPresentForCorrelation`, both in the `DX12` namespace): every DIRECT-type queue's last `ExecuteCommandLists` timestamp is tracked; on every real `Present`, whichever queue fired most recently (within 8ms) scores a point. After ~90 frames, the highest-scoring queue is used. If no queue ever correlates, the DLL refuses to attach rather than guess (see [Known Limitations](#known-limitations)).

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

**Fixed bug: fast auto-hookers missed their own first toast.** `DiffAndPushToasts` only runs once real rendering starts (`g_imguiInit == true`), which on Streamline games can now be ~1.5s after injection (see [DX12 Queue Capture](#dx12-queue-capture)). A mod whose own `ModKit_OnInjectionComplete` callback hooks immediately — well before that first render frame — used to have its "just hooked" transition silently absorbed as the diff baseline on first sight, with no toast ever firing for that session. **Fixed**: the first-sight baseline is now always "nothing active yet" (`PrevModState`'s own default-constructed values), not the row's actual current state, so an already-hooked mod on first observation still produces a normal `false→true` transition instead of a swallowed one. Mods that start unhooked and get hooked later were never affected by this — this only cost the fast-auto-hooker case.

**ModKit-only edge case.** `ModKit.dll`'s shared-memory block is created *lazily*, inside `ModKit_Startup()` — which each individual mod calls when it registers. With zero mods (SteamSwitcher's "Force Inject ModKit" with nothing enabled), no mod ever calls it, so the block genuinely never exists, even though `ModKit.dll` itself is loaded and running. `ReadModRows()` handles this by falling back to `SharedDataReader::IsModKitPresent()` — a same-process `GetModuleHandleA("ModKit.dll")` check, not a shared-memory check — and synthesizing a non-clickable "ModKit (no mods loaded)" row so the panel doesn't silently show nothing. This row (and the equivalent one in SteamSwitcher's own toast-mode panel) disappears automatically the moment ModKit.dll actually unloads, since it's polled fresh every frame.

---

## In-Game Config Windows

Clicking a clickable row in the status panel doesn't just invoke the mod's button callback — it can also render that mod's `ModConfigWindow` (see the `ModKit` project's own README, [Config Window Overlay Bridge](../ModKit/README.md#config-window-overlay-bridge)) directly inside this overlay via ImGui, instead of a native win32 popup.

**Row click → two parallel paths, one wins per mod:**

1. `DrawStatusPanel`'s row click calls `ModKitInterop::ClickButtonForOverlay(modName)` — **not** plain `ClickButton` — so `ModKit.dll` marks the resulting `ModConfigWindow::Open()` call as an in-game overlay click specifically (see `ModKit_IsOverlayActive`'s design in the ModKit README). It also sets `g_openConfigModName = modName` and calls `TrackConfigWindowFor(modName)`.
2. **Rebuilt mod (registers with the bridge):** `Open()` sees `ModKit_IsOverlayActive() == true` and registers a `ModKitConfigWindowProvider` instead of creating a native window. `DrawConfigPanel()` then finds `ModKitInterop::HasConfigWindow(modName) == true` on the next frame and renders the mod's rows itself — this is the operative path, and `TrackConfigWindowFor`'s poll thread finds nothing (harmless).
3. **Legacy/unrebuilt mod (native window only):** `HasConfigWindow` stays `false`, so `DrawConfigPanel` no-ops. `TrackConfigWindowFor`'s background thread (`PollForConfigWindowThreadProc`, polling every 100ms for up to 2s) picks up the native window by its class name (`"CD" + modName + "CfgWnd"`) and tracks its `HWND` so `CloseTrackedConfigWindows()` can `WM_CLOSE` it when the status panel closes.

**`DrawConfigPanel()`** renders one mod's window at a time (matches `ModConfigWindow`'s own singleton-per-mod behavior), positioned immediately to the right of the status panel. It reads `ModKit_GetConfigWindowTitle`/`RowCount`/`Row` fresh every frame and switches on `ModKitConfigRowType` per row (Toggle → `ImGui::Checkbox`, Float/Int → `ImGui::InputFloat`/`InputInt` with `EnterReturnsTrue`, Dropdown → `ImGui::BeginCombo` + `ModKit_GetConfigWindowDropdownOption` per entry, Status → plain read-only text) — writes go back through `ModKit_SetConfigWindowToggle`/`Float`/`Int`/`Dropdown`. An Escape keypress or the header's "x" button calls `ModKit_CloseConfigWindowFromOverlay` and clears `g_openConfigModName`. The panel also stops drawing on its own the moment `HasConfigWindow` goes false — e.g. SteamSwitcher closing every config window on a `NotificationMode` switch, which sends `CLOSECONFIGWINDOWS` over the pipe (see [Pipe Protocol](#pipe-protocol)) and calls `ModKitInterop::CloseAllConfigWindows()` in response.

`g_openConfigModName` is deliberately **not** cleared when the status panel itself closes (INSERT) — drawing is gated on `g_statusPanelOpen` at the call site, so the config window hides and reappears in the same spot rather than resetting, mirroring the underlying `ModConfigWindow` registration surviving the panel toggle. It only clears on an explicit close or a mode switch.

**Anchor window (native fallback only).** `ModConfigWindow.h` finds where to place a *native* config window via `FindWindowA(nullptr, "ModStatusPanel")` — a real HWND in toast mode, but there's no HWND backing this ImGui-drawn panel. `EnsureAnchorWindow`/`UpdateAnchorWindow` keep an invisible, input-transparent (`WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`) HWND titled `"ModStatusPanel"` in sync with the status panel's on-screen rect every frame it's open, purely as a `FindWindowA`/`GetWindowRect` target for legacy mods; `ClearAnchorWindow` blanks the title (not destroy) when the panel closes so a stray config window opened afterward falls back to SteamSwitcher's own anchor instead of matching a stale rect. Only the render thread may touch `g_anchorHwnd` — `UpdateAnchorWindow`/`ClearAnchorWindow` calls from `HotkeyPoll`'s thread would deadlock inside `SetWindowTextA` against a message queue nothing pumps. Rebuilt mods rendered via `DrawConfigPanel` don't need this — they're positioned directly off the status panel's own ImGui rect in the same frame.

---

## Session Header, Dim Backdrop, and Logs Button

`DrawStatusPanel` (`OverlayContent.h`) grew a header block above the existing "MODS - click to configure" strip, mirroring SteamSwitcher's own `ModsStatusPanel.cs` (WinForms, Toast mode) feature-for-feature so both modes read as the same product:

- **Game name, profile/persona name, and a live session timer.** Sourced from `SessionInfo` (`Overlay::g_session`), populated by `GAMEINFO|<name>|<epochMs>|<profile>` over `OverlayPipe` — sent by `ModsPanel.cs`'s `SendGameInfoToOverlay()` right after `SETMODCHANNEL|1`, at the same three points the Toast side calls `SetSessionInfo`: injection completing, the shared auto-inject/GOG continuation, and a live mode-switch to Overlay. `launchEpochMs` of `0` is the "unknown" sentinel — the timer line just doesn't draw.
- **`DisplayProfileText`/`FormatElapsed`** are the exact same transforms `ModsStatusPanel.cs` applies — including stripping a leading `Run … on ` prefix from the profile name if present (some Steam vanity/persona names happen to read like a launcher string; splits at the *last* " on " so a genuinely-containing name is safe). Both modes show identical text for identical session data.
- **Dynamic width.** `ComputeRequiredWidth`-equivalent logic measures the game name and profile+timer line via `ImGui::CalcTextSize` and widens the panel (up to a capped `MAX_WIDTH`) if either needs more room than the base width — same reasoning as the WinForms side, same cap-then-ellipsize fallback beyond that. `DrawConfigPanel` reads back `g_lastStatusPanelWidth` (set every frame by `DrawStatusPanel`) rather than a fixed constant, so it stays correctly positioned to the status panel's right edge regardless of how wide that edge currently is.
- **A real drawn "Logs" button** (filled background, border, hover state — not just underlined text) in the header. Click sends `SHOWLOGS` via the new `OverlayCommandPipe.h` (mirrors `RemoteLog.h`'s shape: a fire-and-forget `CreateFileA`/`WriteFile` into a pipe SteamSwitcher hosts as server — `SteamSwitcherOverlayCmdPipe`, received by `OverlayCommandMonitor.cs`) to bring SteamSwitcher's Log window to the front, anchored near this panel's own on-screen position (the game window's rect, offset by the same `+22, +56` this panel places itself at).
- **A dimmed backdrop** behind the panel while it's open, via `DrawDarkBackdrop()` on `ImGui::GetBackgroundDrawList()` — mirrors `ModsStatusPanel.cs`'s own `DimBackdrop` Form, just via ImGui's background draw list instead of a second Win32 window. Background-draw-list content always renders at the very bottom of the frame regardless of call order, so toasts (`DrawToastStack`, normal ImGui windows) are unaffected by construction, not because of where `DrawDarkBackdrop()` happens to be called from in `DrawOverlay`.

Known Unicode limitation: `GAMEINFO`'s text now arrives as correct UTF-8 (an earlier bug had `OverlayPipeClient.cs` encoding pipe messages as ASCII, silently replacing every character above code point 127 with a literal `?` before the message even left SteamSwitcher — fixed by switching to `Encoding.UTF8.GetBytes`), but this project never loads a custom font or explicit Unicode glyph ranges into ImGui — it runs on ImGui's implicit default font, Basic Latin only. A persona name containing genuinely exotic glyphs (CJK, certain symbol blocks) still renders as ImGui's own missing-glyph fallback, a separate, unaddressed limitation from the encoding bug. The Toast/WinForms side has no such limitation, since it uses whatever system font Windows provides.

---

## Attach Lifecycle

Hooks install early and stay installed for the life of the process — proven safe. The actual attach work (device/context refs, ImGui context creation, backbuffer/RTV setup) is deliberately delayed until one of three explicit triggers fires `PresentHookKit::RequestAttach()`:

- The hotkey (`INSERT`, via `HotkeyPoll`)
- `SETMODCHANNEL|1` over the pipe (SteamSwitcher enables Channel 2 / mod content)
- `TOAST|<text>` over the pipe (a toast alone must be enough to bring up rendering, or it would sit queued and expire before anything is drawing)

`RequestAttach()` is idempotent — every call after the first (INSERT, `SETMODCHANNEL|1`, or a toast) is a no-op against `g_attachRequested`, so only the first ever logs or does anything. Once attach is requested, `LazyInit` (DX9/DX11) or `LazyInit` (DX12, heavier — descriptor heaps, per-backbuffer RTVs, a frame-context/fence ring for the native path, or a `D3D11On12` device wrap for the Streamline path — see [DX12 Queue Capture](#dx12-queue-capture)) runs on the next real frame, followed by `ClaimHwndAndSubclass` to intercept window input. Every attach step is SEH-wrapped; a fault permanently disables rendering for that session rather than retrying every frame (which would otherwise mean 60×/sec real GPU-adjacent resource churn on a game that's never going to succeed).

---

## Pipe Protocol

SteamSwitcher connects as client and pushes plain `KEY|value` text to `\\.\pipe\SteamSwitcherOverlayPipe` (`OverlayPipe.h`):

| Message | Effect |
|---|---|
| `SETMODCHANNEL\|1` | Enables Channel 2 (mod content) and requests attach. |
| `SETMODCHANNEL\|0` | Disables Channel 2 — closes the status panel if open, but toast rendering continues (drains naturally as toasts expire). |
| `TOAST\|<text>` | Pushes a generic, non-mod-related info toast and requests attach. |
| `CLOSECONFIGWINDOWS` | Closes every open mod config window, native or overlay-drawn alike (calls `ModKitInterop::CloseAllConfigWindows()` — see the ModKit README's [Config Window Overlay Bridge](../ModKit/README.md#config-window-overlay-bridge)). Sent the instant SteamSwitcher's `NotificationMode` dropdown changes, so a config window from the old mode never lingers into the new one — see `ModsPanel.cs`'s `Cmb_notificationMode_Changed`. |

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

- **DX12 + NVIDIA Streamline Present-correlation is probabilistic, not authoritative.** The fix (see [DX12 Queue Capture](#dx12-queue-capture)) resolves the real presentation queue by timing correlation, not by direct API observation — a real signal, not a guarantee. If correlation ever comes back inconclusive on a given game, the DLL correctly refuses to attach rather than guess (`g_queueResolvedSuccessfully == false` in `DX12::LazyInit`) — that's a missing overlay, not a crash, and is the expected/safe failure mode, not a regression to chase. `PresentHookKit.h`'s `g_refuseAttachOnStreamlineForTesting` (default `false`) instantly restores the old unconditional refuse-on-Streamline behavior if this path ever needs isolating again.
- **32-bit games are not supported.** This DLL, MinHook, and ImGui are all architecture-agnostic C/C++ in principle, but there is no 32-bit build or 32-bit-aware injection path — SteamSwitcher falls back to its toast/notification path for 32-bit games.
- **`ModKitInterop.h`'s resolved exports assume they might not exist.** The header comment describing this file was written when `ModKit_HasButton`/`ClickButton`/`IsPoolSearching`/`IsPoolClearing` were not yet exported from `ModKit.dll`. All of these, plus the full config-window overlay bridge, are exported now (see the `ModKit` project's own README — Host Integration Helpers, [Config Window Overlay Bridge](../ModKit/README.md#config-window-overlay-bridge)) — the `GetProcAddress` approach still stands (no build-time link is still the right call for two independently-versioned DLLs), but the "these don't exist yet" framing in that file's comments is stale. Every unresolved export still degrades to a safe default (non-clickable rows, no config panel), consistent with the rest of this file's design.

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

## Safety Scope

Like ModKit, this DLL is intended for personal single-player modding and debugging. Do not use it with online multiplayer games, kernel-level anti-cheat, or in any way that violates a game's terms of service.