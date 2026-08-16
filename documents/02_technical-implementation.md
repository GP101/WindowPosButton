# WindowPosButton — Technical Implementation Details

> This document examines each component introduced in
> `01_architecture-overview.md`. It does not repeat the detailed fixes for the
> three issues covered by `03_dpi-button-size-and-cpu-usage.md`: inconsistent
> button sizes across monitors with different DPI, diagnostic logging CPU/disk
> overhead, and proportional next-monitor movement without visible flicker.
>
> All locations refer to `WindowPosButton/WindowPosButton.cpp`. Line numbers can
> shift as the code evolves, so search by function name for an exact location.

## 1. Bootstrap (`wWinMain`, `:2083`)

Each startup step has a reason:

1. **Single-instance mutex** (`CreateMutexW(kMutexName)`): if it already exists,
   report `ERROR_ALREADY_EXISTS`, show a message, and exit.
2. `InitializeDiagnosticLog()`: initialize first so every later stage can log.
3. `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`:
   this must run **before any window is created**.
4. `RegisterClasses()`: register the overlay class (`kOverlayClassName`) and the
   hidden main-window class (`kMainClassName`).
5. `RegisterWindowMessageW(L"TaskbarCreated")`: register the broadcast message
   sent when Explorer restarts (see §10).
6. Create `g_hMain` as a **normal top-level window that is never shown**. It is
   not a message-only `HWND_MESSAGE` window because such windows do not receive
   broadcast messages such as `WM_DISPLAYCHANGE`, which is needed for multi-monitor
   hot-plug handling.
7. `WTSRegisterSessionNotification`: detect session locks and unlocks for tray
   icon recovery (see §10).
8. `ScheduleTrayIconRecovery("startup")`: a logon-triggered task can start before
   Explorer's notification area, so initial registration reuses the retry path.
9. Install two repeating timers: the two-second Z-order safety net (§6) and the
   15-ms horizontal-resize double-click poll (§9).
10. Install five `SetWinEventHook` hooks, all system-wide (`hProcess=0`,
    `idThread=0`) and `WINEVENT_OUTOFCONTEXT` (see §2-2 in `01_`).
11. Enumerate existing windows with `EnumWindows`, then call `EnsureTracked` and
    `DispatchUpdate`.
12. Run the message loop.
13. On exit, remove the five hooks, release/close the mutex, and call
    `ShutdownDiagnosticLog()` to close the log file.

## 2. Window Tracking Lifecycle

### `TrackedWindow` (`:103`)

One instance exists for each target window and is stored in
`g_windows` (`std::unordered_map<HWND, std::unique_ptr<TrackedWindow>>`).

- `OverlayButton left, right, nextMonitor, center80`: each button's HWND plus its
  `hover` and `pressed` state.
- `dispatchInFlight`, `dispatchAgainRequested`: event coalescing state (§3).
- `pendingMonitorMoveReassert`, `pendingMonitorMoveRect`: state for the
  next-monitor action; see the next-monitor section of `03_`.

### `EnsureTracked` (`:745`)

If the target is already tracked, this function returns. Otherwise it creates a
`TrackedWindow` and four overlay HWNDs:

```cpp
CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kOverlayClassName, L"", WS_POPUP, 0, 0, 1, 1, ...)
```

- `WS_POPUP` without a caption means `ShouldTrackWindow` automatically rejects
  the overlays themselves.
- `WS_EX_NOACTIVATE` prevents a button click from making the overlay foreground.
  After handling a click, `PerformButtonAction` explicitly calls
  `SetForegroundWindow` for the target.
- **No owner is assigned** (`owner = nullptr`). The code comment explains that a
  cross-process owned popup can tie this helper's UI thread to COM/OLE-heavy
  applications such as Explorer, Unity, and Visual Studio during activation or
  shutdown.
- Newly created overlays begin hidden at 1×1. Their real size and location are
  set and shown on the first pass through the event pipeline (§3).

### `RemoveTracked` / `HideOverlaysForTarget`

- `RemoveTracked` (`:1085`) runs on `EVENT_OBJECT_DESTROY`: it destroys four
  overlay windows, erases the `g_windows` entry, and erases
  `g_locationEventLogs`.
- `HideOverlaysForTarget` (`:772`) runs on `EVENT_SYSTEM_MINIMIZESTART` and
  `EVENT_OBJECT_HIDE`: it sends only `SW_HIDE` to the four buttons and retains
  the `g_windows` entry for reuse when the target returns.

## 3. Event Pipeline

### `WinEventProc` (`:1413`): Event Routing

| Event | Handling |
|---|---|
| `EVENT_SYSTEM_FOREGROUND` | If `ShouldTrackWindow`, call `EnsureTracked`, immediately call `RaiseOverlaysAboveTarget` to repair Z-order without waiting for the asynchronous pipeline (§6), then call `DispatchUpdate`. |
| `EVENT_OBJECT_SHOW` | Call `EnsureTracked` and `DispatchUpdate`; the event can arrive for any window, not only tracked ones. |
| `EVENT_OBJECT_LOCATIONCHANGE` | Call only `DispatchUpdate`. Logging is throttled to one line per second by `LogLocationEventThrottled`. |
| `EVENT_SYSTEM_MINIMIZESTART` / `EVENT_OBJECT_HIDE` | Call `HideOverlaysForTarget`. |
| `EVENT_SYSTEM_MINIMIZEEND` | Call `DispatchUpdate`. |
| `EVENT_OBJECT_DESTROY` | Call `RemoveTracked`. |
| `EVENT_OBJECT_REORDER` | Call `ReassertForegroundZOrder` (§6). Since `hwnd` is commonly a desktop container instead of a specific target, operate only on the current foreground window. |

Events where `idObject != OBJID_WINDOW` are filtered before entering these cases.

### `DispatchUpdate` (`:956`): Coalescing and a Shared Timer Slot

```cpp
void DispatchUpdate(HWND target, bool rearmSettleTimer = true) {
    auto it = g_windows.find(target);
    if (it == g_windows.end()) return;
    TrackedWindow& window = *it->second;

    if (rearmSettleTimer) {
        SetTimer(g_hMain, reinterpret_cast<UINT_PTR>(target), kOverlaySettleDelayMs, nullptr);
    }
    if (window.dispatchInFlight) {
        window.dispatchAgainRequested = true;
        return;
    }
    window.dispatchInFlight = true;
    SpawnMeasureThread(target);
}
```

- **Coalescing:** if a measurement worker is already running for a window
  (`dispatchInFlight`), no new worker is created. Instead,
  `dispatchAgainRequested` records that one more pass is needed after completion.
  When `WM_OVERLAY_RESULT` returns (`MainWndProc`, `:1929`), it decides whether
  to spawn again. This keeps at most one measurement worker per window during a
  flood of `EVENT_OBJECT_LOCATIONCHANGE` events, such as dragging.
- **Shared per-window timer slot:** several requirements that mean “do something
  else for this window shortly” reuse the target HWND itself as a `SetTimer` ID.
  This is safely distinct from the small global timer IDs
  `kZOrderReassertTimerId` (1), `kTrayRecoveryTimerId` (2), and
  `kHorizontalResizeDoubleClickTimerId` (3). Calling `SetTimer` again with the
  same ID resets its countdown, so multiple consumers can rearm this one slot;
  only the final delay fires once. Its current consumers are:
  1. `ApplyUpdateResult` retry (`retryLater`) when a restored window's location
     has not settled yet.
  2. `DispatchUpdate` rearming its `kOverlaySettleDelayMs` (150 ms) timer.
  3. `MoveWindowToNextMonitor` using `kMonitorMoveReassertDelayMs` (300 ms); see
     `03_` for details.

  When the timer fires, the default `WM_TIMER` branch in `MainWndProc` (`:1966`)
  performs the common sequence: `KillTimer` →
  `RevealMonitorMovePlacementIfPending` (consumer 3, if pending) →
  `DispatchUpdate(target, rearmSettleTimer=false)` (consumer 2). `false` avoids
  endlessly rearming a timer that is already executing its confirmation pass.

### `ComputeUpdatePlan` (`:683`): Worker-Thread Work

This is the work that `SpawnMeasureThread` submits to the thread pool. It receives
only the target HWND and never touches `g_windows` or overlay HWNDs (see §5 of
`01_`).

- **Readiness test:** `IsWindow && ShouldTrackWindow && !IsIconic && !stale`.
  `WindowRectLooksLikeMinimizedPlaceholder` (`:651`) detects `stale`: immediately
  after restoration, `IsIconic()` can be false while `GetWindowRect` and DWM
  caption coordinates still return the minimized off-screen placeholder, roughly
  `(-32000, -32000)`. Trusting it would draw overlays off-screen, so the function
  requests a later retry through `retryLater`.
- When ready, it calls `ComputeButtonRect` (§4) for SnapRight, SnapLeft, Center80,
  and MoveNextMonitor (the last only where there is more than one monitor), then
  returns an `UpdateResult` containing `ButtonPlacement{visible, rect}` values.

### `ApplyUpdateResult` (`:855`): Main-Thread Application

- **Revalidation:** the target might have been minimized or destroyed between the
  worker snapshot and main-thread receipt. The function checks `IsWindow`,
  `ShouldTrackWindow`, and `IsIconic` again. These are inexpensive local calls
  that do not send messages to the target.
- If not ready, it hides all four buttons and rearms the shared timer if
  `retryLater` is set.
- If ready, it applies every visible button with `RenderAndPositionOverlay` (§5)
  and `SetWindowPos`, inserting them in order relative to the position found by
  `FindExternalFrontNeighbor` (§6).

## 4. Button Placement: `ComputeButtonRect` (`:392`)

This calculates screen coordinates for slots immediately left of the caption
buttons. A larger `slot` number places the button farther left.

1. **Primary path:** obtain the actual caption-button region through
   `DwmGetWindowAttribute(DWMWA_CAPTION_BUTTON_BOUNDS)`.
2. **Fallback:** if that DWM query fails, estimate using empirical 96-DPI values:
   30 px title-bar height, three 45 px buttons, and an 8 px right margin, scaled
   by the monitor DPI.
3. Clamp top and bottom to the monitor work area.
4. When `shrink=true` (the Center80 button), reduce the calculated square to 80%.

The combined monitor/window DPI calculation and why it can be unstable while
dragging are described in §1 of `03_`.

## 5. Overlay Rendering: `RenderAndPositionOverlay` (`:500`)

- The function creates a 32-bpp `CreateDIBSection` pixel buffer, writes
  premultiplied alpha one pixel at a time with `SetPixelPremultiplied` (`:489`),
  then submits it in one operation through `UpdateLayeredWindow` (`ULW_ALPHA`).
  Direct pixels, rather than GDI shape APIs, provide crisp un-antialiased borders
  and translucent backgrounds.
- **Hover/pressed background:** an alpha-55 black wash on hover and alpha-90 while
  pressed approximate native caption-button feedback.
- **Four glyph types**, all drawn inside the same bounds:
  - Snap left/right: a rectangle outline with the corresponding half filled in
    accent blue.
  - Center80: an outer outline with a smaller filled rectangle in the center.
  - Next monitor: two monitor icons side by side, with only the right-hand
    destination monitor filled.
  - Snap-type outlines remain visible over their filled half, preventing a button
    from disappearing when the title-bar color matches the accent blue.
- `RegisterClasses` assigns the overlay class the fixed `IDC_HAND` cursor.
- `OverlayWndProc` (`:1485`) updates `hover` and `pressed` state and redraws on
  every mouse move/leave and left/right down/up message. If a pressed button is
  released inside its bounds (`PtInRect`), it calls `PerformButtonAction`.
  Shift+left-click maps to `ButtonVariant::ShiftLeftClick`, ordinary right-click
  to `ButtonVariant::RightClick`, and Shift+right-click to
  `ButtonVariant::ShiftRightClick`.

## 6. Maintaining Z-Order

Buttons must sit immediately above their target but be naturally covered by a
window that truly covers the target. They are therefore not globally topmost;
they are relatively topmost with respect to their target.

- `FindExternalFrontNeighbor` (`:786`) walks forward in Z-order from the target
  until it finds the first window that is not this program's overlay class. This
  identifies what is actually in front of the target.
- `RaiseOverlaysAboveTarget` (`:808`) reinserts the four buttons in order directly
  behind that neighbor (therefore directly above the target). An early-return
  guard skips the work if the order is already correct. This is necessary for
  correctness as well as speed: its own `SetWindowPos` calls trigger
  `EVENT_OBJECT_REORDER`, which would otherwise recursively invoke it forever.
- `ReassertForegroundZOrder` (`:836`) finds the foreground window and calls
  `RaiseOverlaysAboveTarget` only for it. Both `EVENT_OBJECT_REORDER` and the
  two-second `kZOrderReassertTimerId` safety-net timer call this function; the
  timer is only a fallback for rare missed reorder events.

## 7. Button Actions: `PerformButtonAction` (`:1353`)

Every action begins with `SetForegroundWindow(hwnd)`, because overlay buttons have
`WS_EX_NOACTIVATE` and otherwise would not activate their target.

- **Center80:** call `PrepareForManualResize` (restoring first if maximized), then
  use 80% of work-area width and a 16:9 height. Right-click uses full work-area
  height and top alignment; normal click centers horizontally.
- **SnapLeft/SnapRight:** `widthRatio` is 0.5 by default, 0.3 with Shift+left-click,
  0.7 with right-click, and 0.4 with Shift+right-click. Height always fills the
  work area. Placement uses `SetWindowPosForVisibleFrame` (§8).
- **MoveNextMonitor:** right-click calls `ExpandWindowAcrossAllMonitors`; all
  other variants call `MoveWindowToNextMonitor`. The ratio calculation and
  flicker avoidance are documented in `03_`.
- **`ExpandWindowAcrossAllMonitors` (`:1146`):** restore first if maximized, then
  use `SM_XVIRTUALSCREEN`, `SM_YVIRTUALSCREEN`, `SM_CXVIRTUALSCREEN`, and
  `SM_CYVIRTUALSCREEN` to place the window over the complete virtual desktop.
  Normal Windows maximization applies only to one monitor, so this is needed for
  a true all-monitor expansion.

## 8. `SetWindowPosForVisibleFrame` / `GetVisibleFrameRect`: Common Placement Pattern

`GetWindowRect` includes the **invisible resize border** on many DWM-managed
windows. Placing a raw `GetWindowRect` at the work area's left edge therefore
leaves the visibly rendered frame inset by that border.

- `GetVisibleFrameRect` (`:1183`) obtains the truly visible frame through
  `DWMWA_EXTENDED_FRAME_BOUNDS`, returning the supplied fallback rectangle if it
  fails.
- `SetWindowPosForVisibleFrame` (`:1117`) calculates the inset between the
  current raw and visible rectangles, then adds that inset to a requested visible
  rectangle to derive the actual `SetWindowPos` coordinates.
- `GetWindowRectForVisibleFrame` (`:1192`) performs the reverse calculation,
  such as when deriving `WINDOWPLACEMENT.rcNormalPosition` from a desired visible
  rectangle.

This pattern is shared by SnapLeft/SnapRight, `ExpandWindowHorizontally`,
`MoveWindowToNextMonitor`, and most other placement functions.

## 9. Horizontal-Resize Double-Click Expansion

As described in §2-4 of `01_`, this uses a lightweight poll rather than a global
mouse hook or cross-process `WM_NCHITTEST` messages.

- `GetHorizontalResizeEdge` (`:975`) determines whether the cursor is on the
  current foreground window's left or right resize edge. Its width is
  `SM_CXSIZEFRAME + SM_CXPADDEDBORDER`; top and bottom corner areas are excluded
  because they are diagonal-resize regions.
- `HorizontalResizeDoubleClickState` (`:124`) records the target, edge, position,
  and timestamp of the first click. It recognizes a second click as a double-click
  when it meets `GetDoubleClickTime()`, `SM_CXDOUBLECLK`, and `SM_CYDOUBLECLK`.
- `PollHorizontalResizeDoubleClick` (`:1031`, every 15 ms) detects left-button
  press/release transitions with `GetAsyncKeyState(VK_LBUTTON)`, advances that
  state machine, and calls `ExpandWindowHorizontally` when the double-click is
  confirmed on release.
- `ExpandWindowHorizontally` (`:999`) keeps top and bottom unchanged and expands
  only width to the work-area width: the horizontal counterpart of Windows'
  normal “double-click top edge to maximize vertically” behavior.

## 10. Tray Icon Lifecycle

- `CreateTrayIcon` (`:2023`) calls `Shell_NotifyIconW(NIM_ADD)` and then requests
  `NOTIFYICON_VERSION_4` using `NIM_SETVERSION` for modern notification behavior,
  such as `WM_CONTEXTMENU` notifications.
- In `MainWndProc`, `WM_TRAYICON` handling compares `LOWORD(lParam)`, because with
  `NOTIFYICON_VERSION_4` the notification code occupies the low word and cursor
  coordinates occupy the high word. This avoids missing right-clicks away from
  coordinate (0,0).
- `ShowTrayMenu` (`:1828`) provides About, “Start automatically with Windows”
  (checked from current state; §11), and Exit. It calls `SetForegroundWindow`
  before displaying the menu and posts `WM_NULL` afterward, a standard idiom that
  lets the menu close correctly when it loses focus.
- **Recovery** (`ScheduleTrayIconRecovery`, `:1870`) follows the same path in
  four cases: call `RestoreTrayIcon()` immediately, then retry up to
  `kTrayRecoveryRetryCount` (four) times at
  `kTrayRecoveryRetryIntervalMs` (500 ms):
  1. Receipt of Explorer's `TaskbarCreated` broadcast after Explorer restarts.
  2. `WTS_SESSION_UNLOCK` in `WM_WTSSESSION_CHANGE`.
  3. Resume from sleep via `WM_POWERBROADCAST`
     (`PBT_APMRESUMEAUTOMATIC` or `PBT_APMRESUMESUSPEND`).
  4. Process startup in `wWinMain`, because a logon task can run before Explorer's
     notification area has initialized.
- `RestoreTrayIcon` (`:2049`) tries `NIM_MODIFY` first; if the existing registration
  survives, that is enough. On failure, it re-adds with `NIM_ADD`.

## 11. Startup: Windows Task Scheduler (`RegisterStartupTask`, etc.)

As explained in §2-1 of `01_`, Task Scheduler replaces the registry `Run` key so
the program can launch elevated at logon.

- `ConnectTaskScheduler` (`:1580`) acquires `ITaskService` and `ITaskFolder` COM
  objects and connects to the root (`\`) folder.
- `IsStartupEnabled` (`:1608`) verifies that the task exists and is enabled, and
  also that its `IExecAction` path matches the **current executable path**. If the
  executable is moved, reporting startup as off is intentional.
- `RegisterStartupTask` (`:1670`) configures:
  - `IPrincipal`: `TASK_LOGON_INTERACTIVE_TOKEN` + `TASK_RUNLEVEL_HIGHEST`, to run
    elevated for an interactive logon.
  - `ITaskSettings`: enabled, start when available, and no start/stop restrictions
    based on battery power.
  - Trigger: `TASK_TRIGGER_LOGON`.
  - Action: `TASK_ACTION_EXEC` using `GetCurrentExecutablePath`.
  - Registration/update through `RegisterTaskDefinition(..., TASK_CREATE_OR_UPDATE, ...)`.
- `DeleteStartupTask` (`:1793`) removes the task. `ERROR_FILE_NOT_FOUND` is
  considered success because the desired state is already achieved.
- `DeleteLegacyStartupValue` (`:1572`) removes remnants of prior versions using
  the registry `Run` key (`kLegacyRunKeyPath`); both registration and deletion call it.
- If a tray-menu toggle fails, `SetStartupEnabled` (`:1818`) reports it through
  `MessageBoxW`.

## 12. Diagnostic Logging

- Line format: `YYYY-MM-DD HH:MM:SS.mmm tid=<thread ID> event=<event name>
  hwnd=0x<address> pid=<process ID> <additional data>` (`LogDiagnostic` /
  `LogWindowDiagnostic`, `:234` / `:255`).
- Log file: `%LOCALAPPDATA%\WindowPosButton\diagnostic.log`. At startup, the
  existing log is moved to `diagnostic-prev.log` (one previous generation only),
  then a fresh log begins (`InitializeDiagnosticLog`, `:274`).
- `LogLocationEventThrottled` (`:317`) and `LogReorderEventThrottled` (`:337`)
  reduce potentially flood-prone `EVENT_OBJECT_LOCATIONCHANGE` and
  `EVENT_OBJECT_REORDER` logs to one summary line per second.
- Logging remains enabled in Release builds; there is no conditional compilation.
  The resulting performance issue and its fixes—keeping the file handle open and
  throttling REORDER—are documented in §2 of `03_`.

## 13. Resources and Build

- `WindowPosButton.rc` contains only the application icon. There is no separate
  `VERSIONINFO` resource; the version string is managed by the `kAppVersion` code
  constant (`:79`). DPI awareness is configured in code through
  `SetProcessDpiAwarenessContext`, not a manifest.
- Linked libraries are `dwmapi`, `ole32`, `oleaut32`, `shell32`, `taskschd`, and
  `shcore` (the `#pragma comment(lib, ...)` directives near the top of the file).
