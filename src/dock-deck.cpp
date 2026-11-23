/*
Dock Deck for OBS
Copyright (C) 2026 Paul Phillips

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "dock-deck.hpp"
#include "deck-strip.hpp"
#include "deck-websocket.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QDockWidget>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

// Marks our own stand-in docks so they are never mistaken for real ones.
static constexpr const char *kPlaceholderProperty = "dockDeckPlaceholder";

// A locked deck stores each dock's own features here while they are off.
static constexpr const char *kSavedFeaturesProperty = "dockDeckSavedFeatures";

static bool isPlaceholder(const QObject *obj)
{
	return obj->property(kPlaceholderProperty).toBool();
}

static bool sceneExists(const QString &name)
{
	if (name.isEmpty())
		return false;
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	const bool ok = source && obs_source_is_scene(source);
	obs_source_release(source);
	return ok;
}

static QString currentCollectionName()
{
	char *name = obs_frontend_get_current_scene_collection();
	const QString out = QString::fromUtf8(name ? name : "");
	bfree(name);
	return out;
}

// libobs "source_rename" (any thread): scene renames are queued to the UI.
static void onSourceRename(void *data, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	const QString newName = QString::fromUtf8(calldata_string(cd, "new_name"));
	const QString oldName = QString::fromUtf8(calldata_string(cd, "prev_name"));
	auto *self = static_cast<DockDeck *>(data);
	QMetaObject::invokeMethod(
		self, [self, oldName, newName]() { self->onSourceRenamed(oldName, newName); }, Qt::QueuedConnection);
}

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static const char *sourceName(SwitchSource source)
{
	switch (source) {
	case SwitchSource::Tab:
		return "tab";
	case SwitchSource::Menu:
		return "menu";
	case SwitchSource::Startup:
		return "startup";
	case SwitchSource::Import:
		return "import";
	case SwitchSource::Hotkey:
		return "hotkey";
	case SwitchSource::Websocket:
		return "websocket";
	case SwitchSource::SceneLink:
		return "scene-link";
	case SwitchSource::Api:
		return "api";
	}
	return "unknown";
}

DockDeck::DockDeck(QMainWindow *mainWindow) : QObject(mainWindow), main_(mainWindow)
{
	// Config lives in OBS's per-plugin config folder, e.g.
	// ~/.config/obs-studio/plugin_config/obs-dock-deck/layouts.json
	char *path = obs_module_config_path("layouts.json");
	configPath_ = QString::fromUtf8(path ? path : "");
	bfree(path);
	char *hkPath = obs_module_config_path("hotkeys.json");
	hotkeyPath_ = QString::fromUtf8(hkPath ? hkPath : "");
	bfree(hkPath);

	if (!configPath_.isEmpty()) {
		QDir().mkpath(QFileInfo(configPath_).absolutePath());
		switch (store_.load(configPath_)) {
		case DeckStore::LoadResult::Ok:
			obs_log(LOG_INFO, "loaded %d deck(s) from %s", static_cast<int>(store_.decks.size()),
				configPath_.toUtf8().constData());
			break;
		case DeckStore::LoadResult::Missing:
			obs_log(LOG_INFO, "no decks yet; a default one is created when OBS finishes loading");
			break;
		case DeckStore::LoadResult::Corrupt:
			obs_log(LOG_WARNING, "deck collection file was unreadable; kept it as %s.bad and started fresh",
				configPath_.toUtf8().constData());
			break;
		}
	}

	// Tab strip: a fixed toolbar in the top toolbar area, which Qt places
	// directly under the menu bar and above every dock and the preview. It
	// stays hidden until OBS has finished loading and a deck is applied.
	strip_ = new DeckStrip(main_);
	main_->addToolBar(Qt::TopToolBarArea, strip_);
	strip_->setVisible(false);

	connect(strip_, &DeckStrip::deckActivated, this,
		[this](const QString &name) { switchToDeck(name, SwitchSource::Tab); });
	connect(strip_, &DeckStrip::newDeckRequested, this, &DockDeck::newDeckFromCurrent);
	connect(strip_, &DeckStrip::overwriteRequested, this, &DockDeck::overwriteDeck);
	connect(strip_, &DeckStrip::renameRequested, this, &DockDeck::renameDeck);
	connect(strip_, &DeckStrip::deleteRequested, this, &DockDeck::deleteDeck);
	connect(strip_, &DeckStrip::exportRequested, this, [this](const QString &name) { exportDecks(name); });
	connect(strip_, &DeckStrip::hideRequested, this, [this]() { setTabStripVisible(false); });
	connect(strip_, &DeckStrip::deckMoved, this, &DockDeck::moveDeck);
	connect(strip_, &DeckStrip::lockToggled, this, &DockDeck::toggleLock);
	strip_->currentScene = []() {
		obs_source_t *scene = obs_frontend_get_current_scene();
		const QString name = QString::fromUtf8(scene ? obs_source_get_name(scene) : "");
		obs_source_release(scene);
		return name;
	};
	connect(strip_, &DeckStrip::linkSceneRequested, this,
		[this](const QString &name) { setDeckScene(name, strip_ ? strip_->currentScene() : QString()); });
	connect(strip_, &DeckStrip::unlinkSceneRequested, this,
		[this](const QString &name) { setDeckScene(name, QString()); });

	// Tools > Dock Deck submenu (switch decks, toggle the strip, options).
	// It is rebuilt each time it opens so it always matches the deck list.
	toolsAction_ = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("DockDeck.Title")));
	if (toolsAction_) {
		toolsMenu_ = new QMenu(main_);
		toolsAction_->setMenu(toolsMenu_);
		connect(toolsMenu_, &QMenu::aboutToShow, this, &DockDeck::rebuildToolsMenu);
	}

	// OBS's own Docks > Lock Docks. A deck counts as locked if either that
	// or the deck's own lock is on. OBS sets dock features in its toggled
	// slot; we re-assert ours right after it (queued).
	obsLockAction_ = main_->findChild<QAction *>(QStringLiteral("lockDocks"));
	if (obsLockAction_) {
		connect(obsLockAction_, &QAction::toggled, this,
			[this]() { QTimer::singleShot(0, this, &DockDeck::updateLock); });
	} else {
		obs_log(LOG_INFO, "OBS 'Lock Docks' action not found; only per-deck locks apply");
	}

	// Watch the main window's Close event: it arrives before OBS tears down
	// docks (browser docks are removed during shutdown), so this is the last
	// moment the dock arrangement is complete.
	main_->installEventFilter(this);
}

DockDeck::~DockDeck()
{
	if (main_)
		main_->removeEventFilter(this);
}

// ---------------------------------------------------------------------------
// The central switch
// ---------------------------------------------------------------------------

bool DockDeck::switchToDeckId(const QString &id, SwitchSource source)
{
	const Deck *deck = store_.findById(id);
	return deck ? switchToDeck(deck->name, source) : false;
}

bool DockDeck::stepDeck(int delta, SwitchSource source)
{
	const qsizetype count = store_.decks.size();
	if (!ready_ || count == 0)
		return false;
	const qsizetype current = store_.indexOf(store_.active);
	const qsizetype next = ((current < 0 ? 0 : current) + delta % count + count) % count;
	return switchToDeck(store_.decks[next].name, source);
}

DockDeck::Snapshot DockDeck::snapshot() const
{
	QMutexLocker lock(&snapshotMutex_);
	return snapshot_;
}

void DockDeck::publishSnapshot()
{
	Snapshot snap;
	snap.ready = ready_;
	for (const Deck &deck : store_.decks)
		snap.decks.append({deck.id, deck.name, deck.locked, linkedScene(deck)});
	if (const Deck *active = store_.find(store_.active))
		snap.activeId = active->id;
	QMutexLocker lock(&snapshotMutex_);
	snapshot_ = snap;
}

bool DockDeck::switchToDeck(const QString &name, SwitchSource source)
{
	// Hotkeys and websocket requests arrive on OBS worker threads; widgets may
	// only be touched on the UI thread.
	if (QThread::currentThread() != thread()) {
		QMetaObject::invokeMethod(
			this, [this, name, source]() { switchToDeck(name, source); }, Qt::QueuedConnection);
		return true;
	}

	if (applying_)
		return false;

	Deck *target = store_.find(name);
	if (!target) {
		obs_log(LOG_WARNING, "switch to unknown deck '%s' (%s) ignored", name.toUtf8().constData(),
			sourceName(source));
		scheduleRefresh(); // e.g. resync the strip if it showed something stale
		return false;
	}

	// Before OBS finishes loading, just remember the choice.
	if (!ready_) {
		store_.active = name;
		return true;
	}

	// Picking the deck you are on (hotkey, remote) still selects its scene.
	if (name == store_.active) {
		if (source != SwitchSource::Startup && source != SwitchSource::Import)
			activateLinkedScene(name);
		return true;
	}

	// Keep the dock changes made on the tab we are leaving.
	if (store_.autoSave)
		captureActive();

	// Capturing may have reallocated the list; look the target up again.
	target = store_.find(name);
	if (!target)
		return false;

	// FUTURE: the optional fade hooks in here (snapshot old docks, apply,
	// snapshot new docks, cross-fade an overlay, then remove it).
	apply(*target);
	store_.active = name;

	obs_log(LOG_INFO, "switched to deck '%s' (%s)", name.toUtf8().constData(), sourceName(source));
	saveConfig();
	scheduleRefresh();
	if (source != SwitchSource::Startup && source != SwitchSource::Import)
		activateLinkedScene(name);
	if (const Deck *now = store_.find(name))
		DeckWebsocket::emitDeckChanged(now->name, now->id, sourceName(source));
	return true;
}

// Snapshot the main window's current dock arrangement.
Deck DockDeck::capture(const QString &name, const Deck *previous) const
{
	Deck deck;
	deck.name = name;

	// Qt forgets the size of hidden docks in saveState(). Placeholders that
	// are hidden only because the setting is off are shown for the snapshot
	// and hidden again right away (same call, nothing is painted), so their
	// size survives switching decks while the setting is off.
	QVector<QDockWidget *> reshow;
	if (!store_.showPlaceholders) {
		for (auto it = placeholders_.constBegin(); it != placeholders_.constEnd(); ++it) {
			const bool deckShows = previous ? previous->docks.value(it.key(), true) : true;
			if (it.value() && deckShows && it.value()->isHidden()) {
				it.value()->show();
				reshow.append(it.value());
			}
		}
	}
	deck.state = main_->saveState();
	for (QDockWidget *dock : reshow)
		dock->hide();

	// Only docks that belong to the main window itself; floating docks keep
	// the main window as parent, so they are included too. A dock's "shown"
	// state is its Docks-menu checkmark (toggleViewAction), which stays true
	// for docks that are merely behind another tab. Placeholders count as the
	// dock they stand in for (same objectName, title = the real dock's title).
	const auto docks = main_->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *dock : docks) {
		const QString id = dock->objectName();
		if (id.isEmpty())
			continue;
		bool shown = dock->toggleViewAction()->isChecked();
		// With the setting off, placeholders are hidden by us, not by the
		// user: keep the deck's own shown/hidden value for that dock.
		if (isPlaceholder(dock) && !store_.showPlaceholders)
			shown = previous ? previous->docks.value(id, true) : true;
		deck.docks.insert(id, shown);
		const QString title = dock->windowTitle().simplified();
		if (!title.isEmpty())
			deck.titles.insert(id, title);
	}

	// Keep what we knew about docks that aren't here right now.
	if (previous) {
		for (auto it = previous->docks.constBegin(); it != previous->docks.constEnd(); ++it) {
			if (!deck.docks.contains(it.key()))
				deck.docks.insert(it.key(), it.value());
		}
		for (auto it = previous->titles.constBegin(); it != previous->titles.constEnd(); ++it) {
			if (!deck.titles.contains(it.key()))
				deck.titles.insert(it.key(), it.value());
		}
	}
	return deck;
}

// Apply a saved arrangement. Missing docks are skipped (or get placeholders),
// never fatal.
void DockDeck::apply(const Deck &deck)
{
	const QScopedValueRollback<bool> guard(applying_, true);

	// Placeholders must exist before restoreState() so Qt can put them where
	// the missing docks were; ones the new deck doesn't need are removed so
	// they don't linger on screen.
	syncPlaceholders(&deck);

	// restoreState() ignores docks it doesn't know and keeps slots for saved
	// docks that don't exist right now, so a missing plugin is harmless.
	if (!deck.state.isEmpty() && !main_->restoreState(deck.state))
		obs_log(LOG_WARNING, "deck '%s': Qt could not restore the saved dock state",
			deck.name.toUtf8().constData());

	// Then enforce shown/hidden for every dock the deck knows about.
	const auto docks = main_->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *dock : docks) {
		const auto it = deck.docks.constFind(dock->objectName());
		if (it == deck.docks.constEnd())
			continue; // dock is newer than the deck: leave it alone
		const bool want = it.value() && (store_.showPlaceholders || !isPlaceholder(dock));
		if (dock->toggleViewAction()->isChecked() != want)
			dock->setVisible(want);
	}

	// The strip is part of saveState(), so put it back the way the user wants.
	if (strip_)
		strip_->setVisible(store_.showTabStrip);

	applyLock(&deck);
}

// ---------------------------------------------------------------------------
// Placeholders for missing docks
//
// A deck can name docks that don't exist right now: a plugin that isn't
// installed (yet), a browser dock from another computer, or a plugin that
// creates its dock late. For each such dock that the deck shows, we add a
// plain QDockWidget with the same objectName before restoreState(), so Qt
// puts it exactly where the real dock would go and the arrangement keeps its
// shape. It is not registered with OBS (no Docks-menu entry). Because it uses
// the real dock's objectName, anything that saves the window state (OBS's own
// dock state on exit, our auto-save) records the real dock's spot, which is
// exactly what we want once the real dock is back.
//
// When the real dock shows up later (we watch the main window for new child
// widgets), adoptRealDocks() swaps it into the placeholder's spot.
//
// The "Show Placeholders for Missing Docks" setting only hides them. A
// hidden dock keeps its slot and size in Qt's layout (and in saveState()),
// so turning the setting back on, switching decks, or the real dock
// arriving later all bring it back at exactly the same size and place.
// ---------------------------------------------------------------------------

QHash<QString, QDockWidget *> DockDeck::realDocks() const
{
	QHash<QString, QDockWidget *> out;
	const auto docks = main_->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *dock : docks) {
		if (!isPlaceholder(dock) && !dock->objectName().isEmpty())
			out.insert(dock->objectName(), dock);
	}
	return out;
}

void DockDeck::syncPlaceholders(const Deck *deck)
{
	const QHash<QString, QDockWidget *> real = realDocks();

	// Which docks need a stand-in: shown in this deck and missing here.
	QHash<QString, QString> needed; // objectName -> title
	if (deck) {
		for (auto it = deck->docks.constBegin(); it != deck->docks.constEnd(); ++it) {
			if (it.value() && !real.contains(it.key()))
				needed.insert(it.key(), deck->titles.value(it.key()));
		}
	}

	// Remove stale ones first (also ones whose real dock has appeared).
	const QStringList current = placeholders_.keys();
	for (const QString &name : current) {
		if (!needed.contains(name) || !placeholders_.value(name))
			removePlaceholder(name);
	}

	for (auto it = needed.constBegin(); it != needed.constEnd(); ++it) {
		if (!placeholders_.value(it.key()))
			placeholders_.insert(it.key(), createPlaceholder(it.key(), it.value()));
	}
}

QDockWidget *DockDeck::createPlaceholder(const QString &name, const QString &title)
{
	const QString shown = title.isEmpty() ? name : title;

	// Title = the real dock's title, so auto-save keeps the friendly name.
	auto *dock = new QDockWidget(shown, main_);
	dock->setObjectName(name);
	dock->setProperty(kPlaceholderProperty, true);

	auto *body = new QWidget(dock);
	auto *layout = new QVBoxLayout(body);
	auto *text = new QLabel(T("DockDeck.Placeholder.Text").arg(shown), body);
	text->setTextFormat(Qt::PlainText);
	text->setWordWrap(true);
	text->setAlignment(Qt::AlignCenter);
	QFont bold = text->font();
	bold.setBold(true);
	text->setFont(bold);

	auto *hint = new QLabel(T("DockDeck.Placeholder.Hint"), body);
	hint->setTextFormat(Qt::PlainText);
	hint->setWordWrap(true);
	hint->setAlignment(Qt::AlignCenter);
	hint->setEnabled(false); // themes render disabled text muted

	layout->addStretch();
	layout->addWidget(text);
	layout->addWidget(hint);
	layout->addStretch();
	dock->setWidget(body);

	main_->addDockWidget(Qt::RightDockWidgetArea, dock);
	if (!store_.showPlaceholders)
		dock->hide();
	obs_log(LOG_INFO, "placeholder for missing dock '%s'", name.toUtf8().constData());
	return dock;
}

void DockDeck::removePlaceholder(const QString &name)
{
	QPointer<QDockWidget> dock = placeholders_.take(name);
	if (!dock)
		return;
	// Take it out of the layout and drop its name right away, so a later
	// restoreState() can't match the dying placeholder instead of the real dock.
	main_->removeDockWidget(dock);
	dock->setObjectName(QString());
	delete dock.data();
}

void DockDeck::scheduleAdopt()
{
	if (adoptQueued_)
		return;
	adoptQueued_ = true;
	// Run after the code that created the dock has finished setting it up
	// (objectName, title, addDockWidget all happen in one go in OBS).
	QTimer::singleShot(0, this, &DockDeck::adoptRealDocks);
}

void DockDeck::adoptRealDocks()
{
	adoptQueued_ = false;
	if (!ready_ || applying_ || placeholders_.isEmpty())
		return;

	struct Swap {
		QString name;
		QDockWidget *real;
		bool shown;
	};
	QVector<Swap> swaps;
	const QHash<QString, QDockWidget *> real = realDocks();
	for (auto it = placeholders_.constBegin(); it != placeholders_.constEnd(); ++it) {
		if (it.value() && real.contains(it.key())) {
			// With the setting off the placeholder is hidden by us; use the
			// deck's value for the real dock instead.
			const Deck *active = store_.find(store_.active);
			const bool shown = store_.showPlaceholders
						   ? it.value()->toggleViewAction()->isChecked()
						   : (active ? active->docks.value(it.key(), true) : true);
			swaps.append({it.key(), real.value(it.key()), shown});
		}
	}
	if (swaps.isEmpty())
		return;

	const QScopedValueRollback<bool> guard(applying_, true);

	// Snapshot the window with each real dock briefly renamed, so the entry
	// saved under the dock's name is the placeholder's position. Nothing else
	// runs in between, so OBS never sees the temporary names.
	for (const Swap &s : swaps)
		s.real->setObjectName(s.name + QStringLiteral("#dockdeck-adopting"));
	// Same as capture(): a placeholder hidden only by the setting is shown
	// for the snapshot so its size is recorded (it is removed right after).
	for (const Swap &s : swaps) {
		QDockWidget *ph = placeholders_.value(s.name);
		if (ph && s.shown && ph->isHidden())
			ph->show();
	}
	const QByteArray state = main_->saveState();
	for (const Swap &s : swaps)
		s.real->setObjectName(s.name);

	// Swap: drop the placeholders, then let Qt move the real docks into place.
	for (const Swap &s : swaps)
		removePlaceholder(s.name);
	main_->restoreState(state);
	for (const Swap &s : swaps) {
		if (s.real->toggleViewAction()->isChecked() != s.shown)
			s.real->setVisible(s.shown);
		obs_log(LOG_INFO, "dock '%s' appeared; moved it into its deck position", s.name.toUtf8().constData());
	}
	if (strip_)
		strip_->setVisible(store_.showTabStrip);
	updateLock(); // the real docks must not be movable in a locked deck

	if (store_.autoSave) {
		captureActive();
		saveConfig();
	}
}

void DockDeck::setShowPlaceholders(bool enabled)
{
	// Record what is on screen first (with the old setting), so the deck keeps
	// any change the user made to a placeholder.
	if (ready_ && store_.autoSave)
		captureActive();
	store_.showPlaceholders = enabled;

	// Only show/hide: the placeholders stay in the layout, so Qt keeps their
	// exact slot and size while hidden.
	const Deck *active = store_.find(store_.active);
	for (auto it = placeholders_.constBegin(); it != placeholders_.constEnd(); ++it) {
		if (!it.value())
			continue;
		const bool want = enabled && (active ? active->docks.value(it.key(), true) : true);
		if (it.value()->toggleViewAction()->isChecked() != want)
			it.value()->setVisible(want);
	}
	saveConfig();
}

void DockDeck::captureActive()
{
	// Locked (by the deck or by OBS's Lock Docks): never auto-save.
	if (locked_)
		return;
	Deck *active = store_.find(store_.active);
	if (active) {
		Deck fresh = capture(active->name, active);
		fresh.id = active->id;
		fresh.locked = active->locked;
		fresh.scenes = active->scenes;
		*active = fresh;
	}
}

// ---------------------------------------------------------------------------
// OBS lifecycle
// ---------------------------------------------------------------------------

void DockDeck::onFinishedLoading()
{
	currentCollection_ = currentCollectionName();
	signal_handler_connect(obs_get_signal_handler(), "source_rename", onSourceRename, this);

	// Defer one event-loop turn so plugins that add docks in their own
	// FINISHED_LOADING handler have done so before we restore.
	QTimer::singleShot(0, this, [this]() {
		if (store_.decks.isEmpty()) {
			// First run: the current arrangement becomes the first tab.
			store_.decks.append(capture(T("DockDeck.DefaultDeckName")));
			store_.active = store_.decks.front().name;
		} else {
			if (!store_.find(store_.active))
				store_.active = store_.decks.front().name;
			apply(*store_.find(store_.active));
			obs_log(LOG_INFO, "restored deck '%s' (startup)", store_.active.toUtf8().constData());
		}

		ready_ = true;
		if (strip_)
			strip_->setVisible(store_.showTabStrip);
		updateLock(); // also covers the first run, where nothing was applied
		loadHotkeyBindings();
		saveConfig(); // also registers the hotkeys
		refreshUi();
	});
}

void DockDeck::onExit()
{
	// Remote requests stop first: they must not queue work during teardown.
	DeckWebsocket::shutdown();
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", onSourceRename, this);

	// Dock state was already captured on the Close event; just write it out.
	if (ready_)
		saveConfig();
	ready_ = false; // ignore anything that arrives during teardown
	publishSnapshot();

	// libobs is still alive here; unregister so no hotkey can reach us later.
	saveHotkeyBindings();
	unregisterHotkeys();
	obs_data_release(hotkeyData_);
	hotkeyData_ = nullptr;
}

bool DockDeck::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == main_ && event->type() == QEvent::Close && ready_ && store_.autoSave) {
		captureActive();
		saveConfig();
	}
	// A new child of the main window may be a dock we hold a placeholder for,
	// or a new dock that has to be locked too.
	if (watched == main_ && event->type() == QEvent::ChildAdded && ready_) {
		if (!placeholders_.isEmpty())
			scheduleAdopt();
		if (locked_ && !lockQueued_) {
			lockQueued_ = true;
			QTimer::singleShot(0, this, [this]() {
				lockQueued_ = false;
				updateLock();
			});
		}
	}

	// Locked: no resizing. The separators between docks are not widgets;
	// QMainWindow itself handles them: a press on a gap starts a resize, and
	// HoverMove *and* MouseMove switch to the resize cursor. Swallow all of
	// those, and if a resize cursor is up anyway (e.g. it was set just before
	// locking), let Qt put the normal cursor back.
	if (watched == main_ && locked_) {
		switch (event->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick: {
			const auto *me = static_cast<QMouseEvent *>(event);
			if (!main_->childAt(me->position().toPoint()))
				return true;
			break;
		}
		case QEvent::HoverEnter:
		case QEvent::HoverMove:
		case QEvent::MouseMove:
			resetSeparatorCursor();
			return true;
		default:
			break;
		}
	}
	return QObject::eventFilter(watched, event);
}

void DockDeck::saveConfig()
{
	store_.ensureIds();
	if (ready_)
		syncHotkeys(); // names/ids may have changed
	publishSnapshot();
	if (configPath_.isEmpty())
		return;
	if (!store_.save(configPath_))
		obs_log(LOG_WARNING, "could not write %s", configPath_.toUtf8().constData());
}

// ---------------------------------------------------------------------------
// UI sync
// ---------------------------------------------------------------------------

// Coalesce UI rebuilds and run them outside any tab-bar signal handler.
void DockDeck::scheduleRefresh()
{
	if (refreshQueued_)
		return;
	refreshQueued_ = true;
	QTimer::singleShot(0, this, [this]() {
		refreshQueued_ = false;
		refreshUi();
	});
}

void DockDeck::refreshUi()
{
	if (!strip_)
		return;
	QVector<DeckStrip::Tab> tabs;
	for (const Deck &deck : store_.decks) {
		DeckStrip::Tab tab;
		tab.name = deck.name;
		tab.locked = deck.locked;
		tab.linkedScene = linkedScene(deck);
		tab.sceneMissing = !tab.linkedScene.isEmpty() && !sceneExists(tab.linkedScene);
		tabs.append(tab);
	}
	strip_->setDecks(tabs, store_.active);
}

void DockDeck::rebuildToolsMenu()
{
	toolsMenu_->clear();

	// One checkable entry per deck; picking one switches to it.
	auto *group = new QActionGroup(toolsMenu_);
	for (const Deck &deck : store_.decks) {
		const QString name = deck.name;
		const QString shown = QString(name).replace(QLatin1Char('&'), QStringLiteral("&&"));
		QAction *a = toolsMenu_->addAction(deck.locked ? T("DockDeck.Menu.LockedDeck").arg(shown) : shown);
		a->setCheckable(true);
		a->setChecked(name == store_.active);
		a->setEnabled(ready_);
		group->addAction(a);
		connect(a, &QAction::triggered, this, [this, name]() { switchToDeck(name, SwitchSource::Menu); });
	}
	if (!store_.decks.isEmpty())
		toolsMenu_->addSeparator();

	QAction *add = toolsMenu_->addAction(T("DockDeck.Menu.New"));
	add->setEnabled(ready_);
	connect(add, &QAction::triggered, this, &DockDeck::newDeckFromCurrent);
	QAction *save = toolsMenu_->addAction(T("DockDeck.Menu.SaveActive"));
	connect(save, &QAction::triggered, this, [this]() { overwriteDeck(store_.active); });
	const Deck *activeDeck = store_.find(store_.active);
	save->setEnabled(ready_ && activeDeck && !activeDeck->locked);
	toolsMenu_->addSeparator();

	bool anyLocked = false, anyUnlocked = false;
	for (const Deck &deck : store_.decks)
		(deck.locked ? anyLocked : anyUnlocked) = true;
	QAction *lockAll = toolsMenu_->addAction(T("DockDeck.Menu.LockAll"));
	lockAll->setEnabled(ready_ && anyUnlocked);
	connect(lockAll, &QAction::triggered, this, [this]() { setAllLocked(true); });
	QAction *unlockAll = toolsMenu_->addAction(T("DockDeck.Menu.UnlockAll"));
	unlockAll->setEnabled(ready_ && anyLocked);
	connect(unlockAll, &QAction::triggered, this, [this]() { setAllLocked(false); });
	if (obsDocksLocked()) {
		QAction *note = toolsMenu_->addAction(T("DockDeck.Menu.ObsLocked"));
		note->setEnabled(false);
	}
	toolsMenu_->addSeparator();

	QAction *exportAll = toolsMenu_->addAction(T("DockDeck.Menu.Export"));
	exportAll->setEnabled(ready_ && !store_.decks.isEmpty());
	connect(exportAll, &QAction::triggered, this, [this]() { exportDecks(); });
	QAction *import = toolsMenu_->addAction(T("DockDeck.Menu.Import"));
	import->setEnabled(ready_);
	connect(import, &QAction::triggered, this, &DockDeck::importDecks);
	toolsMenu_->addSeparator();

	QAction *show = toolsMenu_->addAction(T("DockDeck.Menu.ShowStrip"));
	show->setCheckable(true);
	show->setChecked(store_.showTabStrip);
	connect(show, &QAction::toggled, this, &DockDeck::setTabStripVisible);

	QAction *autoSave = toolsMenu_->addAction(T("DockDeck.Menu.AutoSave"));
	autoSave->setCheckable(true);
	autoSave->setChecked(store_.autoSave);
	autoSave->setToolTip(T("DockDeck.Menu.AutoSave.Tooltip"));
	connect(autoSave, &QAction::toggled, this, &DockDeck::setAutoSave);

	QAction *placeholders = toolsMenu_->addAction(T("DockDeck.Menu.Placeholders"));
	placeholders->setCheckable(true);
	placeholders->setChecked(store_.showPlaceholders);
	placeholders->setToolTip(T("DockDeck.Menu.Placeholders.Tooltip"));
	connect(placeholders, &QAction::toggled, this, &DockDeck::setShowPlaceholders);
	toolsMenu_->setToolTipsVisible(true);
}

// ---------------------------------------------------------------------------
// User actions
// ---------------------------------------------------------------------------

QString DockDeck::askForName(const QString &title, const QString &initial, const QString &ignore, bool *importChosen)
{
	if (importChosen)
		*importChosen = false;

	QString name = initial;
	for (;;) {
		QDialog dialog(main_);
		dialog.setWindowTitle(title);
		auto *layout = new QVBoxLayout(&dialog);
		layout->addWidget(new QLabel(T("DockDeck.Dialog.NameLabel"), &dialog));
		auto *edit = new QLineEdit(name, &dialog);
		edit->selectAll();
		layout->addWidget(edit);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		bool wantImport = false;
		if (importChosen) {
			// Alternative to naming a new deck: bring decks in from a file.
			QPushButton *import =
				buttons->addButton(T("DockDeck.Dialog.ImportInstead"), QDialogButtonBox::ActionRole);
			import->setAutoDefault(false);
			connect(import, &QPushButton::clicked, &dialog, [&dialog, &wantImport]() {
				wantImport = true;
				dialog.reject();
			});
		}
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		layout->addWidget(buttons);
		dialog.resize(qMax(dialog.sizeHint().width(), 360), dialog.sizeHint().height());

		const bool ok = dialog.exec() == QDialog::Accepted;
		if (wantImport) {
			*importChosen = true;
			return QString();
		}
		name = edit->text().trimmed();
		if (!ok || name.isEmpty())
			return QString();
		if (!store_.nameTaken(name, ignore))
			return name;
		QMessageBox::warning(main_, title, T("DockDeck.Dialog.NameTaken"));
	}
}

QStringList DockDeck::pickDecksForExport()
{
	QDialog dialog(main_);
	dialog.setWindowTitle(T("DockDeck.Dialog.ExportCollectionTitle"));
	auto *layout = new QVBoxLayout(&dialog);
	layout->addWidget(new QLabel(T("DockDeck.Dialog.ExportPick"), &dialog));

	auto *all = new QCheckBox(T("DockDeck.Dialog.SelectAll"), &dialog);
	all->setTristate(true);
	layout->addWidget(all);

	auto *list = new QListWidget(&dialog);
	for (const Deck &deck : store_.decks) {
		auto *item = new QListWidgetItem(deck.locked ? T("DockDeck.Menu.LockedDeck").arg(deck.name) : deck.name,
						 list);
		item->setData(Qt::UserRole, deck.name);
		item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Checked);
	}
	layout->addWidget(list);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
	QPushButton *exportBtn = buttons->addButton(T("DockDeck.Dialog.ExportButton"), QDialogButtonBox::AcceptRole);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);

	// The master box mirrors the list (checked / unchecked / partial), and
	// clicking it selects all or none. Export needs at least one deck.
	bool syncing = false;
	auto syncMaster = [&]() {
		int checked = 0;
		for (int i = 0; i < list->count(); ++i)
			checked += list->item(i)->checkState() == Qt::Checked ? 1 : 0;
		syncing = true;
		all->setCheckState(checked == 0 ? Qt::Unchecked
						: (checked == list->count() ? Qt::Checked : Qt::PartiallyChecked));
		syncing = false;
		exportBtn->setEnabled(checked > 0);
	};
	connect(list, &QListWidget::itemChanged, &dialog, [&]() {
		if (!syncing)
			syncMaster();
	});
	connect(all, &QCheckBox::clicked, &dialog, [&]() {
		// Qt cycles unchecked -> partial -> checked -> unchecked on click:
		// so from none or some, a click selects all; from all, none.
		const bool selectAll = all->checkState() != Qt::Unchecked;
		syncing = true;
		for (int i = 0; i < list->count(); ++i)
			list->item(i)->setCheckState(selectAll ? Qt::Checked : Qt::Unchecked);
		syncing = false;
		syncMaster();
	});
	syncMaster();

	if (dialog.exec() != QDialog::Accepted)
		return {};
	QStringList names;
	for (int i = 0; i < list->count(); ++i) {
		if (list->item(i)->checkState() == Qt::Checked)
			names.append(list->item(i)->data(Qt::UserRole).toString());
	}
	return names;
}

void DockDeck::newDeckFromCurrent()
{
	if (!ready_)
		return;

	// Suggest "Deck N" with the first free number.
	QString suggestion;
	for (int n = static_cast<int>(store_.decks.size()) + 1;; ++n) {
		suggestion = T("DockDeck.NewDeckName").arg(n);
		if (!store_.nameTaken(suggestion))
			break;
	}

	bool importInstead = false;
	const QString name = askForName(T("DockDeck.Dialog.NewTitle"), suggestion, QString(), &importInstead);
	if (importInstead) {
		importDecks();
		return;
	}
	if (name.isEmpty())
		return;

	// The current docks become the new deck, which becomes active. The tab
	// we came from keeps its own last saved/auto-saved arrangement.
	if (store_.autoSave)
		captureActive();
	store_.decks.append(capture(name, store_.find(store_.active)));
	store_.active = name;
	obs_log(LOG_INFO, "created deck '%s'", name.toUtf8().constData());
	saveConfig();
	scheduleRefresh();
}

void DockDeck::overwriteDeck(const QString &name)
{
	Deck *deck = store_.find(name);
	if (!ready_ || !deck || deck->locked)
		return; // a locked deck is kept as is; unlock it first

	// Overwriting a deck you are not looking at loses what it had, so ask.
	if (name != store_.active &&
	    QMessageBox::question(main_, T("DockDeck.Title"), T("DockDeck.Dialog.OverwriteConfirm").arg(name)) !=
		    QMessageBox::Yes)
		return;

	Deck fresh = capture(name, deck);
	fresh.id = deck->id;
	fresh.locked = deck->locked;
	fresh.scenes = deck->scenes;
	*deck = fresh;
	obs_log(LOG_INFO, "saved current docks into deck '%s'", name.toUtf8().constData());
	saveConfig();
}

void DockDeck::renameDeck(const QString &name)
{
	if (!store_.find(name))
		return;

	const QString newName = askForName(T("DockDeck.Dialog.RenameTitle"), name, name);
	Deck *deck = store_.find(name); // re-lookup after the modal dialog
	if (newName.isEmpty() || newName == name || !deck)
		return;

	deck->name = newName;
	if (store_.active == name)
		store_.active = newName;
	saveConfig();
	scheduleRefresh();
}

void DockDeck::deleteDeck(const QString &name)
{
	if (!store_.find(name))
		return;

	if (QMessageBox::question(main_, T("DockDeck.Title"), T("DockDeck.Dialog.DeleteConfirm").arg(name)) !=
	    QMessageBox::Yes)
		return;

	const qsizetype index = store_.indexOf(name);
	if (index < 0)
		return;
	store_.decks.removeAt(index);

	if (store_.active == name) {
		// Move to the neighbouring tab, like closing a browser tab. Clearing
		// active first stops auto-save from writing into the deleted deck.
		store_.active.clear();
		if (!store_.decks.isEmpty()) {
			const qsizetype next = qMin(index, store_.decks.size() - 1);
			switchToDeck(store_.decks[next].name, SwitchSource::Menu);
		}
	}

	obs_log(LOG_INFO, "deleted deck '%s'", name.toUtf8().constData());
	updateLock(); // e.g. the last deck is gone
	saveConfig();
	scheduleRefresh();
}

void DockDeck::moveDeck(int from, int to)
{
	// Mirrors a drag in the tab bar; no UI rebuild (that would cancel the drag).
	if (from < 0 || to < 0 || from >= store_.decks.size() || to >= store_.decks.size())
		return;
	store_.decks.move(from, to);
	saveConfig();
}

void DockDeck::setTabStripVisible(bool visible)
{
	store_.showTabStrip = visible;
	if (strip_ && ready_)
		strip_->setVisible(visible);
	saveConfig();
}

void DockDeck::setAutoSave(bool enabled)
{
	store_.autoSave = enabled;
	saveConfig();
}

// ---------------------------------------------------------------------------
// Deck locking
//
// A locked deck mirrors OBS's Docks > Lock Docks: every dock loses its
// Movable/Floatable/Closable features (each dock's own features are kept in a
// property and put back on unlock). On top of that we block resizing by
// swallowing presses on the separators, and auto-save skips the deck. OBS's
// own Lock Docks counts as a lock for every deck ("locked if either").
// ---------------------------------------------------------------------------

bool DockDeck::obsDocksLocked() const
{
	return obsLockAction_ && obsLockAction_->isChecked();
}

void DockDeck::updateLock()
{
	applyLock(store_.find(store_.active));
}

void DockDeck::applyLock(const Deck *deck)
{
	const bool deckLocked = deck && deck->locked;
	const bool obsLocked = obsDocksLocked();
	locked_ = deckLocked || obsLocked;

	const auto docks = main_->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *dock : docks) {
		const QVariant saved = dock->property(kSavedFeaturesProperty);
		if (deckLocked) {
			// Under OBS's lock the features are already off; OBS restores
			// them itself when it unlocks, and we re-save them then.
			if (!saved.isValid() && !obsLocked)
				dock->setProperty(kSavedFeaturesProperty, static_cast<int>(dock->features()));
			if (dock->features() != QDockWidget::NoDockWidgetFeatures)
				dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
		} else if (saved.isValid()) {
			if (!obsLocked)
				dock->setFeatures(QDockWidget::DockWidgetFeatures(saved.toInt()));
			dock->setProperty(kSavedFeaturesProperty, QVariant());
		}
	}
	resetSeparatorCursor(); // never leave a resize cursor behind on (un)lock
}

// QMainWindow sets a resize cursor on itself while the mouse is over a
// separator and only takes it back on hover/leave events. A plain
// unsetCursor() isn't enough (its CursorChange handler re-applies the resize
// cursor), so hand it the HoverLeave it uses to restore the normal cursor.
void DockDeck::resetSeparatorCursor()
{
	if (!main_->testAttribute(Qt::WA_SetCursor))
		return;
	switch (main_->cursor().shape()) {
	case Qt::SplitHCursor:
	case Qt::SplitVCursor:
	case Qt::SizeHorCursor:
	case Qt::SizeVerCursor: {
		QHoverEvent leave(QEvent::HoverLeave, QPointF(), QPointF(), QPointF());
		QCoreApplication::sendEvent(main_, &leave);
		break;
	}
	default:
		break;
	}
}

void DockDeck::toggleLock(const QString &name)
{
	if (const Deck *deck = store_.find(name))
		setDeckLocked(name, !deck->locked);
}

bool DockDeck::setDeckLocked(const QString &name, bool locked)
{
	Deck *deck = store_.find(name);
	if (!deck)
		return false;
	if (deck->locked == locked)
		return true;

	// Locking the deck on screen: keep what is on screen right now first.
	if (locked && name == store_.active && ready_ && store_.autoSave)
		captureActive();
	deck = store_.find(name);
	if (!deck)
		return false;

	deck->locked = locked;
	obs_log(LOG_INFO, "%s deck '%s'", deck->locked ? "locked" : "unlocked", name.toUtf8().constData());
	if (ready_)
		updateLock();
	saveConfig();
	scheduleRefresh();
	return true;
}

void DockDeck::setAllLocked(bool locked)
{
	if (locked && ready_ && store_.autoSave)
		captureActive();
	for (Deck &deck : store_.decks)
		deck.locked = locked;
	obs_log(LOG_INFO, "%s all decks", locked ? "locked" : "unlocked");
	if (ready_)
		updateLock();
	saveConfig();
	scheduleRefresh();
}

// ---------------------------------------------------------------------------
// Linked scenes
//
// A deck can link one scene per scene collection (stored in the deck, so it
// follows renames of the deck and travels with exports). Selecting the deck
// (tab, menu, hotkey, obs-websocket) switches OBS to that scene, unless it is
// already showing or no longer exists. In Studio Mode the scene goes to the
// Preview instead, so picking a deck never cuts the live Program; the
// transition stays in your hands. Startup and import never change scenes.
// Scene renames are followed (libobs "source_rename"); deleted scenes just
// stop switching and show as missing in the tab tooltip.
// ---------------------------------------------------------------------------

QString DockDeck::linkedScene(const Deck &deck) const
{
	return deck.scenes.value(currentCollection_);
}

void DockDeck::activateLinkedScene(const QString &deckName)
{
	const Deck *deck = store_.find(deckName);
	if (!deck)
		return;
	const QString scene = linkedScene(*deck);
	if (scene.isEmpty())
		return;

	obs_source_t *source = obs_get_source_by_name(scene.toUtf8().constData());
	if (!source || !obs_source_is_scene(source)) {
		obs_source_release(source);
		obs_log(LOG_INFO, "deck '%s': linked scene '%s' not found; not switching scenes",
			deckName.toUtf8().constData(), scene.toUtf8().constData());
		return;
	}

	const bool studio = obs_frontend_preview_program_mode_active();
	obs_source_t *current = studio ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();
	if (current != source) {
		if (studio)
			obs_frontend_set_current_preview_scene(source);
		else
			obs_frontend_set_current_scene(source);
		obs_log(LOG_INFO, "deck '%s': switched %s to scene '%s'", deckName.toUtf8().constData(),
			studio ? "preview" : "program", scene.toUtf8().constData());
	}
	obs_source_release(current);
	obs_source_release(source);
}

bool DockDeck::setDeckScene(const QString &name, const QString &scene)
{
	Deck *deck = store_.find(name);
	if (!deck || currentCollection_.isEmpty())
		return false;
	if (scene.isEmpty()) {
		deck->scenes.remove(currentCollection_);
		obs_log(LOG_INFO, "deck '%s': scene link removed", name.toUtf8().constData());
	} else {
		if (!sceneExists(scene))
			return false;
		deck->scenes.insert(currentCollection_, scene);
		obs_log(LOG_INFO, "deck '%s': linked to scene '%s'", name.toUtf8().constData(),
			scene.toUtf8().constData());
	}
	saveConfig();
	scheduleRefresh();
	return true;
}

void DockDeck::onSourceRenamed(const QString &oldName, const QString &newName)
{
	if (currentCollection_.isEmpty() || oldName == newName)
		return;
	bool changed = false;
	for (Deck &deck : store_.decks) {
		auto it = deck.scenes.find(currentCollection_);
		if (it != deck.scenes.end() && it.value() == oldName) {
			it.value() = newName;
			changed = true;
		}
	}
	if (changed) {
		obs_log(LOG_INFO, "scene '%s' renamed to '%s'; deck links follow", oldName.toUtf8().constData(),
			newName.toUtf8().constData());
		saveConfig();
		scheduleRefresh();
	}
}

void DockDeck::onFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_RENAMED: {
		// OBS doesn't say the old name; it is the one we last saw.
		const QString now = currentCollectionName();
		if (!currentCollection_.isEmpty() && !now.isEmpty() && now != currentCollection_) {
			for (Deck &deck : store_.decks) {
				if (deck.scenes.contains(currentCollection_))
					deck.scenes.insert(now, deck.scenes.take(currentCollection_));
			}
			currentCollection_ = now;
			saveConfig();
		}
		scheduleRefresh();
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		currentCollection_ = currentCollectionName();
		publishSnapshot();
		scheduleRefresh();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		scheduleRefresh(); // a linked scene may have appeared or gone
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Import / export
// ---------------------------------------------------------------------------

// "Name (2)", "Name (3)", ... first one that is free.
QString DockDeck::uniqueName(const QString &base) const
{
	for (int n = 2;; ++n) {
		const QString candidate = QStringLiteral("%1 (%2)").arg(base).arg(n);
		if (!store_.nameTaken(candidate))
			return candidate;
	}
}

void DockDeck::exportDecks(const QString &onlyName)
{
	if (!ready_)
		return;

	// Make sure the active deck reflects what is on screen right now.
	if (store_.autoSave)
		captureActive();

	// A collection: let the user pick which decks go into it.
	const QStringList names = onlyName.isEmpty() ? pickDecksForExport() : QStringList{onlyName};
	QVector<Deck> decks;
	for (const QString &name : names) {
		if (const Deck *deck = store_.find(name))
			decks.append(*deck);
	}
	if (decks.isEmpty())
		return;

	// Suggest "<Deck name>.dockdeck.json" (minus characters that are not
	// allowed in file names), or "Deck Collection.dockdeck.json" for several.
	QString base = decks.size() == 1 ? decks.front().name : T("DockDeck.CollectionFileName");
	base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
	const QString fileName = base.trimmed() + QStringLiteral(".dockdeck.json");
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

	const QString title =
		T(onlyName.isEmpty() ? "DockDeck.Dialog.ExportCollectionTitle" : "DockDeck.Dialog.ExportDeckTitle");
	const QString path = QFileDialog::getSaveFileName(main_, title, QDir(dir).filePath(fileName),
							  T("DockDeck.Dialog.FileFilter"));
	if (path.isEmpty())
		return;

	QString error;
	if (!DeckFile::write(path, decks, &error)) {
		QMessageBox::warning(main_, title, T("DockDeck.Dialog.ExportFailed").arg(path, error));
		return;
	}
	obs_log(LOG_INFO, "exported %d deck(s) to %s", static_cast<int>(decks.size()), path.toUtf8().constData());
}

void DockDeck::importDecks()
{
	if (!ready_)
		return;

	const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	const QString path = QFileDialog::getOpenFileName(main_, T("DockDeck.Dialog.ImportTitle"), dir,
							  T("DockDeck.Dialog.FileFilter"));
	if (path.isEmpty())
		return;

	DeckFile::ReadResult file;
	const DeckFile::ReadError err = DeckFile::read(path, &file);
	if (err != DeckFile::ReadError::None) {
		const char *key = "DockDeck.Import.Error.Invalid";
		switch (err) {
		case DeckFile::ReadError::CannotOpen:
			key = "DockDeck.Import.Error.CannotOpen";
			break;
		case DeckFile::ReadError::TooNew:
			key = "DockDeck.Import.Error.TooNew";
			break;
		case DeckFile::ReadError::NoDecks:
			key = "DockDeck.Import.Error.NoDecks";
			break;
		default:
			break;
		}
		QMessageBox::warning(main_, T("DockDeck.Dialog.ImportTitle"), T(key));
		return;
	}

	// Keep the active tab's current docks before anything changes.
	if (store_.autoSave)
		captureActive();

	// Docks that exist here, to report what the imported decks can't place.
	// (placeholders don't count: they stand in for docks that are missing).
	const QHash<QString, QDockWidget *> localDocks = realDocks();
	QSet<QString> missingDocks;

	int added = 0, replaced = 0, skipped = 0;
	bool reapplyActive = false;

	// "Do the same for the rest" remembers the answer for later clashes.
	enum class Choice { Ask, Replace, KeepBoth, Skip };
	Choice remembered = Choice::Ask;

	for (Deck deck : file.decks) {
		// Case-insensitive clash, matching how names are validated.
		Deck *existing = nullptr;
		for (Deck &l : store_.decks) {
			if (l.name.compare(deck.name, Qt::CaseInsensitive) == 0) {
				existing = &l;
				break;
			}
		}

		Choice choice = existing ? remembered : Choice::KeepBoth;
		if (existing && choice == Choice::Ask) {
			QMessageBox box(QMessageBox::Question, T("DockDeck.Dialog.ImportTitle"),
					T("DockDeck.Import.Clash").arg(deck.name), QMessageBox::NoButton, main_);
			QPushButton *replaceBtn = box.addButton(T("DockDeck.Import.Replace"), QMessageBox::AcceptRole);
			QPushButton *keepBtn = box.addButton(T("DockDeck.Import.KeepBoth"), QMessageBox::AcceptRole);
			QPushButton *skipBtn = box.addButton(T("DockDeck.Import.Skip"), QMessageBox::RejectRole);
			box.setDefaultButton(keepBtn);
			box.setEscapeButton(skipBtn);
			auto *applyToAll = new QCheckBox(T("DockDeck.Import.ApplyToAll"), &box);
			box.setCheckBox(applyToAll);
			box.exec();

			if (box.clickedButton() == replaceBtn)
				choice = Choice::Replace;
			else if (box.clickedButton() == keepBtn)
				choice = Choice::KeepBoth;
			else
				choice = Choice::Skip;
			if (applyToAll->isChecked())
				remembered = choice;
		}

		if (choice == Choice::Skip) {
			++skipped;
			continue;
		}

		// Only docks this imported deck actually shows. A deck also lists
		// docks it keeps hidden (often another deck's missing dock), and
		// those don't matter here.
		for (auto it = deck.docks.constBegin(); it != deck.docks.constEnd(); ++it) {
			if (it.value() && !localDocks.contains(it.key()))
				missingDocks.insert(deck.titles.value(it.key(), it.key()));
		}

		if (existing && choice == Choice::Replace) {
			// Keep the existing tab's name and position; take the docks.
			const QString keepName = existing->name;
			const QString keepId = existing->id; // keeps its hotkey binding
			*existing = deck;
			existing->name = keepName;
			existing->id = keepId;
			if (keepName == store_.active)
				reapplyActive = true;
			++replaced;
		} else {
			if (existing)
				deck.name = uniqueName(deck.name);
			// Same id as a deck here (e.g. re-importing a copy): new id.
			if (store_.idTaken(deck.id))
				deck.id.clear();
			store_.decks.append(deck);
			store_.ensureIds();
			++added;
		}
	}

	// The tab you are on was replaced: show its new docks right away. Clearing
	// active first makes switchToDeck() apply it without auto-saving over it.
	if (reapplyActive) {
		const QString name = store_.active;
		store_.active.clear();
		switchToDeck(name, SwitchSource::Import);
	}

	obs_log(LOG_INFO, "imported from %s: %d added, %d replaced, %d skipped, %d invalid", path.toUtf8().constData(),
		added, replaced, skipped, file.invalidEntries);
	saveConfig();
	scheduleRefresh();

	QString summary = T("DockDeck.Import.Summary").arg(added).arg(replaced).arg(skipped);
	if (file.invalidEntries > 0)
		summary += QStringLiteral("\n") + T("DockDeck.Import.InvalidEntries").arg(file.invalidEntries);
	if (!missingDocks.isEmpty()) {
		QStringList names = missingDocks.values();
		names.sort(Qt::CaseInsensitive);
		summary += QStringLiteral("\n\n") +
			   T("DockDeck.Import.MissingDocks").arg(names.join(QStringLiteral(", ")));
	}
	QMessageBox::information(main_, T("DockDeck.Dialog.ImportTitle"), summary);
}
