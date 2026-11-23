# Dock Deck for OBS

Switch OBS dock arrangements (Decks) with one-click tabs, hotkeys, Stream Deck, or scene changes. Lightweight, Windows + Linux.

Save your OBS dock arrangements as named **Decks** and switch between them from
slim tabs under the menu bar. Windows and Linux. GPL-2.0-or-later.

Scaffolded from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
with the macOS bits removed. The Qt6 frontend API (`ENABLE_FRONTEND_API`, `ENABLE_QT`) is on.

## Terms

- **Deck**: one named dock arrangement, shown as one tab under the menu bar.
- **Deck Collection**: all of your Decks together.

## Features (v0.2.2)

- **Decks**: a Deck is a named snapshot of OBS's docks: where every dock sits
  (`QMainWindow::saveState()`), which docks are shown or hidden, and each dock's
  title.
- **Deck tabs** directly under the menu bar: one tab per Deck, plus a small
  **+** to save the current docks as a new Deck. Drag tabs to reorder them.
- **Right-click a Deck tab**: *Save Current Docks Into This Deck*,
  *Rename Deck...*, *Delete Deck...*, *Export Deck...*. Double-click a tab to
  rename it.
- **Tools → Dock Deck**: switch Decks, *New Deck From Current Docks...*,
  *Export Deck Collection...*, *Import Decks...*, and the settings
  *Show Deck Tabs*, *Remember Dock Changes Per Deck* and
  *Show Placeholders for Missing Docks*.
- **Remember Dock Changes Per Deck** (on by default): when you move docks
  around, the active Deck keeps those changes, like pages. Turn it off if you
  only want Decks to change when you save them explicitly.
- **Export / import**: *Export Deck...* saves one Deck, *Export Deck Collection...*
  saves all of them (default file names `<Deck name>.dockdeck.json` and
  `Deck Collection.dockdeck.json`). *Import Decks...* merges a file into your
  Deck Collection. When an imported name already exists you choose **Replace**
  (keeps the tab's place), **Keep Both** (adds "Name (2)"), or **Skip**, with an
  option to do the same for the rest. Damaged entries are ignored and bad files
  are rejected with a message; nothing changes until a file has been read
  successfully.
- **Placeholders for missing docks** (on by default): if a Deck shows a dock
  that isn't available (plugin not installed, a browser dock from another
  computer, or a plugin that loads late), a placeholder panel holds its spot and
  says *Missing dock: <title>* with a hint to install the plugin that provides
  it or close the panel. When the real dock appears, it moves into that spot and
  the placeholder disappears. Switching to a Deck that doesn't use the dock
  removes the placeholder.
- **Safe restore**: the active Deck is re-applied after OBS finishes loading
  (so docks added by other plugins exist). A missing dock never causes an error,
  and a dock that is newer than a Deck is left as it is.

Window size and position are not part of a Deck; only the docks are.

**Sharing Decks between computers:** Decks refer to docks by their internal
names. Docks from plugins (or browser docks) that don't exist on the importing
computer can't appear there: they get a placeholder (if that setting is on) and
the import summary lists them. Install the same plugins / create the same
browser docks to get them back.

### How placeholders work

Before a Deck is applied, Dock Deck adds a plain Qt dock with the missing
dock's internal name for each missing dock the Deck shows, so Qt's
`restoreState()` puts it exactly where the real one belongs. Placeholders are
not registered with OBS (no Docks-menu entry). Because they use the real dock's
name, anything that saves the window layout (OBS on exit, Dock Deck's
auto-save) records the real dock's spot, so nothing foreign is left behind.
When a new dock is added to the OBS window, Dock Deck checks whether it matches
a placeholder; if so it captures the window with the placeholder still in
place, removes the placeholder and restores, which moves the real dock in.

### Where settings are stored

One global file (not per profile or scene collection yet):

- Linux: `~/.config/obs-studio/plugin_config/obs-dock-deck/layouts.json`
- Windows: `%APPDATA%\obs-studio\plugin_config\obs-dock-deck\layouts.json`

The file keeps its old name and format so earlier versions' settings load
unchanged. Export files wrap the same entries as
`{"format": "dock-deck-layouts", "version": 1, "layouts": [{"name", "state", "docks", "titles"}]}`,
where `state` is base64 `QMainWindow::saveState()` data, `docks` maps each
dock name to shown/hidden, and `titles` (optional, new in 0.2.2) maps each dock
name to its title. Import also accepts a copied `layouts.json` and files from
0.2.1 (no titles); 0.2.1 can read 0.2.2 files too.

If the file is ever unreadable it is kept as `layouts.json.bad`, and Dock Deck
starts fresh with a single **Main** Deck.

## Usage

1. Start OBS. On the first run, your current docks become the **Main** Deck.
2. Arrange docks the way you want for another task, then click **+** and name
   it (for example "Gaming"). That arrangement is now its own Deck.
3. Click Deck tabs to switch. With *Remember Dock Changes Per Deck* on, changes
   you make on a Deck stay with that Deck.
4. To move Decks to another computer, use **Tools → Dock Deck → Export Deck
   Collection...** (or right-click a tab → *Export Deck...*) there and
   **Import Decks...** here.
5. To hide the tabs, right-click them → *Hide Deck Tabs*. Bring them back with
   **Tools → Dock Deck → Show Deck Tabs**.

## For developers

Every Deck switch goes through one function,
`DockDeck::switchToDeck(name, source)` in `src/dock-deck.cpp`. It is safe to
call from any thread and before OBS finishes loading. Future triggers (hotkeys,
obs-websocket, scene links) and the optional fade will call or hook into it.

| File | What it does |
| --- | --- |
| `src/plugin-main.cpp` | Module entry points and OBS frontend events |
| `src/dock-deck.*` | Deck switching, startup restore, placeholders, menus, dialogs |
| `src/deck-strip.*` | The Deck tabs toolbar (view only; emits signals) |
| `src/deck-store.*` | Deck data model, config load/save, export/import file format |

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

You should see `[obs-dock-deck] plugin loaded successfully (version 0.2.2)`,
and the Deck tabs should appear under the menu bar.

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

GitHub Actions builds Ubuntu 26.04 and Windows (`.github/workflows`) on every push to `main`.

## Roadmap

1. ✅ Empty plugin loads in OBS (Linux + Windows).
2. ✅ Save/load dock arrangements with `QMainWindow::saveState` / `restoreState`,
   plus minimalist tabs under the menu bar, one tab per Deck.
3. Per-Deck hotkeys (Stream Deck) and obs-websocket vendor requests for remote
   switching (bots like Streamer.bot/SAMMI can expose it to chat moderators).
4. Link Decks to scenes and scene collections for automatic switching.
5. Deck packs: export/import a Deck together with selected scenes, sources and
   media files. Per-item checkboxes on both export and import, plus a master
   *Select all / none* checkbox and per-group (scenes, sources, media)
   all/none checkboxes (tri-state). Media that is left out (for example
   purchased overlays) imports as an empty, unpointed source for the user to
   fill in. Plugins the pack needs but that are missing are listed. Secrets
   (stream keys, passwords, tokens) are never included.
6. Optional quick fade when switching Decks (off while streaming/recording by
   default, can be turned off completely).
7. Scene Organizer dock with search and nested folders.
