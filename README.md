# Dock Deck for OBS

Switch OBS dock layouts with one-click tabs, hotkeys, Stream Deck, or scene changes. Lightweight, Windows + Linux.

Save your OBS dock arrangements as named layouts and switch between them from a
slim tab strip under the menu bar. Windows and Linux. GPL-2.0-or-later.

Scaffolded from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
with the macOS bits removed. The Qt6 frontend API (`ENABLE_FRONTEND_API`, `ENABLE_QT`) is on.

## Features (v0.2.0)

- **Layouts**: a layout is a named snapshot of OBS's docks: where every dock
  sits (`QMainWindow::saveState()`), plus which docks are shown or hidden.
- **Tab strip** directly under the menu bar: one tab per layout, plus a small
  **+** to save the current docks as a new layout. Drag tabs to reorder them.
- **Right-click a tab**: *Save Current Docks Into This Layout*, *Rename...*,
  *Delete...*. Double-click a tab to rename it.
- **Tools → Dock Deck**: switch layouts, create or save layouts, show or hide
  the tab strip, and turn *Remember Dock Changes Per Layout* on or off.
- **Remember Dock Changes Per Layout** (on by default): when you move docks
  around, the active layout keeps those changes, like pages. Turn it off if you
  only want layouts to change when you save them explicitly.
- **Safe restore**: the active layout is re-applied after OBS finishes loading
  (so docks added by other plugins exist). A missing dock is skipped, and a dock
  that is newer than a layout is left as it is.

Window size and position are not part of a layout; only the docks are.
The placeholder **Docks → Dock Deck** panel from 0.1.0 is gone; the tab strip
and the Tools menu replace it.

### Where settings are stored

One global file (not per profile or scene collection yet):

- Linux: `~/.config/obs-studio/plugin_config/obs-dock-deck/layouts.json`
- Windows: `%APPDATA%\obs-studio\plugin_config\obs-dock-deck\layouts.json`

If the file is ever unreadable it is kept as `layouts.json.bad`, and Dock Deck
starts fresh with a single **Main** layout.

## Usage

1. Start OBS. On the first run, your current docks become the **Main** tab.
2. Arrange docks the way you want for another task, then click **+** and name
   it (for example "Gaming"). That arrangement is now its own tab.
3. Click tabs to switch. With *Remember Dock Changes Per Layout* on, changes you
   make on a tab stay with that tab.
4. To hide the strip, right-click it → *Hide Tab Strip*. Bring it back with
   **Tools → Dock Deck → Show Tab Strip**.

## For developers

Every layout switch goes through one function,
`DockDeck::switchToLayout(name, source)` in `src/dock-deck.cpp`. It is safe to
call from any thread and before OBS finishes loading. Future triggers (hotkeys,
obs-websocket, scene links) and the optional fade will call or hook into it.

| File | What it does |
| --- | --- |
| `src/plugin-main.cpp` | Module entry points and OBS frontend events |
| `src/dock-deck.*` | Layout switching, startup restore, menus, dialogs |
| `src/tab-strip.*` | The tab strip toolbar (view only; emits signals) |
| `src/layout-store.*` | Layout data model and JSON load/save |

## Build on Linux (against the system OBS)

Needs `obs-studio` (headers + libobs/obs-frontend-api CMake configs), `qt6-base`,
`cmake`, `ninja`, and a C/C++ compiler.

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

Output: `build_x86_64/obs-dock-deck.so`

## Install for testing on Linux (per user, no sudo)

```bash
D=~/.config/obs-studio/plugins/obs-dock-deck
mkdir -p "$D/bin/64bit" "$D/data"
install -m755 build_x86_64/obs-dock-deck.so "$D/bin/64bit/"
cp -r data/. "$D/data/"
```

Restart OBS, then check the newest log:

```bash
grep -h 'obs-dock-deck' "$(ls -t ~/.config/obs-studio/logs/*.txt | head -1)"
```

You should see `[obs-dock-deck] plugin loaded successfully (version 0.2.0)`,
and the Dock Deck tab strip should appear under the menu bar.

## Build on Windows

Needs Visual Studio 2022 and CMake 3.28+. The template downloads the OBS sources,
obs-deps, and Qt6 listed in `buildspec.json` on first configure.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Copy the built `obs-dock-deck.dll` to
`C:\ProgramData\obs-studio\plugins\obs-dock-deck\` (OBS 33+ layout; the old `bin\64bit` folder is flagged Legacy) and `data\` to
`C:\ProgramData\obs-studio\plugins\obs-dock-deck\data\`.

## CI

GitHub Actions builds Ubuntu and Windows (`.github/workflows`).

## Roadmap

1. ✅ Empty plugin loads in OBS (Linux + Windows).
2. 🧪 Save/load dock layouts with `QMainWindow::saveState` / `restoreState`, plus a
   minimalist tab strip under the menu bar, one tab per layout. *(built, in testing)*
3. Per-layout hotkeys (Stream Deck) and obs-websocket vendor requests for remote
   switching (bots like Streamer.bot/SAMMI can expose it to chat moderators).
4. Link layouts to scenes and scene collections for automatic switching.
5. Optional quick fade when switching layouts (off while streaming/recording by
   default, can be turned off completely).
6. Scene Organizer dock with search and nested folders.
