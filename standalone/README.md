# Dynamic Island for Windows — standalone EXE

This is Windhawk's **Dynamic Island for Windows** mod (v1.4.0 source) built as a
regular standalone executable that needs **no Windhawk at all**.

    out\DynamicIsland.exe   (~1.9 MB, x64, static C++ runtime)

## Run

Double-click `DynamicIsland.exe`. On first run it creates next to itself:

- `island.ini` — every setting with its default value, ready to edit by hand
- `storage\state.ini` — runtime state (hidden/pinned/progress, file tray links)

It also registers a **Windows startup entry** (`Behavior.RunOnStartup` is on by
default), so the island relaunches automatically after every reboot — see
[Run on startup](#run-on-startup-auto) below. Settings reload **live** when you
save `island.ini` (within ~1 second).

## Keyboard shortcuts

| Shortcut | Action | Configurable via |
|---|---|---|
| **Ctrl+Alt+D** | Instantly hide / show the island (zero-CPU while hidden) | `Shortcuts.HideShowHotkeyEnabled` / `HideShowModifiers` / `HideShowKey` |
| **Ctrl+B** | Toggle **click-through mode** (see below) | `Shortcuts.ClickThroughHotkeyEnabled` / `ClickThroughModifiers` / `ClickThroughKey` |
| **Ctrl + hover** | Momentary click-through while held | — |
| **Ctrl+Alt+Space** | Quick Lookup search box (requires `Modules.QuickLookup=1`) | `Shortcuts.LookupModifiers` / `LookupKey` |
| **Scroll** | Switch dashboard tabs (media, calendar, weather, hardware, …) | — |
| **Right-click** | Island menu: themes, lyrics sync, pin, Open Settings | — |
| **Ctrl+Alt+Shift+X** | Quit the island | — |

## Click-through mode (Ctrl+B)

Press the click-through hotkey (default **Ctrl+B**) and the island stops
existing for your mouse: hover, clicks and drags pass straight through to
whatever is beneath it — even while the island is expanded or hovered — and the
pill fades to **30% opacity** so you can still see it. Press the hotkey again to
restore normal behavior and your previous transparency.

Holding **Ctrl** while hovering does the same thing temporarily, releasing when
you move away.

## Run on startup (auto)

On by default. Every launch refreshes the per-user registry entry
(`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` → `DynamicIsland`), so the
entry self-heals if you move the exe folder — no admin rights needed. Toggle it
in the settings page (**Behavior & Visibility → Run on startup**): turning it
off deletes the entry cleanly, and the change applies live without a restart.
No other registry or filesystem footprint exists — deleting the folder removes
the app entirely.

## Settings — one-click save

Right-click the island → **Open Settings**. The exe spins up a tiny loopback
HTTP server (`127.0.0.1`, random port, nothing external) and opens
`settings.html` from disk next to the exe, so the page can **save `island.ini`
directly with one click** — every section (Appearance, Behavior, Animations,
Modules, Themes, Indicators, Shortcuts) reloads into the island live.

If `settings.html` is opened as a plain file instead, the page falls back to
downloading `island.ini`, which you can drop next to the exe manually.

## Quit / control

| Action                          | How                                      |
|---------------------------------|------------------------------------------|
| Quit the island                 | `DynamicIsland.exe --quit` or **Ctrl+Alt+Shift+X** |
| Debug log to `island.log`       | start with `DynamicIsland.exe --log`     |
| Custom settings file            | set env `DYNAMIC_ISLAND_INI=<path>`      |
| Portable data dir               | set env `DYNAMIC_ISLAND_DIR=<dir>`       |

A second double-click when it is already running simply does nothing
(single-instance). If the exe folder is read-only, data falls back to
`%LOCALAPPDATA%\DynamicIsland`.

## The Coding Terminal indicator (new)

The island lights a purple dot (default `#7C5CFF`) showing the agent name
whenever a coding agent or AI CLI is running, detected two ways:

1. **Process snapshot** — agent executables with no window (ChatGPT tray, etc.)
2. **Terminal window titles** — only terminal hosts are scanned
   (Windows Terminal, conhost, cmd, PowerShell, pwsh), avoiding browser-tab
   false positives.

Keys in `island.ini`:

    Indicators.CodingDot=1                     ; on/off
    Indicators.CodingDotHex=#7C5CFF            ; dot colour
    Indicators.CodingShowName=1                ; show agent name next to the dot
    Indicators.CodingTab=1                     ; Agents dashboard tab (below)
    Indicators.CodingList=opencode;claude;codex;gemini;aider;kilo;chatgpt.exe

The list is semicolon-separated, case-insensitive substring matching against
process names and terminal titles. Optional custom label per entry:

    Indicators.CodingList=opencode;claude|Claude Code;code.exe|Kilocode

Multiple agents at once render as `opencode +1`. Plain shells (an open cmd or
PowerShell with no agent inside) keep the dot dark.

## The Agents tab (new)

While agents are detected, the dashboard wheel gains an **Agents** tab in the
style of the [Agent Notch](https://github.com/KurianJose7586/notch) project:
hover the island, scroll (or click the pagination dots) to reach it. Each row
shows:

- a **state dot** — green while that agent's process is burning CPU
  ("Working"), grey while parked ("Idle")
- the agent label plus a subtitle with the **host terminal** and the
  process' uptime
- the state word on the right, tinted like the dot

A **click on the Agents tab jumps to that agent's terminal** (the linked
Windows Terminal / cmd / PowerShell window). Start a terminal whose title
contains a `CodingList` keyword — `cmd /k title opencode` — to see it appear
live.

The tab only exists in the wheel while at least one agent is running, so plain
shells never add it. Disable it with `Indicators.CodingTab=0`; the collapsed
dot+name chip stays available via `Indicators.CodingDot`.

## Tweaking beyond defaults

Every default shipped in the Windhawk YAML block (`Indicators.*`, `Modules.*`,
`Behavior.*`, `Animations.*`, `Themes.*`, …) is pre-written to `island.ini`.
Delete a line to fall back to the baked-in default.

## Build (optional)

    powershell -ExecutionPolicy Bypass -File build.ps1

Requires only Windhawk's bundled Clang 20 toolchain (`Compiler\`); no Windhawk
runtime. The produced exe imports only system DLLs — see the notes inside
`build.ps1` for the kernel32/libuuid/static-libc++ linking details.

## Source layout

- `generate_defaults.ps1` — regenerates `island_defaults.h` from the mod's YAML
  settings block
- `island_standalone_api.h` — the Wh_* API shim (INI settings, storage, logging)
- `island_main.cpp` — entry point, single-instance, INI watcher, loopback
  settings server, startup-entry registration
- `island_defaults.h` — generated defaults table (do not edit by hand)
- `settings.html` — the settings dashboard; served by the exe over
  `127.0.0.1:<port>` for one-click saves. Keep it next to `DynamicIsland.exe`.
- `build.ps1` — one-command build (see above)
- `..\public\` — agent logos (`<match>.png`, e.g. `opencode.png`) shown on the
  Agents tab; the exe looks for a `public` folder next to itself
- `..\mod\dynamic-island-for-windows.wh.cpp` — the mod source: byte-for-byte the
  original except one small `#ifdef ISLAND_STANDALONE` bootstrap block, so it
  still compiles as a Windhawk mod unchanged
