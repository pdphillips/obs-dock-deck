# Dock Deck for OBS

Switch OBS dock arrangements (Decks) with one-click tabs, hotkeys, Stream Deck, or scene changes. Lightweight, Windows + Linux.

Save your OBS dock arrangements as named **Decks** and switch between them from
slim tabs under the menu bar. Windows and Linux. GPL-2.0-or-later.

Scaffolded from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
with the macOS bits removed. The Qt6 frontend API (`ENABLE_FRONTEND_API`, `ENABLE_QT`) is on.

## Terms

- **Deck**: one named dock arrangement, shown as one tab under the menu bar.
- **Deck Collection**: all of your Decks together.

## Features (v0.3.1)

- **Decks**: a Deck is a named snapshot of OBS's docks: where every dock sits
  (`QMainWindow::saveState()`), which docks are shown or hidden, and each dock's
  title.
- **Deck tabs** directly under the menu bar: one tab per Deck, plus a small
  **+** to save the current docks as a new Deck. Drag tabs to reorder them.
- **Right-click a Deck tab**: *Lock Deck* / *Unlock Deck*,
  *Save Current Docks Into This Deck*,
  *Rename Deck...*, *Delete Deck...*, *Export Deck...*. Double-click a tab to
  rename it.
- **Tools → Dock Deck**: switch Decks, *New Deck From Current Docks...*,
  *Lock All Decks* / *Unlock All Decks*, *Export Deck Collection...*,
  *Import Decks...*, and the settings
  *Show Deck Tabs*, *Remember Dock Changes Per Deck* and
  *Show Placeholders for Missing Docks*.
- **Deck locking**: each tab has a small padlock; click it to lock or unlock
  that Deck (faint open padlock = unlocked, solid closed padlock = locked).
  While a locked Deck is shown, its docks can't be moved, resized, floated or
  closed, and dock changes are not auto-saved into it, so it always comes back
  exactly as you locked it. This works like OBS's own **Docks → Lock Docks**
  (which removes the move/float/close buttons), and additionally blocks
  dragging the splitters between docks. OBS's Lock Docks still works and counts
  as a lock for every Deck: a Deck is locked if either is on. Locked Decks show
  *(Locked)* in **Tools → Dock Deck**, and *Save Current Docks Into This Deck*
  is disabled for them (unlock first). Docks can still be shown/hidden from
  OBS's Docks menu, but on a locked Deck that isn't saved.
- **Remember Dock Changes Per Deck** (on by default): when you move docks
  around, the active Deck keeps those changes, like pages. Turn it off if you
  only want Decks to change when you save them explicitly.
- **Export / import**: *Export Deck...* saves one Deck. *Export Deck Collection...*
  first lists all Decks with checkboxes (all checked, plus *Select all / none*)
  so you choose which Decks go into the file; Export is disabled until at least
  one is checked. Default file names are `<Deck name>.dockdeck.json` and
  `Deck Collection.dockdeck.json`. The **+** dialog also has
  *Import Deck or Deck Collection...*, which closes it and starts the import. *Import Decks...* merges a file into your
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
the import summary lists them (only docks shown by the Decks that were actually
added or replaced in that import). Install the same plugins / create the same
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
`{"format": "dock-deck-layouts", "version": 1, "layouts": [{"name", "state", "docks", "titles", "locked"}]}`,
where `state` is base64 `QMainWindow::saveState()` data, `docks` maps each
dock name to shown/hidden, and `titles` (optional, new in 0.2.2) maps each dock
name to its title, and `locked` (optional, new in 0.3.0, written only when
`true`) marks a locked Deck. Import also accepts a copied `layouts.json` and
files from 0.2.x (no titles / no lock: imported unlocked); older versions read
0.3.0 files too and simply ignore the lock.

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
5. To protect a finished Deck, click the padlock on its tab (or right-click →
   *Lock Deck*). Click it again to make changes.
6. To hide the tabs, right-click them → *Hide Deck Tabs*. Bring them back with
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

You should see `[obs-dock-deck] plugin loaded successfully (version 0.3.1)`,
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

## Manual test checklist (0.3.1)

Export / import:
1. Tools → Dock Deck → *Export Deck Collection...*: every Deck is listed and
   checked. *Select all / none* clears all (Export greys out) and checks all;
   unchecking one Deck makes it partial. Export two Decks, then import the file:
   only those two come in.
2. With a Deck whose dock is missing (placeholder), export a *different* Deck,
   delete it and import it: the summary must not mention the other Deck's
   missing dock.
3. Click **+** → *Import Deck or Deck Collection...*: the name dialog closes and
   the import file picker opens.

From 0.3.0:

Placeholders keep their size:
1. Make a Deck that shows a plugin's dock, quit OBS, remove that plugin, start
   OBS: a *Missing dock* placeholder sits where the dock was.
2. **Tools → Dock Deck → Show Placeholders for Missing Docks** off, then on:
   the placeholder comes back at exactly the same size and place.
3. Switch to another Deck and back (also with the setting off in between): same
   size. Reinstall the plugin: the real dock takes the placeholder's size.

Deck locking:
1. Click the padlock on a tab: it turns solid. Docks lose their float/close
   buttons, can't be dragged out, and the splitters between docks can't be
   dragged.
2. Hide a dock from OBS's Docks menu, switch to another Deck and back: the
   locked Deck comes back unchanged.
3. Right-click the tab: *Unlock Deck*; *Save Current Docks Into This Deck* is
   greyed out while locked. Tools → Dock Deck shows *(Locked)* next to it.
4. *Lock All Decks* / *Unlock All Decks* change every tab's padlock.
5. With every Deck unlocked, turn on OBS's **Docks → Lock Docks**: docks are
   locked (Tools → Dock Deck notes it), changes aren't auto-saved. Turn it off:
   docks are free again. With a Deck locked, toggling OBS's lock on/off leaves
   the Deck locked.
6. Lock a Deck, restart OBS: still locked. Export it and import it on the other
   computer: it arrives locked; 0.2.x files import unlocked.

## CI

GitHub Actions builds Ubuntu 26.04 and Windows (`.github/workflows`) on every push to `main`.

## Roadmap

1. ✅ Empty plugin loads in OBS (Linux + Windows).
2. ✅ Save/load dock arrangements with `QMainWindow::saveState` / `restoreState`,
   plus minimalist tabs under the menu bar, one tab per Deck (since then:
   export/import, placeholders for missing docks, Deck locking).
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
