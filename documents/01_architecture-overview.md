# WindowPosButton — Architecture Overview

> This document is for readers new to the codebase. Read `01_` → `02_` → `03_`
> to build an understanding in this order: what the program does and why it is
> designed this way (01), how it is implemented (02), and how specific bugs were
> fixed (03).
>
> Primary source file: `WindowPosButton/WindowPosButton.cpp` (a single file of
> roughly 2,160 lines). Almost all code is inside an anonymous `namespace { ... }`,
> with only `wWinMain` outside it. The application uses the Win32 API directly;
> there is no separate UI framework.

## 1. What Is This Program?

WindowPosButton is a resident Windows utility that displays **four small buttons
on the title bar of every standard window, immediately to the left of Minimize**.

- **Snap left/right:** place the window on the left or right at 50% of screen
  width (30% with Shift+left-click and 70% with right-click).
- **Centered 80%:** center the window at 80% of the work-area width with a 16:9
  aspect ratio; right-click uses the full work-area height.
- **Move to next monitor:** move through monitors in display-layout order,
  wrapping from the last monitor to the first; right-click expands across all
  monitors.
- **Additional feature:** double-clicking a window's left or right resize edge
  expands that window horizontally to the work-area width.

The application lives in the system tray and provides a “Start automatically
with Windows” option.

## 2. Core Constraints and the Decisions They Produce

This is the most important section of the document: the constraints below explain
every subsequent architectural decision.

### 2-1. It Must Operate on Windows Owned by Other Processes

Every target window belongs to **another process**, which creates two fundamental
constraints.

1. **It cannot draw directly into another process's window.** Instead of drawing
   buttons into the target title bar, the program creates independent top-level
   layered `WS_POPUP` overlay windows and positions them precisely over the area
   immediately left of the caption buttons (Minimize, Maximize, Close). Each
   target has four overlay windows: left, right, centered 80%, and next monitor
   (`TrackedWindow`, `EnsureTracked`, `WindowPosButton.cpp:745`).
2. **Windows UIPI (User Interface Privilege Isolation) prevents a lower-privilege
   process from controlling an elevated window.** To make buttons work on any
   elevated window—for example, development tools or installers—the application
   itself must always run elevated. Therefore:
   - Startup is registered through **Windows Task Scheduler**, not the registry
     `Run` key (`RegisterStartupTask`, `WindowPosButton.cpp:1670`,
     `TASK_LOGON_INTERACTIVE_TOKEN` + `TASK_RUNLEVEL_HIGHEST`). A `Run` key cannot
     start an elevated application automatically without a UAC prompt.
   - The About dialog makes this explicit: “This application runs with
     administrator privileges so the buttons are available on elevated windows.”
     (`ShowAboutWindow`, `:1851`).

### 2-2. It Does Not Enter Other Processes

The project deliberately avoids DLL injection and in-process hooking.

- It subscribes to window events with `SetWinEventHook` using
  **`WINEVENT_OUTOFCONTEXT`** (`wWinMain`, `:2124`). `WINEVENT_INCONTEXT` would
  require injecting this process's DLL into every process that produces an event;
  `OUTOFCONTEXT` delivers events asynchronously to this process's message queue.
- It avoids synchronous `SendMessage`-style calls to target windows. The
  `PerformButtonAction` comment notes that synchronous messages can amplify UI
  stalls in COM/OLE-heavy workflows, such as Unity launching Visual Studio.

### 2-3. The Main Thread Must Never Block

Calculating button position and size requires cross-process DWM queries such as
`DwmGetWindowAttribute`. If a target process is slow or hung, these calls can take
a long time or fail to return. Therefore:

- Such queries always run on an **OS thread-pool worker**. `ComputeUpdatePlan`
  performs the work, and `SpawnMeasureThread` queues it with `QueueUserWorkItem`
  (`:906`, `:927`). Results return to the main thread only through
  `PostMessageW` and `WM_OVERLAY_RESULT`.
- Consequently, **no lock protects shared state**. Workers never touch
  `g_windows` or overlay HWNDs; only the main thread does. As the
  `ComputeUpdatePlan` comment says: “Never touches g_windows, TrackedWindow, or
  any OverlayButton, so it needs no lock — there is nothing shared to protect.”

### 2-4. Event-Driven, with Minimal Polling

The application responds to movement, focus, visibility, destruction, and Z-order
changes by subscribing to OS accessibility events (WinEvents; `WinEventProc`,
`:1413`). It has only two genuine periodic polls:

- **Z-order safety net** (`kZOrderReassertTimerId`, two seconds). Since
  `EVENT_OBJECT_REORDER` handles the normal path, this is only a fallback for
  unusual cases.
- **Horizontal-resize double-click detection**
  (`kHorizontalResizeDoubleClickTimerId`, 15 ms). This is a lightweight
  `GetAsyncKeyState` compromise in place of a global mouse hook or sending
  `WM_NCHITTEST` to another process's window.

## 3. Component Map

| Component | Key functions/structures | Responsibility |
|---|---|---|
| Bootstrap | `wWinMain` (`:2083`) | Single-instance mutex, DPI configuration, class registration, hooks, and message loop |
| Window tracking | `TrackedWindow`, `g_windows`, `EnsureTracked`/`RemoveTracked`, `ShouldTrackWindow` | Decides which windows receive buttons and owns their state |
| Event pipeline | `WinEventProc`, `DispatchUpdate`, `ComputeUpdatePlan`, `ApplyUpdateResult` | Event → asynchronous measurement → main-thread application |
| Button placement | `ComputeButtonRect` | Calculates screen coordinates for slots left of caption buttons, including DPI |
| Overlay rendering | `RenderAndPositionOverlay`, `SetPixelPremultiplied` | Pixel-level drawing into layered overlay windows |
| Z-order management | `RaiseOverlaysAboveTarget`, `ReassertForegroundZOrder`, `FindExternalFrontNeighbor` | Keeps overlays immediately above their target while allowing other windows to cover them naturally |
| Button actions | `PerformButtonAction`, `MoveWindowToNextMonitor`, `ExpandWindowAcrossAllMonitors` | Moves and resizes target windows on click |
| Horizontal expansion | `GetHorizontalResizeEdge`, `PollHorizontalResizeDoubleClick`, `ExpandWindowHorizontally` | Detects double-clicks on horizontal resize edges |
| Tray icon | `CreateTrayIcon`/`RestoreTrayIcon`, `ShowTrayMenu`, `ScheduleTrayIconRecovery` | Notification-area icon, context menu, and recovery after Explorer/session changes |
| Startup | `IsStartupEnabled`, `RegisterStartupTask`/`DeleteStartupTask` | Elevated logon startup via Task Scheduler |
| Diagnostics | `LogDiagnostic`/`LogWindowDiagnostic`, `InitializeDiagnosticLog` | `%LOCALAPPDATA%\WindowPosButton\diagnostic.log` |

## 4. Data Flow: From an Event to Updated Buttons

```
OS WinEvent (for example, the user drags a window)
   → WinEventProc (:1413)                  main thread; classifies the event
   → DispatchUpdate(hwnd) (:956)           main thread; coalesces if work is pending
   → SpawnMeasureThread (:927)             queues work with QueueUserWorkItem
   → ComputeUpdatePlan (:683, worker)      calls ComputeButtonRect for each button
   → PostMessageW(WM_OVERLAY_RESULT)       worker returns result to main thread
   → ApplyUpdateResult (:855, main thread) RenderAndPositionOverlay + SetWindowPos
```

Button clicks are handled immediately outside this pipeline. When an overlay
window (`OverlayWndProc`, `:1485`) receives `WM_LBUTTONUP` or `WM_RBUTTONUP`, it
calls `PerformButtonAction` (`:1353`) to move the target window. The resulting
`EVENT_OBJECT_LOCATIONCHANGE` and related events then run the pipeline above to
recalculate the overlay positions. In other words, moving a window and updating
the button positions share the same event pipeline.

## 5. Threading Model

- **Main thread:** owns the message loop, every overlay HWND, `g_windows`, and
  all timers. It is the only thread that calls `SetWindowPos` or
  `UpdateLayeredWindow` for overlays (see the `ApplyUpdateResult` comment).
- **OS thread-pool workers** (`QueueUserWorkItem`): run `ComputeUpdatePlan`.
  They only read target-window, monitor, and DWM state, then copy values into a
  result message.
- **No synchronization primitive:** this division eliminates the need for a
  mutex or critical section. The sole cross-thread communication mechanism is
  `PostMessageW(WM_OVERLAY_RESULT, ...)`.

## 6. Which Windows Are Tracked?

`ShouldTrackWindow` (`:357`) requires every one of the following:

- It is a valid, currently visible top-level window
  (`GetAncestor(hwnd, GA_ROOT) == hwnd`).
- It has both `WS_CAPTION` and `WS_MINIMIZEBOX`, limiting tracking to conventional
  windows with a title bar and Minimize button.
- It is not `WS_EX_TOOLWINDOW`, which excludes tool windows and some palette
  windows.
- It is not cloaked (`DWMWA_CLOAKED`), excluding such cases as UWP windows on a
  different virtual desktop.
- Its rectangle has nonzero width and height.

The program's own overlays use `WS_POPUP` and have no caption, so these criteria
naturally exclude them.

## 7. Continue Reading

- **`02_technical-implementation.md`** explains the components above in more
  detail: event coalescing and timer reuse, button rendering, Z-order maintenance,
  tray-icon recovery, and Task Scheduler registration.
- **`03_dpi-button-size-and-cpu-usage.md`** records three specific issues that
  were discovered and fixed: inconsistent button size across differing monitor
  DPIs, CPU/disk overhead from diagnostic logging, and preserving proportions and
  avoiding flicker when moving to the next monitor.
