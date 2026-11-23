# Dock Deck for OBS

Switch OBS dock layouts with one-click tabs, hotkeys, Stream Deck, or scene changes. Lightweight, Windows + Linux.

Save your OBS dock arrangements as named layouts and switch between them from a
slim tab strip under the menu bar. Windows and Linux. GPL-2.0-or-later.

Scaffolded from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
with the macOS bits removed. The Qt6 frontend API (`ENABLE_FRONTEND_API`, `ENABLE_QT`) is on.

## Status

Milestone 1: an empty plugin that logs on load/unload and adds a placeholder
**Dock Deck** dock (Docks menu).

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

You should see `[obs-dock-deck] plugin loaded successfully (version 0.1.0)`,
and **Docks → Dock Deck** should be listed.

## Build on Windows

Needs Visual Studio 2022 and CMake 3.28+. The template downloads the OBS sources,
obs-deps, and Qt6 listed in `buildspec.json` on first configure.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Copy the built `obs-dock-deck.dll` to
`C:\ProgramData\obs-studio\plugins\obs-dock-deck\bin\64bit\` and `data\` to
`C:\ProgramData\obs-studio\plugins\obs-dock-deck\data\`.

## CI

GitHub Actions builds Ubuntu and Windows (`.github/workflows`).

## Roadmap

1. Empty plugin loads in OBS (Linux + Windows).
2. Save/load dock layouts with `QMainWindow::saveState` / `restoreState`, plus a
   minimalist tab strip under the menu bar, one tab per layout.
3. Per-layout hotkeys (Stream Deck) and obs-websocket vendor requests for remote
   switching (bots like Streamer.bot/SAMMI can expose it to chat moderators).
4. Link layouts to scenes and scene collections for automatic switching.
5. Scene Organizer dock with search and nested folders.
