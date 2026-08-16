# DPI Button-Size Inconsistency and CPU-Usage Improvements

Work date: 2026-08-13
Target file: `WindowPosButton/WindowPosButton.cpp`

This document records two changes completed in one session: (1) inconsistent overlay button sizes when moving a window between monitors with different DPI scales, and (2) unnecessary continuous CPU and disk I/O from diagnostic logging.

## 1. Inconsistent Button Sizes When Crossing a DPI Boundary

### Background

WindowPosButton draws four overlay buttons next to each target window's title bar: Snap Left, Snap Right, Center 80%, and Move to Next Monitor. They are separate layered `WS_POPUP` windows. When a user moves a window between monitors with different DPI scales, such as 175% and 100%, the buttons should retain a consistent appearance. Static analysis identified a structural cause that could reproduce the inconsistency.

### Cause

`ComputeButtonRect` in `WindowPosButton.cpp` reads the following values sequentially and non-atomically from different subsystems:

1. `DwmGetWindowAttribute(DWMWA_CAPTION_BUTTON_BOUNDS)`: managed by DWM and updated only after the target window redraws.
2. `GetWindowRect`: the outer window rectangle.
3. `GetMonitorDpi` (`MonitorFromWindow` + `GetDpiForMonitor`): DPI for the nearest monitor, whose result can change from one frame to the next while the window spans a boundary.
4. `GetDpiForWindow`: the target window's own DPI, which can update at a different time depending on the target application's DPI-awareness level.

The calculation combines them as `scale = monitorDpi / windowDpi`. While a window is being dragged, these values can refer to different moments. The button size can briefly become incorrect and, in some cases, remain incorrect after the drag ends. `EVENT_OBJECT_LOCATIONCHANGE` repeatedly triggers recalculation (`DispatchUpdate` → `ComputeButtonRect`) during the drag, magnifying the effect because the final event snapshot becomes the final size. `MoveWindowToNextMonitor` also previously did not resize windows according to their destination monitor, making the race easier to expose (see §3 for the subsequent correction).

### Applied Fix

- **Remove duplicate `MonitorFromWindow` calls:** `ComputeButtonRect` previously queried `MonitorFromWindow` separately for DPI lookup and work-area clamping. It now reuses one `HMONITOR`, reducing the chance that the DPI and work area come from different “nearest monitor” decisions while dragging across a boundary.
- **Add a settle-confirmation timer:** every `DispatchUpdate` rearms a one-shot, per-window timer, `kOverlaySettleDelayMs = 150 ms`. Calling `SetTimer` again with the same ID resets its countdown, so the timer fires exactly once 150 ms after the **last** event for that window. By then, DWM caption updates and the target window's DPI-change processing will normally be complete. A clean confirmation calculation overwrites a mismatched snapshot captured during the drag. The callback uses `rearmSettleTimer=false` to avoid rearming itself forever.
- **Reuse the existing timer path:** the change reuses the same per-window timer slot and `WM_TIMER` handling already used for retrying a restoring window, keeping the scope small.

### Limitation

The four-value calculation is still not truly atomic. This change stabilizes the finally visible size by confirming it after the system settles. Visual validation with a real 175% ↔ 100% monitor drag was not performed because the development environment did not have the required hardware.

## 2. CPU Usage: Synchronous File I/O from Always-On Diagnostic Logging

### Background

The code was reviewed because lowering CPU use was important.

### Cause

- `InitializeDiagnosticLog()` runs unconditionally from `wWinMain`; with no `_DEBUG`/`NDEBUG` conditional compilation, logging was **always enabled in Release builds** as well.
- `AppendDiagnosticLine` opened the file with `_wfopen_s`, wrote a line with `fputs`, and closed it with `fclose` for every log entry. That incurred a CreateFile/CloseHandle cycle each time.
- All five `SetWinEventHook` subscriptions were system-wide (`hProcess=0`, `idThread=0`). In particular, `EVENT_OBJECT_REORDER` is produced whenever the desktop top-level Z-order changes—Alt+Tab, tooltips, menus, and many events unrelated to this application. It was not throttled, so every event ran `LogWindowDiagnostic` and a file open/write/close. All of that work is synchronous on the same main UI thread that renders overlays and processes timers (`WINEVENT_OUTOFCONTEXT`).

Thus, ordinary desktop use could generate dozens of synchronous disk-I/O operations per second on the main thread for activity unrelated to WindowPosButton.

### Applied Fix

- **Keep the log file handle open:** open `g_diagnosticLogFile` once in `InitializeDiagnosticLog` and close it only in `ShutdownDiagnosticLog` after the message loop ends. `AppendDiagnosticLine` now writes to the open handle with `fputs` + `fflush`, removing the per-line CreateFile/CloseHandle round trip while retaining `fflush` for durability.
- **Throttle `EVENT_OBJECT_REORDER`:** add `LogReorderEventThrottled` using the same one-summary-line-per-second approach already used for `EVENT_OBJECT_LOCATIONCHANGE`. Because a REORDER `hwnd` usually identifies the desktop container rather than a particular target, one global state object, `g_reorderEventLog`, is sufficient.

### Verification

`MSBuild WindowPosButton.vcxproj /p:Configuration=Release /p:Platform=x64` compiled successfully. CPU and disk reduction during actual use still requires verification in a deployed environment.

## 3. Follow-Up: DPI Adjustment During Next-Monitor Movement and Worker Creation

To ensure CPU use would not increase, the follow-up also completed work on `MoveWindowToNextMonitor`, which was only a contributing factor in §1, and on the remaining `CreateThread` implementation after §2.

### 3-1. `MoveWindowToNextMonitor`: Preserve Work-Area Percentages

Previously, moving a window to the next monitor only **clamped** its pixel size to the destination work area. An initial fix rescaled it using the source/destination DPI ratio (`dpiScale = destinationDpi / sourceDpi`), but the intended behavior was not a DPI ratio. It was to preserve the same **percentage of the monitor work area**. For example, a window snapped to the left and occupying 50% of the current monitor should remain left-aligned and occupy 50% of the destination monitor. DPI ratio and work-area-resolution ratio generally differ, so DPI-based scaling could not fulfill this requirement exactly.

The implementation now obtains the source visible frame with `GetVisibleFrameRect` and calculates `leftRatio`, `topRatio`, `widthRatio`, and `heightRatio` relative to the source monitor work area, clamping each to `[0, 1]`. It applies those same ratios to the destination work area, for example `destination.work.left + leftRatio * destinationWidth`. Other placement buttons already use this model in `PerformButtonAction` (`SnapLeft`, `SnapRight`, `Center80`, and `targetWidth = monWidth * widthRatio`), so next-monitor movement now follows the same rule. The obsolete source/destination `GetMonitorDpi` calls and the old `ScaleWindowPosition` helper, whose only two call sites were replaced, were removed.

Maximized windows continue to work without special changes. A maximized `sourceRect` already occupies nearly the entire source monitor, so the same ratio formula derives nearly the entire destination monitor. The existing `if (wasMaximized)` branch moves the restored placement and calls `ShowWindow(SW_MAXIMIZE)` to maximize on the destination monitor again.

#### 3-1-1. Additional Issue Found on Physical Hardware: Size Changed Again After Placement

Testing on a physical dual-monitor setup (175% left, 100% right) confirmed that the ratio calculation itself was correct: the reported placement and size were correct after moving. However, the window **changed size once more immediately after placement**.

The cause was Windows DPI virtualization, not this utility. When a window crosses to a monitor with a different DPI, Windows sends the target process `WM_DPICHANGED`. A DPI-aware target generally responds by changing its own size and position. Its scaling basis is normally a DPI ratio, which differs from this utility's work-area-percentage basis; the window therefore appears to change again immediately after WindowPosButton positions it. Since that message handling belongs to the target process, WindowPosButton cannot directly prevent it.

**First attempt: reapply at the end (rejected).** The first implementation stored the target rectangle after positioning a non-maximized window and reapplied the same rectangle once after `kMonitorMoveReassertDelayMs = 300 ms`. Physical testing showed a clear sequence: correct placement → incorrect size for 300 ms → correct placement again. Raising the reaction speed by reapplying on the next `EVENT_OBJECT_LOCATIONCHANGE` still looked like two adjustments. Even a single exposed frame in which the target application renders its own DPI-based size reads as a second resize to a user. Microsoft's DPI-awareness guidance tells applications to apply the rectangle suggested by `WM_DPICHANGED`'s `lParam`, so a well-behaved DPI-aware application can make the effect more reliable rather than eliminating it. No response speed can fundamentally remove the exposed intermediate frame.

**Second attempt: hide, then reveal only the final state (adopted).** For a non-maximized window, the program now calls `ShowWindow(hwnd, SW_HIDE)` immediately before applying the target rectangle. While hidden, the target may react to `WM_DPICHANGED` and adjust itself any number of times, but DWM does not composite it to the screen. After `kMonitorMoveReassertDelayMs` (300 ms), `RevealMonitorMovePlacementIfPending` checks that the window is still in the expected non-minimized/non-maximized state, reapplies the desired rectangle with `SetWindowPosForVisibleFrame`, and only then calls `ShowWindow(SW_SHOW)` plus `SetForegroundWindow`. The user sees the window appear once, at the intended position and size, after a small delay; the intermediate transitions are invisible.

The event-driven “reapply only when a rectangle comparison detects a change” path is no longer needed and was removed. While hidden, extra reapplications have no visual benefit. `MoveWindowToNextMonitor` also must not call `DispatchUpdate(hwnd)` immediately after hiding: `ComputeUpdatePlan` would consider the target not ready, and `ApplyUpdateResult` could rearm the shared timer slot at `kOverlaySettleDelayMs` (150 ms), incorrectly shortening the intended 300-ms wait. Instead, after reveal, the normal `EVENT_OBJECT_SHOW` naturally triggers `DispatchUpdate`.

Windows that were maximized are excluded from the hide/reapply path. Calling `ShowWindow(SW_MAXIMIZE)` recomputes them for the destination work area, their intermediate state was not reported as a problem in physical testing, and changing that unverified behavior would carry unnecessary risk.

### 3-2. Measurement Workers: `CreateThread` → OS Thread Pool (`QueueUserWorkItem`)

For each dispatch, `MeasureThreadProc` previously created and immediately discarded a dedicated OS thread with `CreateThread`. Coalescing prevents a flood per window, but frequent events or multiple windows can still create threads often. Thread creation and destruction reserve a stack and allocate/release kernel objects, a large overhead compared with the few DWM/GDI calls that form the actual task.

`CreateThread`/`CloseHandle` were replaced with `QueueUserWorkItem(MeasureThreadProc, param, WT_EXECUTELONGFUNCTION)`. The shared process thread pool can reuse workers, avoiding a create/destroy cycle for every dispatch while preserving the behavior: each window is processed asynchronously and returns its result to the main thread through `WM_OVERLAY_RESULT`. `WT_EXECUTELONGFUNCTION` hints that a slow or hung DWM call—the “pathological case” mentioned by the original comment—should not delay unrelated pool work.

### Verification

`MSBuild WindowPosButton.vcxproj /p:Configuration=Release /p:Platform=x64` compiled successfully. Visual validation of window/button size during 175% ↔ 100% monitor movement and a long-running CPU-use comparison were not performed because of the development-environment constraints.

## Remaining Item (Reference)

- `g_locationEventLogs` can continue accumulating entries for windows that are not tracked. This is a memory concern rather than a CPU concern and was intentionally deferred in this work.
