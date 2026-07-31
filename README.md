# WindowPosButton

WindowPosButton is a native Windows utility that adds compact window-management buttons to the title bars of eligible desktop applications. It makes common placement tasks available directly beside the standard caption buttons.

The application runs in the notification area and requests administrator privileges so it can also work with elevated windows.

## Features

- Adds snap, monitor, and centered-layout controls to eligible window title bars.
- Supports left and right snap widths of 50%, 30%, and 70%.
- Moves windows through connected monitors in physical display-layout order.
- Expands a window across all connected monitors on demand.
- Provides an 80%-width, 16:9 centered layout and a full-height variant.
- Expands a foreground window horizontally by double-clicking its left or right resize edge.
- Offers optional automatic startup through a per-user Task Scheduler logon task.
- Restores the notification-area icon after Explorer restarts, session unlock, and resume events.
- Uses per-monitor DPI awareness and DWM visible-frame bounds for accurate placement.

## Requirements

- Windows 10 or Windows 11 desktop.
- Administrator approval when Windows displays the UAC prompt.
- For building: Visual Studio 2022 with the **Desktop development with C++** workload, the MSVC v143 toolset, and a Windows 10 or later SDK.

## Quick Start

1. Build the `Release|x64` configuration, or use a release executable from a trusted source.
2. Start `WindowPosButton.exe` and approve the UAC prompt.
3. Focus an eligible application window. The overlay controls appear immediately to the left of its native minimize, maximize, and close buttons.
4. Right-click the WindowPosButton notification-area icon to open **About**, enable or disable **Start automatically with Windows**, or choose **Exit**.

The program has no conventional main window. Its notification-area icon is the primary control point.

## User Guide

### Title-Bar Controls

The controls are shown only for visible top-level windows with a normal caption and a minimize button. Tool windows, cloaked windows, minimized windows, and windows without a suitable title bar are excluded.

| Action | Left click | Shift + left click | Right click |
| --- | --- | --- | --- |
| Snap left | Place the window at the left 50% of the current monitor work area. | Place it at the left 30%. | Place it at the left 70%. |
| Snap right | Place the window at the right 50% of the current monitor work area. | Place it at the right 30%. | Place it at the right 70%. |
| Move to next monitor | Move the window to the next connected monitor, preserving its relative position and size where possible. | Same as left click. | Expand the window across the full virtual desktop. |
| Center at 80% | Center a window that is 80% of the current monitor width, using a 16:9 aspect ratio. | Same as left click. | Use 80% width and the full available work-area height. |

The monitor control is hidden when only one monitor is available. Monitor cycling follows the Windows display arrangement from left to right, then top to bottom, and wraps after the last monitor.

### Horizontal Expansion Gesture

For a tracked foreground window that is resizable and not maximized or minimized:

1. Point at the left or right resize edge, away from the corner regions.
2. Double-click the edge using the normal Windows double-click timing.

The window keeps its current vertical position and size, while its visible frame expands to the left and right edges of the current monitor work area. This gesture is limited to the foreground tracked window; the application does not install a global mouse hook.

### Automatic Startup

Enable **Start automatically with Windows** from the notification-area menu to create or update the `WindowPosButton` Task Scheduler task. The task runs at logon for the current user with the highest available privileges. Disable the option to remove that task.

## Build from Source

Open `WindowPosButton.sln` in Visual Studio, select `Release` and `x64`, then build the solution. From a Developer PowerShell, the equivalent command is:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe' .\WindowPosButton.sln /p:Configuration=Release /p:Platform=x64 /m
```

The executable is written to:

```text
x64\Release\WindowPosButton.exe
```

The project is a Unicode C++17 Win32 application. Its Release configuration enables whole-program optimization and requires administrator execution through the embedded application manifest.

## Technical Overview

- `WindowPosButton/WindowPosButton.cpp` contains the application logic, hidden main window, notification-area menu, WinEvent hooks, overlay rendering, placement actions, Task Scheduler integration, and diagnostics.
- `WindowPosButton/WindowPosButton.vcxproj` defines the Visual Studio C++ build configurations and dependencies, including Task Scheduler, COM, and Windows Terminal Services libraries.
- `WindowPosButton/WindowPosButton.rc` and `WindowPosButton.ico` provide the application icon resources.

At runtime, a hidden main window owns the notification-area icon and receives Windows messages. Out-of-context WinEvent hooks track eligible windows and schedule one-shot measurements for overlay updates. The overlay controls are layered, non-activating, unowned windows, preventing cross-process owner relationships and keeping the target application independent of the overlay.

Window placement uses monitor work areas and compensates for invisible resize borders through `DWMWA_EXTENDED_FRAME_BOUNDS`, so the visible frame reaches the intended edges. Per-monitor DPI awareness is enabled with `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`.

## Diagnostics and Troubleshooting

The application writes diagnostic logs to:

```text
%LOCALAPPDATA%\WindowPosButton\diagnostic.log
```

The prior session log is retained as `diagnostic-prev.log` in the same directory.

- **No title-bar controls appear:** Confirm that the target is a visible, normal top-level desktop window with a title bar and minimize button. Some applications with highly customized title bars may not expose compatible caption bounds.
- **The notification-area icon is missing:** Wait briefly after sign-in or an Explorer restart. The application retries registration during startup and reacts to Explorer, unlock, and resume events. Check the diagnostic log if the issue persists.
- **A build cannot replace the executable:** Exit the running application from the notification-area menu, then rebuild. A running executable can lock the Release output.
- **The startup option fails:** Run the application with administrator approval and check that Task Scheduler is available for the current user.

## Privacy and Security

WindowPosButton operates locally. It observes window events and reads window geometry to position its controls; it does not use a global mouse hook, transmit data, or require a network connection. Administrator privileges are requested solely to provide controls for elevated windows and to create the elevated logon task when startup is enabled.

## License

Copyright (c) 2026 jintaeks@gmail.com

This project is licensed under the GNU General Public License v3.0 only (GPL-3.0-only). You may copy, modify, and redistribute it under the terms of the GPL v3. A copy of the license text is available at <https://www.gnu.org/licenses/gpl-3.0.html>.

## Contributing

Contributions are welcome. Please keep changes focused, preserve the non-blocking overlay architecture, build `Release|x64`, and include clear testing notes for window-manager interactions.
