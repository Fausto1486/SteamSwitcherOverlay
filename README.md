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
9. [In-Game Stats Windows](#in-game-stats-windows)
10. [Session Header, Dim Backdrop, and Logs Button](#session-header-dim-backdrop-and-logs-button)
11. [Attach Lifecycle](#attach-lifecycle)
12. [Pipe Protocol](#pipe-protocol)
13. [Hotkey](#hotkey)
14. [Logging](#logging)
15. [Known Limitations](#known-limitations)
16. [Deployment Layout](#deployment-layout)
17. [Build Notes](#build-notes)
18. [Safety Scope](#safety-scope)

---

## Architecture

The overlay is a single DLL injected into the game process. Once loaded, it:

1. Pins itself (`GET_MODULE_HANDLE_EX_FLAG_PIN`) so it can't be unloaded out from under its own background threads.
2. Hooks the game's DXGI `Present`/`ResizeBuffers` (shared across D3D11/D3D12, plus a separate `IDirect3DDevice9::EndScene` hook for classic D3D9, which never goes through DXGI — see [Hooking Strategy](#hooking-strategy)) and, for D3D12 specifically, `ID3D12CommandQueue::ExecuteCommandLists` and `IDXGIFactory2::CreateSwapChainForHwnd`.
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
| `OverlayContent.h` | `DrawOverlay()` — the actual pixels. Builds the per-frame `ModRow` list from `SharedDataReader`, diffs it against the previous frame to push toast notifications, and draws the status panel, toast stack, a mod's config-window panel (see [In-Game Config Windows](#in-game-config-windows)), and a mod's stats-window panel (see [In-Game Stats Windows](#in-game-stats-windows)) via ImGui. |
| `SharedDataReader.h` | Read-only access to ModKit's `ModKitSharedData_v1` named shared-memory block. Ported from SteamSwitcher's own `ModSharedStatusReadercs.cs` — same layout, same read philosophy (open fresh every read, no locking, torn reads self-correct on next resync). No `ModKit.dll` build dependency. |
| `ModKitInterop.h` | Lazy `GetProcAddress`-based access to `ModKit.dll` exports — the original four (`ModKit_HasButton`/`ClickButton`/`IsPoolSearching`/`IsPoolClearing`), the config-window overlay bridge (`ModKit_ClickButtonForOverlay`, `ModKit_HasConfigWindow`, `ModKit_GetConfigWindowTitle`/`RowCount`/`Row`/`DropdownOption`, `ModKit_SetConfigWindowToggle`/`Float`/`Int`/`Dropdown`, `ModKit_CloseConfigWindowFromOverlay`/`CloseAllConfigWindows` — see [In-Game Config Windows](#in-game-config-windows)), and the stats-window overlay bridge (`ModKit_HasStatsWindow`, `ModKit_GetStatsWindowTitle`/`RowCount`/`Row`/`DropdownOption`, `ModKit_ClickStatsWindowButton`, `ModKit_ToggleStatsWindowCheckbox`, `ModKit_ChangeStatsWindowEdit`, `ModKit_SelectStatsWindowDropdown`, `ModKit_CloseStatsWindowFromOverlay`/`CloseAllStatsWindows` — see [In-Game Stats Windows](#in-game-stats-windows)) — resolved at runtime, re-attempted every call until found, since load order between the two DLLs isn't guaranteed. `ModKitConfigRowView`/`ModKitConfigRowType` and `ModKitStatsRowView`/`ModKitStatsRowType` are deliberately redefined here (byte-for-byte copies of `ModKit.h`'s versions) rather than included, per this file's no-header-include/no-static-link philosophy. No static link to `ModKit.lib` at all. |
| `OverlayPipe.h` | Named-pipe server (`SteamSwitcherOverlayPipe`) — SteamSwitcher connects as client and pushes `SETMODCHANNEL|`/`TOAST|` commands. See [Pipe Protocol](#pipe-protocol). |
| `HotkeyPoll.h` | Independent `GetAsyncKeyState(VK_INSERT)` poll (50ms) to toggle the status panel. No dependency on `ModKit.dll` being loaded at all. |
| `Logging.h` / `RemoteLog.h` | `Logging::LogFmt` forwards every line to SteamSwitcher's optional debug-log pipe (`SteamSwitcherOverlayLogPipe`) via a best-effort, no-retry `CreateFileA`/`WriteFile` per call. No local log file. |
| `MinHook/` | Vendored [MinHook](https://github.com/TsudaKageyu/minhook) — the actual inline-hooking engine everything above is built on. |
| `imgui/` | Vendored [Dear ImGui](https://github.com/ocornut/imgui) with `imgui_impl_win32`/`imgui_impl_dx9`/`imgui_impl_dx11`/`imgui_impl_dx12` backends. |
| `D3D8to9/` | A separate, self-contained C++ project (own `.vcxproj`, own output — `d3d8.dll`, not `SteamSwitcherOverlay32.dll`) that converts a D3D8 game's real calls to D3D9, so `DX9::Install()` above picks it up with zero D3D8-specific code in this DLL at all. Referenced from `SteamSwitcherOverlay.vcxproj` as a `<ProjectReference>` (Win32 only — D3D8 games are never 64-bit, so no x64 config exists), with `LinkLibraryDependencies`/`UseLibraryDependencyInputs` both off — a pure build-order dependency, not something this DLL links against or exports symbols from. Its own post-build step stages the output to `Data\Wrappers\D3D8to9\d3d8.dll`; SteamSwitcher's `ModManager.DeployLegacyRendererWrapper` symlinks it into a game's own install folder — see the SteamSwitcher README for the full deployment design. |

---

## Render Backend Support

| Backend | Status |
|---|---|
| DX9 | **Shipping.** Confirmed working on both a genuine DX9-via-wrapper title (Blood Omen 2, via a community D3D8-to-D3D9 conversion) and a D3D8to9-wrapped title deployed by this project itself (see [Deployment Layout](#deployment-layout) and `ModManager.DeployLegacyRendererWrapper` on the SteamSwitcher side). The dummy device uses `D3DDEVTYPE_NULLREF`, not `D3DDEVTYPE_HAL` — the dummy only needs a valid `EndScene` vtable address, never actually renders, and `D3DDEVTYPE_HAL` was confirmed failing outright on a game whose own device (via a wrapper) already held exclusive-fullscreen access to the display by the time this DLL's own dummy-device attempt ran — a real, reproducible driver-level contention, not a hypothetical. `NULLREF` needs no display/GPU resources at all, so it can't contend with anything, and the vtable address it produces is the same shared `d3d9.dll` code regardless of device type. |
| DX11 | **Shipping.** Confirmed stable across multiple real games, including Steam overlay present and fullscreen/exclusive-fullscreen transitions. |
| DX12 | **Shipping**, on either of two render paths selected per-attach (`DX12::g_usingD3D11On12`, decided in `LazyInit`): a native `ImGui_ImplDX12` path by default, or a `D3D11On12` interop path specifically when NVIDIA Streamline (`sl.interposer.dll`) is present — see [DX12 Queue Capture](#dx12-queue-capture). Frame-boundary detection is now a native `Present`/`Present1` hook (see [Hooking Strategy](#hooking-strategy)), not solely dependent on DX11's borrowed vtable — confirmed necessary on a real title (`ParasiteMutant_Demo`, Unity) where the DX11-borrowed hook never fired at all. |

DX9 installs the same way DX11 does — a dummy-device hook attempt at startup that harmlessly no-ops (`DX9::Install()` logs "not installed (game likely doesn't use it)") if the process isn't actually using Direct3D9. No module-detection gate is needed for it — `Direct3DCreate9` succeeds off Windows' own bundled `d3d9.dll` regardless of whether the target game touches Direct3D9 at all, so the hook attempt itself carries no real cost even against a DX11/DX12-only game.

---

## Hooking Strategy

Every hook in this DLL is installed via MinHook, which patches the **target function's own machine code** — not a per-instance vtable slot. This has a load-bearing consequence used throughout the codebase: a hook installed once, via a throwaway dummy device/swapchain, intercepts calls from **every** real instance of that function in the process, DX9/DX11/DX12 games alike.

Concretely:

- **`IDXGISwapChain::Present`** (vtable slot 8) and **`ResizeBuffers`** (slot 13) are hooked via a dummy D3D11 device + flip-model swapchain created in `DX11::Install()`. On some GPU/driver combinations this address is shared with D3D12's own flip-model swapchain implementation, so this hook alone happens to catch a DX12 game's real `Present` calls too — but that's a coincidence, not a guarantee (see below). **This does not cover classic (non-Ex) Direct3D9** — plain `IDirect3D9`/`IDirect3DDevice9` never goes through DXGI at all, which is exactly why `DX9::Install()` exists as an entirely separate hook on `IDirect3DDevice9::EndScene` rather than being redundant with this one. A DX9Ex game (`IDirect3DDevice9Ex`, available Vista+) can in principle present via a path closer to DXGI, but this codebase doesn't special-case that distinction — it relies on `DX9::Install()`'s own `EndScene` hook unconditionally, which covers both.
- **`IDXGISwapChain::Present`/`Present1`** (vtable slots 8/22) are *also* hooked natively for D3D12, via a dummy D3D12 device + `DIRECT` command queue + genuine flip-discard swapchain created in `DX12::InstallNativePresentHook()` (called from `DX12::Install()`). This exists because the DX11-borrowed hook above is not reliable for D3D12: it was confirmed to never fire at all on a real D3D12 game whose `Present` vtable didn't happen to coincide with DX11's (`ParasiteMutant_Demo` — DX9/11/12 all loaded, `NOBACKEND` every time despite `ExecuteCommandLists` correctly firing). Both hook attempts route through MinHook's same shared-function-patch mechanism, so whichever one installs first "wins" a given address — if DX11's probe already owns slot 8 on this driver, `DX12::PatchDX12PresentIfNew`'s own attempt on slot 8 fails with `MH_ERROR_ALREADY_CREATED` (logged, harmless, expected) and slot 22 (`Present1`) is hooked independently instead. Either slot succeeding is sufficient — `DX12::g_presentHooked` is set by whichever one lands, not just slot 8, so "DX12 native Present hook: FAILED" in the log means *neither* slot could be hooked, not that the more common `Present1` path failed while `Present` "succeeded" by coincidence.
- **`ID3D12CommandQueue::ExecuteCommandLists`** is hooked via a dummy D3D12 device + command queue in `DX12::Install()`. Fires for every queue's `ExecuteCommandLists` call in the process, not just the swapchain's real presentation queue — see [DX12 Queue Capture](#dx12-queue-capture) for why that matters.
- **`IDXGIFactory2::CreateSwapChainForHwnd`** (vtable slot 15) is hooked from within `DX11::Install()` too, purely to observe swapchain creation — see below.

`DX11::HookedPresent` always attempts its own DX11 attach/render first. On a real DX12 game, `swapChain->GetDevice(ID3D11Device)` simply fails (clean, expected, not an error), and the function falls through to `DX12::TryInitAndRender()` once a command queue has been captured. The native `DX12::HookedPresent`/`HookedPresent1` hooks call `DX12::TryInitAndRender()` directly with the real swapchain pointer, independent of whether DX11's hook ever fires for this game at all.

---

## DX12 Queue Capture

This is the single trickiest part of the codebase and worth understanding before touching `PresentHookKit.h`'s DX12 namespace.

A DX12 swapchain is bound to one specific `ID3D12CommandQueue` at creation time — submitting the overlay's own render commands on the wrong queue produces a cross-queue resource-state hazard (the backbuffer's PRESENT↔RENDER_TARGET transitions have no guaranteed ordering against the real presenting queue's own barriers). Three queue-capture problems exist, each with its own fix:

1. **Wrong queue type.** Capturing whichever queue calls `ExecuteCommandLists` *first* is unsound — non-DIRECT queues (COPY/COMPUTE) exist on the same device and can win that race, and submitting DIRECT-type render commands on one corrupts swapchain state (the game's own `Present()` starts returning `DXGI_ERROR_INVALID_CALL`). **Fixed**: `HookedExecuteCommandLists` only captures a queue whose `D3D12_COMMAND_QUEUE_DESC.Type == D3D12_COMMAND_LIST_TYPE_DIRECT`.
2. **Wrong DIRECT queue.** DIRECT-type filtering alone isn't sufficient — some games have more than one DIRECT queue, and "first DIRECT queue observed" is still a guess. **Fixed properly**: `DX11::HookedCreateSwapChainForHwnd` inspects the `pDevice` parameter of every swapchain-creation call — by DXGI's own API contract, this parameter *is* the swapchain's actual presentation queue for a D3D12 swapchain, no ambiguity possible. This is now the **authoritative** capture source (`DX12::OnSwapChainCreationQueueSeen`); the original `ExecuteCommandLists`-based DIRECT-type heuristic is kept only as a fallback for the case where a game creates its swapchain before this DLL's factory hook installs.
3. **Authoritative capture can be structurally unavailable.** A game whose real swapchain is created before this DLL is injected will never trigger `HookedCreateSwapChainForHwnd` for it — only unrelated (e.g. Steam-internal) swapchain creations get observed. With multiple real DIRECT queues and no authoritative source, the fallback heuristic is a guess — the same class of bug as #2. **Fixed via Present-correlation** (`RecordExecuteCommandListsForCorrelation`/`RecordPresentForCorrelation`, both in the `DX12` namespace): every DIRECT-type queue's last `ExecuteCommandLists` timestamp is tracked; on every real `Present`, whichever queue fired most recently (within 8ms) scores a point. After ~90 frames, the highest-scoring queue is used. If no queue ever correlates, the DLL refuses to attach rather than guess (see [Known Limitations](#known-limitations)).

Every distinct D3D12 queue seen is logged once per session (type, priority, pointer) — cheap, and keeps every future log self-documenting without needing a special diagnostic build to re-derive which queue got captured and why.

**A fourth, self-inflicted problem, now fixed:** `DX12::InstallNativePresentHook()` (see [Hooking Strategy](#hooking-strategy)) creates its own real, throwaway `ID3D12CommandQueue` to obtain a genuine D3D12 `Present`/`Present1` vtable — and that probe's `CreateSwapChainForHwnd` call routes through the same hooked, shared-function `IDXGIFactory2::CreateSwapChainForHwnd` as the real game's calls. Without a guard, `OnSwapChainCreationQueueSeen` had no way to distinguish the probe's own queue from the game's real one, and would "authoritatively" capture the probe's queue — which is `Release()`d moments later, leaving `g_capturedQueue` dangling and faulting the first real render attempt (`g_renderDisabledAfterFault`, silently, with no fallback since the backend had already confirmed by then). **Fixed** via `DX12::g_suppressQueueCapture`, set for the duration of the probe's own `CreateSwapChainForHwnd` call only — `OnSwapChainCreationQueueSeen` early-returns while it's set.

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

## In-Game Stats Windows

The `StatsWindowKit` counterpart of the section above — lets a mod's live-data window (stat bars, editable fields, buttons, dropdowns — e.g. both games' `CharacterStats.cpp`) render inside this overlay via ImGui instead of its native win32 window. See the `ModKit` project's own README, [Stats Window Overlay Bridge](../ModKit/README.md#stats-window-overlay-bridge), for the mod-side registration surface (`StatsWindowKit::OverlayBinding`) and the wire format.

Reuses the exact same row-click mechanism as config windows — `DrawStatusPanel`'s click handler sets both `g_openConfigModName` and `g_openStatsModName` to the clicked mod's name in the same `ClickButtonForOverlay` call, since a mod only ever registers with one of the two bridges. `DrawStatsPanel()` (`OverlayContent.h`) checks `ModKitInterop::HasConfigWindow` first (a genuine collision isn't possible in this codebase — no mod uses both `ModConfigWindow` and `StatsWindowKit` — but this keeps that assumption from silently double-drawing if it ever changes) and otherwise renders exactly like `DrawConfigPanel`: same position (immediately right of the status panel), same header/close-button/Escape handling, same "stops drawing the moment `HasStatsWindow` goes false" behavior.

**Row rendering** switches on `ModKitStatsRowType` per row: `MODKIT_STATS_DIVIDER` → a hand-drawn rule (lead line, label, trailing line via `ImGui::GetWindowDrawList()`) rather than `ImGui::SeparatorText`, whose own separator line always runs out to the *window's* right edge regardless of cursor X — harmless in a single-column panel but wrong in the two-column layout below, where a column-one divider's line ran straight through column two (and vice versa); this version is explicitly bounded to `contentW`, matching every other widget in the switch. It's also a clickable section header (`ImGui::InvisibleButton` over the row) — clicking it collapses or expands every row between it and the next `MODKIT_STATS_DIVIDER`/`MODKIT_STATS_COLUMN_BREAK`/end of the row list, tracked per-mod in `g_statsCollapsedSections` (an `unordered_set<string>` of collapsed labels, keyed by `modName`) and seeded once per modName from each row's `defaultCollapsed` flag via `g_statsCollapsedDefaultsApplied` — see the `ModKit` project's own README, [Live Data Windows](../ModKit/README.md#live-data-windows-statswindowkit)'s "Collapsible sections" note, for the native-side mirror of this (same `Element` list drives both, so a mod gets this for free on both renderers with no extra code). Collapsed rows are skipped entirely by the draw loop — not drawn, no cursor advance — so `MODKIT_STATS_DIVIDER` is the only row type ever computed as hidden-or-not before the switch even runs. `MODKIT_STATS_BAR` → a colored `ImGui::ProgressBar` plus the pre-formatted `"cur / max"` text (color unpacked from `row.barColor`'s `0x00BBGGRR` layout), `MODKIT_STATS_TEXT` → a plain label/value line, `MODKIT_STATS_EDIT` → `ImGui::InputText`, `MODKIT_STATS_BUTTON` → `ImGui::Button`, `MODKIT_STATS_CHECKBOX` → `ImGui::Checkbox`, `MODKIT_STATS_DROPDOWN` → `ImGui::BeginCombo` + `ModKitInterop::GetStatsWindowDropdownOption` per entry, `MODKIT_STATS_COLUMN_BREAK` → not a real row, see below. Writes go back through `ModKitInterop::ClickStatsWindowButton`/`ToggleStatsWindowCheckbox`/`ChangeStatsWindowEdit`/`SelectStatsWindowDropdown`, all addressed by `rowIndex` (stable for the frame — see the ModKit README's own note on `getRowCount`/`getRow` timing). A collapsed row's `rowIndex` doesn't shift either — rows are skipped, not removed from the list — so `g_statsEditBuffers` keys (below) stay valid across a collapse/expand toggle.

**Two-column layout.** `DrawStatsPanel` fetches every row into a local vector up front (rather than lazily per-row like `DrawConfigPanel` still does) specifically to check for a `MODKIT_STATS_COLUMN_BREAK` before `ImGui::Begin` — the panel's width has to be decided before then, not discovered mid-draw. A mod with no column break gets the original narrow-but-tall 320px panel; one that includes a break gets a wider two-column layout (two 300px columns plus a gutter) instead, with the loop jumping the cursor to the top of column two on hitting that row and staying there for the rest of the list. Whichever column ends up taller sets the panel's auto-fit height, tracked explicitly across the jump rather than left to whatever ImGui's cursor happens to be sitting on at the end of the loop — a collapsed section's rows are simply never drawn, so they don't contribute to this either, and a panel with everything collapsed is exactly divider-row-tall. See [Stats Window Overlay Bridge](../ModKit/README.md#stats-window-overlay-bridge) for the mod-side `Element::ColumnBreak()`.

**Edit-box buffers** (`g_statsEditBuffers`, keyed by `"modName#rowIndex"`) persist across frames so `ImGui::InputText` has a stable buffer to type into. Each is refreshed from that frame's `row.valueText` (the mod's live game value) only while the box isn't `ImGui::IsItemActive()` — the same focus-guard reasoning as `StatsWindowKit::RefreshEditText` on the native side, just re-implemented here since there's no HWND to ask.

---

## Session Header, Dim Backdrop, and Logs Button

`DrawStatusPanel` (`OverlayContent.h`) grew a header block above the existing "MODS - click to configure" strip, mirroring SteamSwitcher's own `ModsStatusPanel.cs` (WinForms, Toast mode) feature-for-feature so both modes read as the same product:

- **Game name, profile/persona name, and a live session timer.** Sourced from `SessionInfo` (`Overlay::g_session`), populated by `GAMEINFO|<name>|<epochMs>|<profile>` over `OverlayPipe` — sent by `ModsPanel.cs`'s `SendGameInfoToOverlay()` right after `SETMODCHANNEL|1`, at the same three points the Toast side calls `SetSessionInfo`: injection completing, the shared auto-inject/GOG continuation, and a live mode-switch to Overlay. `launchEpochMs` of `0` is the "unknown" sentinel — the timer line just doesn't draw.
- **`DisplayProfileText`/`FormatElapsed`** are the exact same transforms `ModsStatusPanel.cs` applies — including stripping a leading `Run … on ` prefix from the profile name if present (some Steam vanity/persona names happen to read like a launcher string; splits at the *last* " on " so a genuinely-containing name is safe). Both modes show identical text for identical session data.
- **Dynamic width.** `ComputeRequiredWidth`-equivalent logic measures the game name, profile+timer line, and footer text (`PoolInfoText`/`MonoPoolInfoText` — the trampoline and MonoKit pool lines) via `ImGui::CalcTextSize`, and widens the panel (up to a capped `MAX_WIDTH`) if any of them needs more room than the base width — same reasoning as the WinForms side, same cap-then-clip fallback beyond that. The footer is measured *before* `WIDTH` is finalized and the string is reused for the actual draw call, so a long trampoline/pool line (e.g. a large `VirtualAlloc` size, or a multi-chunk MonoKit pool) widens the panel instead of being clipped by the fixed-width, `NoResize` window it's drawn into. `DrawConfigPanel` reads back `g_lastStatusPanelWidth` (set every frame by `DrawStatusPanel`) rather than a fixed constant, so it stays correctly positioned to the status panel's right edge regardless of how wide that edge currently is.
- **A real drawn "Logs" button** (filled background, border, hover state — not just underlined text) in the header. Click sends `SHOWLOGS` via the new `OverlayCommandPipe.h` (mirrors `RemoteLog.h`'s shape: a fire-and-forget `CreateFileA`/`WriteFile` into a pipe SteamSwitcher hosts as server — `SteamSwitcherOverlayCmdPipe`, received by `OverlayCommandMonitor.cs`) to bring SteamSwitcher's Log window to the front, anchored near this panel's own on-screen position (the game window's rect, offset by the same `+22, +56` this panel places itself at).
- **A dimmed backdrop** behind the panel while it's open, via `DrawDarkBackdrop()` on `ImGui::GetBackgroundDrawList()` — mirrors `ModsStatusPanel.cs`'s own `DimBackdrop` Form, just via ImGui's background draw list instead of a second Win32 window. Background-draw-list content always renders at the very bottom of the frame regardless of call order, so toasts (`DrawToastStack`, normal ImGui windows) are unaffected by construction, not because of where `DrawDarkBackdrop()` happens to be called from in `DrawOverlay`.

Known Unicode limitation: `GAMEINFO`'s text now arrives as correct UTF-8 (an earlier bug had `OverlayPipeClient.cs` encoding pipe messages as ASCII, silently replacing every character above code point 127 with a literal `?` before the message even left SteamSwitcher — fixed by switching to `Encoding.UTF8.GetBytes`), but this project never loads a custom font or explicit Unicode glyph ranges into ImGui — it runs on ImGui's implicit default font, Basic Latin only. A persona name containing genuinely exotic glyphs (CJK, certain symbol blocks) still renders as ImGui's own missing-glyph fallback, a separate, unaddressed limitation from the encoding bug. The Toast/WinForms side has no such limitation, since it uses whatever system font Windows provides.

---

## Attach Lifecycle

Hooks install early and stay installed for the life of the process — proven safe. The actual attach work (device/context refs, ImGui context creation, backbuffer/RTV setup) is deliberately delayed until one of three explicit triggers fires `PresentHookKit::RequestAttach()`:

- The hotkey (`INSERT`, via `HotkeyPoll`)
- `SETMODCHANNEL|1` over the pipe (SteamSwitcher enables Channel 2 / mod content)
- `TOAST|<text>` over the pipe (a toast alone must be enough to bring up rendering, or it would sit queued and expire before anything is drawing)

`RequestAttach()` is idempotent — every call after the first (INSERT, `SETMODCHANNEL|1`, or a toast) is a no-op against `g_attachRequested`, so only the first ever logs or does anything.

**No backend may attach at all until `g_confirmedGameHwnd` is set** (via the `GAMEHWND` pipe message — see [Pipe Protocol](#pipe-protocol)), full stop. Each of DX9/DX11/DX12's own `LazyInit` gates on it as the very first check, before touching any device or ImGui backend object — not "prefer it when available, race otherwise." An earlier "first successful attach wins" design was tried and is exactly backwards: whichever backend's `Present`/`EndScene` happens to fire first can easily be an incidental device (Steam's own overlay rendering internally in the same process, confirmed real on multiple games) rather than the actual game, and would permanently lock out the correct backend for the rest of the session with no way to correct it. This is safe to require unconditionally because `GAMEHWND` is now sent immediately alongside `SETMODCHANNEL|1` (see `ModInjector.cs`'s `InjectAsync`/`InjectOverlayIntoRunningGame` on the SteamSwitcher side), not deferred to a later point — in practice this costs at most a few frames' delay, invisible at any real framerate.

Once `g_confirmedGameHwnd` is known, `ClaimHwndAndSubclass` accepts a candidate window if it's either an *exact match*, or a *descendant* of it (via `GetAncestor(hwnd, GA_ROOT)`) — some engines create the D3D9/D3D11/D3D12 device against an inner child render window while the outer titled/sized frame window (what SteamSwitcher's own window-detection heuristic finds) is a different HWND. Confirmed real on a game whose device's own `cp.hFocusWindow` never matched by equality alone.

A rejection here is **per-device/per-swapchain, not a permanent session-wide abort**. Each backend tracks the single most recently rejected device/swapchain pointer and skips re-running its (expensive) init sequence for repeat calls from that *same* instance — but a call from any *other* device, including the game's own real one arriving after an incidental device's rejection, still gets a genuine fresh attempt. This matters because multiple real D3D9/D3D11/D3D12 devices legitimately coexist in one process (Steam's own incidental usage *and* the game's actual device, confirmed real on several games) — a wrong device rejecting first must not block a later, correct device from ever getting a chance. The 16 other `g_attachPermanentlyAborted = true` sites in this file (ImGui backend init failing, no focus window at all, Streamline queue resolution failing) are genuinely process-wide, retrying-won't-help failures and are unaffected by this — only the three `ClaimHwndAndSubclass`-refusal sites use per-instance tracking instead.

Once a backend's `LazyInit` runs (DX9/DX11) or the heavier DX12 path (descriptor heaps, per-backbuffer RTVs, a frame-context/fence ring for the native path, or a `D3D11On12` device wrap for the Streamline path — see [DX12 Queue Capture](#dx12-queue-capture)), a confirmed successful attach fires `OnBackendConfirmed("DX9"/"DX11"/"DX12")`, which sends `BACKENDOK|<name>` back to SteamSwitcher over `SteamSwitcherOverlayCmdPipe` (see [Pipe Protocol](#pipe-protocol)) and is idempotent (only the first backend to confirm sends anything, guarded by `g_backendConfirmed`). If no backend confirms within `kNoBackendTimeoutSeconds` (10s) of `RequestAttach()`, a dedicated watchdog thread sends `NOBACKEND` instead — SteamSwitcher falls back to Toast display for that session only (see the SteamSwitcher README's own notes on `_sessionRendererFallbackActive` and `LegacyRenderer`/`DeployLegacyRendererWrapper`), without ever touching the user's saved notification-mode preference.

Every attach step is SEH-wrapped.

---

## Pipe Protocol

SteamSwitcher connects as client and pushes plain `KEY|value` text to `\\.\pipe\SteamSwitcherOverlayPipe` (`OverlayPipe.h`):

| Message | Effect |
|---|---|
| `SETMODCHANNEL\|1` | Enables Channel 2 (mod content) and requests attach. |
| `SETMODCHANNEL\|0` | Disables Channel 2 — closes the status panel if open, but toast rendering continues (drains naturally as toasts expire). |
| `TOAST\|<text>` | Pushes a generic, non-mod-related info toast and requests attach. |
| `GAMEHWND\|<value>` | The real game window handle (decimal-encoded), resolved by SteamSwitcher's own window-detection logic (`ModInjector.cs`) and sent immediately alongside `SETMODCHANNEL\|1` — see [Attach Lifecycle](#attach-lifecycle) for why every backend requires this before attaching at all. |
| `CLOSECONFIGWINDOWS` | Closes every open mod config window and stats window, native or overlay-drawn alike (calls `ModKitInterop::CloseAllConfigWindows()` and `ModKitInterop::CloseAllStatsWindows()` — see the ModKit README's [Config Window Overlay Bridge](../ModKit/README.md#config-window-overlay-bridge) and [Stats Window Overlay Bridge](../ModKit/README.md#stats-window-overlay-bridge)). Sent the instant SteamSwitcher's `NotificationMode` dropdown changes, so a window from the old mode never lingers into the new one — see `ModsPanel.cs`'s `Cmb_notificationMode_Changed`. |

`SETMODCHANNEL` isn't the only place `g_channel2Enabled` matters to ModKit.dll: `DrawOverlay` pushes it there every single frame via `ModKitInterop::SetOverlayModeActive()`, independent of this pipe protocol entirely — see `ModKit_IsOverlayModeActive`'s own comment in the ModKit README for why a mod's keyboard-hotkey handler needs a persistent flag like this rather than the per-click `ModKit_IsOverlayActive()`.

This DLL never writes back over this same pipe — logging goes out over the separate `SteamSwitcherOverlayLogPipe` (see [Logging](#logging)), and the two outbound signals below go out over the separate `SteamSwitcherOverlayCmdPipe` (`OverlayCommandPipe.h`, received by `OverlayCommandMonitor.cs` — the same pipe `SHOWLOGS` already used, see [Session Header, Dim Backdrop, and Logs Button](#session-header-dim-backdrop-and-logs-button)):

| Outbound message | Effect |
|---|---|
| `BACKENDOK\|<name>` | Sent once, by whichever backend (`DX9`/`DX11`/`DX12`) is first to genuinely confirm attach (see [Attach Lifecycle](#attach-lifecycle)). SteamSwitcher logs it and, if a `NOBACKEND` fallback already happened earlier this session, clears it and closes whatever Toast UI opened for it. |
| `NOBACKEND` | Sent once, by the attach watchdog, if no backend confirms within 10s of `RequestAttach()`. SteamSwitcher falls back to Toast display for that session only, scans the target process's loaded modules for `d3d8.dll`/`ddraw.dll` to identify a legacy renderer, and — if identified — deploys a matching D3D9-conversion wrapper (symlinked, not copied) into the game's own install folder so a future launch should attach normally. See the SteamSwitcher README for the full design. |

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

- **DX11's Present hook and DX12's native Present hook can race for the same vtable slot, and that's expected.** On drivers where D3D11 and D3D12 flip-model swapchains happen to share the same `Present` (vtable slot 8) code address, whichever of `DX11::Install()`/`DX12::InstallNativePresentHook()` runs first (currently always DX11, per `InstallWorkerThreadProc`'s call order) wins that address via MinHook; the other's `MH_CreateHook` on the same address correctly fails with `MH_ERROR_ALREADY_CREATED` (status `3`) and is logged, not silently swallowed. This is not a sign either hook is broken — `Present1` (slot 22) is never shared this way and is hooked independently, and `DX12::g_presentHooked` only needs one of the two slots to succeed. See [Hooking Strategy](#hooking-strategy).
- **DX12 + NVIDIA Streamline Present-correlation is probabilistic, not authoritative.** The fix (see [DX12 Queue Capture](#dx12-queue-capture)) resolves the real presentation queue by timing correlation, not by direct API observation — a real signal, not a guarantee. If correlation ever comes back inconclusive on a given game, the DLL correctly refuses to attach rather than guess (`g_queueResolvedSuccessfully == false` in `DX12::LazyInit`) — that's a missing overlay, not a crash, and is the expected/safe failure mode, not a regression to chase. `PresentHookKit.h`'s `g_refuseAttachOnStreamlineForTesting` (default `false`) instantly restores the old unconditional refuse-on-Streamline behavior if this path ever needs isolating again.
- **32-bit games: confirmed working.** `SteamSwitcherOverlay.vcxproj`'s Win32 configuration (`TargetName=SteamSwitcherOverlay32`) has now been built and run end-to-end against a real 32-bit game (Max Payne 2, via the D3D8-to-D3D9 wrapper — see [Deployment Layout](#deployment-layout)), with a confirmed `Backend confirmed: DX9` attach. `ModInjector.cs`'s bitness-aware injection plumbing (`IsWow64Process` detection, the `Mods32\` folder convention, `ModKitInjectHelper32.exe` for the cross-bitness `CreateRemoteThread`/`LoadLibraryA` case) works as designed. D3D8 titles specifically now have a real fix, not just a theoretical path — see `D3D8to9\` below and the SteamSwitcher README's `LegacyRenderer`/`DeployLegacyRendererWrapper` design. DirectDraw-only titles (pre-D3D8, or D3D1-7) still have no support at all — the equivalent conversion project (`dxwrapper`'s `Dd7to9`) is ~48,000 lines tightly coupled to that project's own internal framework, a substantially bigger undertaking than `D3D8to9` was, and hasn't been attempted.
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