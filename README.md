# Dock Deck for OBS

Switch OBS dock arrangements (Decks) with one-click tabs, hotkeys, Stream Deck, or scene changes. Lightweight, Windows + Linux.

Save your OBS dock arrangements as named **Decks** and switch between them from
slim tabs under the menu bar. Windows and Linux. GPL-2.0-or-later.

Scaffolded from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
with the macOS bits removed. The Qt6 frontend API (`ENABLE_FRONTEND_API`, `ENABLE_QT`) is on.

## Terms

- **Deck**: one named dock arrangement, shown as one tab under the menu bar.
- **Deck Collection**: all of your Decks together.

## Features (v0.8.0)

- **Decks**: a Deck is a named snapshot of OBS's docks: where every dock sits
  (`QMainWindow::saveState()`), which docks are shown or hidden, and each dock's
  title.
- **Deck tabs** directly under the menu bar: one tab per Deck, plus a small
  **+** to save the current docks as a new Deck. Drag tabs to reorder them.
- **Right-click a Deck tab**: *Lock Deck* / *Unlock Deck*,
  *Save Current Docks Into This Deck*,
  *Rename Deck...*, *Delete Deck...*, *Export Deck Pack...*.
  Double-click a tab to
  rename it.
- **Tools → Dock Deck**: switch Decks, *New Deck From Current Docks...*,
  *Lock All Decks* / *Unlock All Decks*, *Export Deck Pack...*,
  *Import Deck Pack...*, and the settings
  *Show Deck Tabs*, *Remember Dock Changes Per Deck* and
  *Show Placeholders for Missing Docks*.
- **Deck locking**: each tab has a small padlock; click it to lock or unlock
  that Deck (faint open padlock = unlocked, solid closed padlock = locked).
  While a locked Deck is shown, its docks can't be moved, resized, floated or
  closed, and dock changes are not auto-saved into it, so it always comes back
  exactly as you locked it. This works like OBS's own **Docks → Lock Docks**
  (which removes the move/float/close buttons), and additionally blocks
  dragging the splitters between docks (the mouse keeps its normal arrow over
  them while locked). OBS's Lock Docks still works and counts
  as a lock for every Deck: a Deck is locked if either is on. Locked Decks show
  *(Locked)* in **Tools → Dock Deck**, and *Save Current Docks Into This Deck*
  is disabled for them (unlock first). Docks can still be shown/hidden from
  OBS's Docks menu, but on a locked Deck that isn't saved.
- **Scene Organizer** dock: search your scenes and sort them into nested
  folders. See [Scene Organizer](#scene-organizer).
- **Switch fade** (on by default): the old dock layout fades out over the new
  one (150 ms). See [Switch fade](#switch-fade).
- **Remember Dock Changes Per Deck** (on by default): when you move docks
  around, the active Deck keeps those changes, like pages. Turn it off if you
  only want Decks to change when you save them explicitly.
- **Export / import**: everything is a **Deck pack** (`*.dockdeck`), whether
  it holds one Deck, several, or Decks with their scenes, sources and media.
  Right-click a tab → *Export Deck Pack...* starts with that Deck checked;
  Tools → Dock Deck → *Export Deck Pack...* starts with every Deck checked.
  The **+** dialog also has *Import Deck Pack...*. A pack with only Decks
  checked is just your dock layouts. See [Deck packs](#deck-packs).
- **Placeholders for missing docks** (on by default): if a Deck shows a dock
  that isn't available (plugin not installed, a browser dock from another
  computer, or a plugin that loads late), a placeholder panel holds its spot and
  says *Missing dock: <title>* with a hint to install the plugin that provides
  it or close the panel. When the real dock appears, it moves into that spot and
  the placeholder disappears. Switching to a Deck that doesn't use the dock
  removes the placeholder.
- **Linked scenes**: a Deck can be linked to a scene; selecting the Deck then
  switches OBS to that scene (see [Linked scenes](#linked-scenes)).
- **Hotkeys** for every Deck, Next / Previous Deck and Toggle Lock (see
  [Hotkeys](#hotkeys)).
- **Remote control** through obs-websocket for Streamer.bot (verified, 1.0.7
  on Linux/Wine), SAMMI, Touch Portal and scripts (see [Remote control](#remote-control-obs-websocket)).
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
unchanged. It holds `{"format": "dock-deck-layouts", "version": 1, "layouts": [{"name", "state", "docks", "titles", "locked"}]}`,
where `state` is base64 `QMainWindow::saveState()` data, `docks` maps each
dock name to shown/hidden, and `titles` (optional, new in 0.2.2) maps each dock
name to its title, and `locked` (optional, new in 0.3.0, written only when
`true`) marks a locked Deck, and `id` (new in 0.4.0) is the Deck's permanent
id. Decks without an id get one when loaded. `scenes` (optional, new in 0.5.0)
maps scene collection name to the linked scene. Deck packs store each Deck
in this same form inside their `manifest.json`.

If the file is ever unreadable it is kept as `layouts.json.bad`, and Dock Deck
starts fresh with a single **Main** Deck.

## Deck packs

A Deck pack (`*.dockdeck`) is Dock Deck's only export/import file. It holds
one or more Decks plus, if you want, the scenes, sources and media files they
use, so a single Deck, a set of Decks or a whole setup can be shared or moved
to another computer.

**Export** (right-click a Deck tab → *Export Deck Pack...*, which checks that
Deck, or Tools → Dock Deck → *Export Deck Pack...*, which checks every Deck):
a checklist grouped
as **Decks / Scenes / Sources / Media files**, with a *Select all / none* box
and an all/none box per group (both tri-state). Checking something checks
what it needs: a Deck's linked scene, a scene's sources, nested scenes and
groups (recursively), and the files those sources use. Media files show their
size, and the total is shown at the bottom. Uncheck anything you don't want,
for example purchased overlays: the source is still exported, just without
its file. Packs over 500 MB ask first; the limit is 4 GB. The file is written
in the background with a progress bar (and *Cancel*), so OBS doesn't freeze.
Afterwards a summary lists what was left out for privacy, sources exported
without their file, and settings that contain web addresses (check these
don't contain private links).

**Import** (Tools → Dock Deck → *Import Deck Pack...*, or **+** →
*Import Deck Pack...*): the same checklist,
everything checked. Names that already exist are marked, and each one gets
its own choice: **Rename** (default; adds " (2)"), **Skip** (keep and use the
existing one) or **Replace** (the existing scene/source is removed; for Decks,
the existing tab takes the imported docks). Media files are copied in the
background to `plugin_config/obs-dock-deck/media/<pack name>/` (an existing
file of the same name is never overwritten) and the sources point there.
Media you uncheck, or that wasn't in the pack, leaves the source with an empty
file setting; the summary lists those sources ("open their properties to pick
one"), sources renamed because of a clash, and source types whose plugin isn't
installed. Decks keep their lock state and their linked scene; the link
follows a renamed scene and is set for the scene collection you import into.

### What is never in a pack

- Any setting whose name contains a word like *password*, *passwd*, *pwd*,
  *passphrase*, *secret*, *token*, *auth*/*oauth*/*authorization*, *cookie*,
  *credential*, *session*, *bearer*, *jwt*, or that ends in *key*
  (*stream_key*, *apiKey*, *private_key*, ...). Names are split on `_`, `-`,
  `.` and camelCase, so *key_color* (a chroma key colour) is kept. This applies
  to settings, nested settings, filter settings and private settings.
- The same names as parameters inside web addresses
  (`...?token=...` is removed, `...?theme=dark` stays) and logins inside
  addresses (`rtmp://user:password@host` becomes `rtmp://host`).
- Hotkey bindings, source ids (uuids) and absolute file paths (media are
  copied in and re-pointed, or left empty).
- Never read at all: `service.json` (stream key / server), profiles, OBS's
  own settings and the browser source's cookies and local storage (those live
  in the OBS profile folder, not in source settings).

Known limits: media must be files referenced by full path in source or filter
settings (folders such as an image slideshow's list are exported without
their files); sources that come with no scene don't stay after import (OBS
removes unused sources); packs are plain ZIP files without compression, at
most 4 GB.

## Scene Organizer

**Docks → Scene Organizer** adds a dock listing the current scene
collection's scenes, with a search box and your own folders ("groups").

- Click a scene to switch to it (in Studio Mode it goes to Preview, like OBS's
  own Scenes dock). The scene OBS is showing is highlighted.
- Type in the search box to filter by scene or group name; matching groups
  open to show what's inside. Dragging is off while searching.
- Right-click: *New Group*, *New Group Inside "…"*, *Rename Group* (or F2),
  *Delete Group (Keep Its Scenes)*, *Expand All* / *Collapse All*.
- Drag scenes and groups to reorder them or move them into other groups
  (groups nest as deep as you like).
- Groups are only Dock Deck's view: they never rename, reorder or change OBS
  scenes, and OBS's own Scenes dock keeps its order. New scenes appear at the
  bottom, deleted scenes disappear, renamed scenes keep their place (scenes
  are remembered by OBS's internal scene id, not by name).
- Saved per scene collection in `plugin_config/obs-dock-deck/organizer.json`;
  switching collections shows that collection's groups, and renaming a
  collection keeps them.
- It's a normal OBS dock, so Decks can show, hide and place it like any other.
- Not in Deck packs: groups point at scene ids that are different in another
  collection or on another computer, so imported scenes simply appear at the
  bottom of the organizer, ready to be sorted.

## Switch fade

When you switch Decks, a picture of the old dock layout is laid over the
window, the new Deck is restored underneath at once, and the picture fades
out. Only that picture animates: the real docks are never animated or
re-laid-out, and OBS's preview/program and other video displays (drawn by
OBS's graphics thread, not by the UI) are cut out of the picture, so the live
video stays visible and rendering, streaming and recording aren't affected.
The picture is taken by drawing the window's widgets (not a screen capture),
so it works the same on Wayland, X11 and Windows. Taking it costs a few
milliseconds; the fade runs on Qt's animation timer and never blocks.

**Tools → Dock Deck → Switch Fade Settings...**:

- *Fade when switching Decks* (default on).
- *Duration* 0–1000 ms (default 150; 0 = no fade).
- *Match OBS transition duration*: use the duration set in OBS's Scene
  Transitions dock instead (capped at 1000 ms).
- *Fade while streaming or recording* (default off): while live, Decks switch
  instantly unless this is on.

A switch that arrives during a fade (fast hotkey presses) ends the fade and
snaps straight to the new Deck. OBS starting up and imports never fade. The
Deck tabs and the menu bar switch immediately; floating docks are not part of
the fade.

## Linked scenes

Right-click a Deck tab → **Link to Current Scene ("<scene>")** links the Deck
to the scene OBS is showing right now; **Unlink Scene** removes it. The tab's
tooltip shows the linked scene.

- Selecting the Deck from a tab, the Tools menu, a hotkey or obs-websocket
  switches OBS to the linked scene, unless that scene is already showing.
  Pressing a Deck's hotkey while that Deck is already selected also brings
  back its scene.
- **Studio Mode**: the scene is put in **Preview** instead, so picking a Deck
  never cuts your live Program. You do the transition as usual.
- Links are per scene collection: a Deck can have one scene in each
  collection. In a collection without a link, selecting the Deck only changes
  the docks.
- Renaming a scene or a scene collection keeps the link. If the scene is
  deleted, the Deck just stops switching scenes, and its tooltip says the scene
  is missing. Link it again, or unlink it.
- OBS starting up and importing Decks never change the scene.
- Links travel with Deck packs and come back on import. On another computer they only work if the scene
  collection and scene have the same names.
- Dock Deck does not switch Decks when you change scenes. Links go one way
  only: Deck → scene.

## Hotkeys

**Settings → Hotkeys** lists, for every Deck, **Dock Deck: Switch to <Deck>**,
plus **Dock Deck: Next Deck**, **Dock Deck: Previous Deck** (both wrap around)
and **Dock Deck: Toggle Lock on Current Deck**. Stream Deck and similar tools can
press these hotkeys.

Each Deck has a permanent internal id, so renaming a Deck keeps its hotkey.
Bindings are saved in `hotkeys.json` next to `layouts.json`. A deleted Deck's
binding is kept there too, so importing that Deck again (from an export made on
this computer) brings its hotkey back.

## Remote control (obs-websocket)

If obs-websocket is available (built into OBS 28+; **Tools → WebSocket Server
Settings**), Dock Deck registers the vendor **`dock-deck`**. Without
obs-websocket nothing happens, and nothing breaks.

Send requests with obs-websocket's `CallVendorRequest`. Each answer has
`"ok": true`, or `"ok": false` plus an `"error"` text.

| Request | Request data | Answer |
| --- | --- | --- |
| `GetDecks` | none | `decks: [{name, id, locked, linkedScene, active}]`, `activeName`, `activeId` |
| `GetCurrentDeck` | none | `name`, `id`, `locked`, `linkedScene` |
| `SetCurrentDeck` | `{"name": "Gaming"}` or `{"id": "..."}` | the Deck switched to |
| `NextDeck` / `PreviousDeck` | none | the Deck switched to (wraps around) |
| `SetDeckLocked` | `{"name": "Gaming", "locked": true}` (or `"id"`) | `name`, `id`, `locked` |
| `SetDeckScene` | `{"name": "Gaming", "scene": "Gameplay"}` (or `"id"`; `"scene": ""` unlinks) | `name`, `id`, `linkedScene` |

`linkedScene` is the Deck's scene in the current scene collection (`""` if
none). `SetCurrentDeck`, `NextDeck` and `PreviousDeck` also switch to the
Deck's linked scene, just like clicking its tab.

Names match exactly first, then ignoring upper/lower case. `id` is better for
bots because it doesn't change when a Deck is renamed (get it from `GetDecks`).

Event **`DeckChanged`** (obs-websocket `VendorEvent`, in the default
"all events" subscription) is sent on every switch:
`{"name": "Gaming", "id": "...", "source": "tab" | "menu" | "hotkey" | "websocket" | "import"}`.

**Streamer.bot** (verified with Streamer.bot 1.0.7 on Linux/Wine): add an
*OBS Raw* sub-action and paste the **full request**, including the outer
`CallVendorRequest` wrapper:

```json
{"requestType":"CallVendorRequest","requestData":{"vendorName":"dock-deck","requestType":"SetCurrentDeck","requestData":{"name":"Chatting"}}}
```

```json
{"requestType":"CallVendorRequest","requestData":{"vendorName":"dock-deck","requestType":"NextDeck","requestData":{}}}
```

Pasting only the inner part (`{"vendorName": "dock-deck", ...}`) fails with
`OBS request failed [204]: Your request type is not valid`.

**SAMMI / other tools** (not verified yet): use the tool's raw/custom OBS
request action the same way, with request type `CallVendorRequest` and the
inner `requestData` shown above; if the tool takes one JSON blob, use the full
wrapper as for Streamer.bot.

```json
{
  "vendorName": "dock-deck",
  "requestType": "SetDeckLocked",
  "requestData": { "name": "Gaming", "locked": true }
}
```

The full obs-websocket v5 message (for your own clients) looks like this:

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "1",
    "requestData": {
      "vendorName": "dock-deck",
      "requestType": "GetDecks",
      "requestData": {}
    }
  }
}
```

### Try it from a terminal

`tools/dockdeck-remote.py` is a small test client (Python 3 + `obsws-python`).
Use the port and password from **Tools → WebSocket Server Settings**:

```bash
pip install obsws-python
python tools/dockdeck-remote.py --password YOUR_PASSWORD list
python tools/dockdeck-remote.py --password YOUR_PASSWORD switch "Main"
python tools/dockdeck-remote.py --password YOUR_PASSWORD next
python tools/dockdeck-remote.py --password YOUR_PASSWORD lock "Main"
python tools/dockdeck-remote.py --password YOUR_PASSWORD scene "Main" "Scene"
python tools/dockdeck-remote.py --password YOUR_PASSWORD scene "Main" ""
python tools/dockdeck-remote.py --password YOUR_PASSWORD watch
```

`watch` prints a `DeckChanged` line for every switch (click tabs, use hotkeys)
until you press Ctrl+C.

## Usage

1. Start OBS. On the first run, your current docks become the **Main** Deck.
2. Arrange docks the way you want for another task, then click **+** and name
   it (for example "Gaming"). That arrangement is now its own Deck.
3. Click Deck tabs to switch. With *Remember Dock Changes Per Deck* on, changes
   you make on a Deck stay with that Deck.
4. To move Decks to another computer, use **Tools → Dock Deck → Export Deck
   Pack...** (or right-click a tab → *Export Deck Pack...*) there and
   **Import Deck Pack...** here.
5. To protect a finished Deck, click the padlock on its tab (or right-click →
   *Lock Deck*). Click it again to make changes.
6. To hide the tabs, right-click them → *Hide Deck Tabs*. Bring them back with
   **Tools → Dock Deck → Show Deck Tabs**.

## For developers

Every Deck switch goes through one function,
`DockDeck::switchToDeck(name, source)` in `src/dock-deck.cpp`. It is safe to
call from any thread and before OBS finishes loading. Hotkeys and obs-websocket
requests use it (`SwitchSource::Hotkey` / `Websocket`); scene links and the
optional fade will too.

| File | What it does |
| --- | --- |
| `src/plugin-main.cpp` | Module entry points and OBS frontend events |
| `src/dock-deck.*` | Deck switching, startup restore, placeholders, menus, dialogs |
| `src/deck-strip.*` | The Deck tabs toolbar (view only; emits signals) |
| `src/deck-store.*` | Deck data model, config load/save, export/import file format |
| `src/deck-hotkeys.cpp` | Frontend hotkeys and their saved bindings (`hotkeys.json`) |
| `src/deck-websocket.*` | obs-websocket vendor `dock-deck` (requests + `DeckChanged` event) |
| `src/scene-organizer.*` | Scene Organizer dock (search, nested groups, `organizer.json`) |
| `src/deck-fade.*` | The switch fade overlay (picture of the old docks, faded out) |
| `src/deck-pack.*` | Deck pack export/import dialogs, background copying |
| `src/pack-core.*` | Pack contents: scanning scenes/sources/media, secret scrubbing, re-creating sources |
| `src/pack-zip.*` | Minimal ZIP (stored) writer/reader with CRC checks |
| `src/obs-websocket-api.h` | obs-websocket's header-only plugin API (GPL-2.0+, unmodified apart from formatting) |
| `tools/dockdeck-remote.py` | Command-line test client for the vendor requests |

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

You should see `[obs-dock-deck] plugin loaded successfully (version 0.8.0)`,
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

## Manual test checklist (0.8.0)

Scene Organizer:
1. **Docks → Scene Organizer**: all scenes are listed in OBS's order; the
   current one is highlighted.
2. Right-click → *New Group*, name it, drag two scenes into it; create a group
   inside it and drag a scene there. Drag a group above another scene.
3. Click scenes: OBS switches. Turn on Studio Mode: clicks change Preview only.
4. Type part of a name in the search box: only matches (and their groups)
   show. Search a group's name: its scenes show. Clear: back to normal,
   collapsed groups stay collapsed.
5. Rename a scene in OBS's Scenes dock: same place, new name. Add a scene: it
   appears at the bottom. Delete one: it disappears. OBS's own Scenes dock
   order and names never change because of the organizer.
6. Switch to another scene collection and back: each has its own groups.
   Restart OBS: groups, order and collapsed state are kept.
7. *Delete Group (Keep Its Scenes)*: its scenes and subgroups move up a level.

## Manual test checklist (0.7.0)

Switch fade:
1. Click between two Decks with different layouts: the old layout fades into
   the new one (about 150 ms). The preview keeps playing during the fade with
   no black box, and the tabs switch immediately.
2. Tools → Dock Deck → *Switch Fade Settings...*: set 1000 ms to see it
   clearly, then back. 0 ms or unticking *Fade when switching Decks* switches
   instantly.
3. Hammer Next/Previous Deck hotkeys: no flicker build-up; a switch during a
   fade snaps to the new Deck.
4. Tick *Match OBS transition duration*: the fade follows the duration in the
   Scene Transitions dock (try 300 ms vs 1000 ms).
5. Start recording (or streaming): switching is instant. Tick *Fade while
   streaming or recording*: it fades again. Check the recording/stream has no
   dropped or lagged frames in the Stats dock while switching.
6. Restart OBS: settings are kept, and startup doesn't fade.

## Manual test checklist (0.6.1)

One file format:
1. Right-click a Deck tab: there is *Export Deck Pack...* (no separate
   *Export Deck...*). It opens the pack checklist with only that Deck checked,
   plus its linked scene and what that scene uses.
2. Tools → Dock Deck: only *Export Deck Pack...* and *Import Deck Pack...*
   (no *Export Deck Collection...* / *Import Decks...*). Export starts with
   every Deck checked. Uncheck *Scenes*, *Sources* and *Media files* and export:
   a docks-only `.dockdeck`.
3. Click **+** → *Import Deck Pack...*: the file picker lists `.dockdeck`
   files, including the one just saved. Importing it adds the Decks only.

## Manual test checklist (0.6.0)

Deck packs (use a throwaway scene collection: Scene Collection → New):
1. Make a scene "Pack Test" with an Image source, a Media Source pointing at a
   video, and a nested scene or group. Link a Deck to "Pack Test" and lock it.
2. Right-click that Deck → *Export Deck Pack...*: the Deck, "Pack Test", its
   sources, nested scene/group and both files are pre-checked; sizes shown.
   Try *Select all / none* and the group boxes. Uncheck the video, export.
3. Summary lists the Media Source as exported without its file.
4. Import the pack (same collection): "Pack Test" and the Deck are marked as
   existing; leave *Rename*. Result: "Pack Test (2)", Deck "<name> (2)" locked
   and linked to "Pack Test (2)"; the image points into
   `plugin_config/obs-dock-deck/media/<pack>/`; the Media Source has no file and
   is listed in the summary.
5. Import again choosing *Skip* for everything that exists: nothing duplicated.
   Once more with *Replace* for "Pack Test": the old scene is replaced (still
   only one "Pack Test", now the pack's copy).
6. Copy the pack to the other computer and import it: scenes, sources, media,
   Deck lock and scene link arrive.
7. Privacy: add a Browser source with URL
   `https://example.com/?token=SECRET123&theme=dark`, export it with its
   scene, then run the command below: it prints `clean`, the summary lists the
   token under "Left out for privacy", and the imported source's URL is
   `https://example.com/?theme=dark`.
8. A pack over 500 MB asks before exporting; the progress bar can be cancelled.

```bash
unzip -p "<pack>.dockdeck" manifest.json | grep -E 'SECRET123|/home/|[A-Z]:(\\\\|/)' || echo clean
```

## Manual test checklist (0.5.0)

Linked scenes:
1. Go to scene "A", right-click a Deck tab → *Link to Current Scene ("A")*.
   Hover the tab: the tooltip says *Scene: A*.
2. Go to another scene and another Deck, then click the linked Deck's tab: OBS
   switches to scene A. Click a Deck without a link: the scene stays as it is.
3. Press the linked Deck's hotkey while on it but showing another scene: OBS
   goes back to A. `python tools/dockdeck-remote.py ... switch "<Deck>"` does
   the same; `list` shows `linkedScene`.
4. Studio Mode on: clicking the linked Deck puts A in Preview; Program
   doesn't change.
5. Rename scene A: the tooltip shows the new name and the link still works.
   Delete it: the tooltip says it's missing, clicking the tab only changes the
   docks, and nothing crashes.
6. Switch to another scene collection: the Deck has no link there (tooltip
   has no scene line). Switch back: the link is there again. Rename the
   collection: the link stays.
7. Export the Deck, delete it, import it: the link is back. Restart OBS: the
   scene doesn't change on startup.

From 0.4.0:

Hotkeys:
1. **Settings → Hotkeys**: find *Dock Deck: Switch to <Deck>* for every Deck plus
   *Next Deck*, *Previous Deck*, *Toggle Lock on Current Deck*. Bind keys; each
   works (Next/Previous wrap around; Toggle Lock flips the padlock).
2. Rename a Deck: its hotkey entry shows the new name and keeps its key.
3. Add a Deck with **+**: a new *Switch to* entry appears. Delete one: it goes.
4. Restart OBS: all bindings are still there.

obs-websocket (enable it in **Tools → WebSocket Server Settings**):
1. `python tools/dockdeck-remote.py --password ... list` shows every Deck,
   its id, lock and which is active.
2. `switch "<Deck>"`, `next`, `prev`, `lock "<Deck>"`, `unlock "<Deck>"` do what
   they say in OBS. A wrong name answers `"ok": false` with an error.
3. Run `watch` in a second terminal and click tabs / use hotkeys: one
   `DeckChanged` line per switch with `source` tab / hotkey / websocket.

From 0.3.2:

Locked splitters:
1. Lock a Deck (padlock) and hover the gap between two docks: the cursor stays
   the normal arrow, and stays normal after moving away.
2. Same with OBS's **Docks → Lock Docks** instead of a Deck lock.
3. Hover a gap while unlocked (resize cursor), then lock from the tab's
   padlock: the cursor goes back to normal. Unlock: gaps show the resize cursor
   and resize again.

From 0.3.1:

Export / import:
1. Tools → Dock Deck → *Export Deck Pack...*: every Deck is listed and
   checked. *Select all / none* clears all (Export greys out) and checks all;
   unchecking one Deck makes it partial. Export two Decks, then import the file:
   only those two come in.
2. With a Deck whose dock is missing (placeholder), export a *different* Deck,
   delete it and import it: the summary must not mention the other Deck's
   missing dock.
3. Click **+** → *Import Deck Pack...*: the name dialog closes and the import
   file picker opens.

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
   computer: it arrives locked.

## CI

GitHub Actions builds Ubuntu 26.04 and Windows (`.github/workflows`) on every push to `main`.

## Roadmap

1. ✅ Empty plugin loads in OBS (Linux + Windows).
2. ✅ Save/load dock arrangements with `QMainWindow::saveState` / `restoreState`,
   plus minimalist tabs under the menu bar, one tab per Deck (since then:
   export/import, placeholders for missing docks, Deck locking).
3. ✅ Per-Deck hotkeys (Stream Deck) and obs-websocket vendor requests for remote
   switching (bots like Streamer.bot/SAMMI can expose it to chat moderators).
4. ✅ Linked scenes: selecting a Deck switches OBS to its linked scene (per
   scene collection; Preview in Studio Mode).
5. ✅ Deck packs: export/import a Deck together with selected scenes, sources and
   media files. Per-item checkboxes on both export and import, plus a master
   *Select all / none* checkbox and per-group (scenes, sources, media)
   all/none checkboxes (tri-state). Media that is left out (for example
   purchased overlays) imports as an empty, unpointed source for the user to
   fill in. Plugins the pack needs but that are missing are listed. Secrets
   (stream keys, passwords, tokens) are never included.
6. ✅ Optional quick fade when switching Decks (off while streaming/recording by
   default, can be turned off completely).
7. ✅ Scene Organizer dock with search and nested folders.
