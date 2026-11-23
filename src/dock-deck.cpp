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
#include "tab-strip.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QDockWidget>
#include <QCheckBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

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

	if (!configPath_.isEmpty()) {
		QDir().mkpath(QFileInfo(configPath_).absolutePath());
		switch (store_.load(configPath_)) {
		case LayoutStore::LoadResult::Ok:
			obs_log(LOG_INFO, "loaded %d layout(s) from %s", static_cast<int>(store_.layouts.size()),
				configPath_.toUtf8().constData());
			break;
		case LayoutStore::LoadResult::Missing:
			obs_log(LOG_INFO, "no layouts yet; a default one is created when OBS finishes loading");
			break;
		case LayoutStore::LoadResult::Corrupt:
			obs_log(LOG_WARNING, "layouts file was unreadable; kept it as %s.bad and started fresh",
				configPath_.toUtf8().constData());
			break;
		}
	}

	// Tab strip: a fixed toolbar in the top toolbar area, which Qt places
	// directly under the menu bar and above every dock and the preview. It
	// stays hidden until OBS has finished loading and a layout is applied.
	strip_ = new TabStrip(main_);
	main_->addToolBar(Qt::TopToolBarArea, strip_);
	strip_->setVisible(false);

	connect(strip_, &TabStrip::layoutActivated, this,
		[this](const QString &name) { switchToLayout(name, SwitchSource::Tab); });
	connect(strip_, &TabStrip::newLayoutRequested, this, &DockDeck::newLayoutFromCurrent);
	connect(strip_, &TabStrip::overwriteRequested, this, &DockDeck::overwriteLayout);
	connect(strip_, &TabStrip::renameRequested, this, &DockDeck::renameLayout);
	connect(strip_, &TabStrip::deleteRequested, this, &DockDeck::deleteLayout);
	connect(strip_, &TabStrip::exportRequested, this, [this](const QString &name) { exportLayouts(name); });
	connect(strip_, &TabStrip::hideRequested, this, [this]() { setTabStripVisible(false); });
	connect(strip_, &TabStrip::layoutMoved, this, &DockDeck::moveLayout);

	// Tools > Dock Deck submenu (switch layouts, toggle the strip, options).
	// It is rebuilt each time it opens so it always matches the layout list.
	toolsAction_ = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("DockDeck.Title")));
	if (toolsAction_) {
		toolsMenu_ = new QMenu(main_);
		toolsAction_->setMenu(toolsMenu_);
		connect(toolsMenu_, &QMenu::aboutToShow, this, &DockDeck::rebuildToolsMenu);
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

bool DockDeck::switchToLayout(const QString &name, SwitchSource source)
{
	// Hotkeys and websocket requests arrive on OBS worker threads; widgets may
	// only be touched on the UI thread.
	if (QThread::currentThread() != thread()) {
		QMetaObject::invokeMethod(
			this, [this, name, source]() { switchToLayout(name, source); }, Qt::QueuedConnection);
		return true;
	}

	if (applying_)
		return false;

	DockLayout *target = store_.find(name);
	if (!target) {
		obs_log(LOG_WARNING, "switch to unknown layout '%s' (%s) ignored", name.toUtf8().constData(),
			sourceName(source));
		scheduleRefresh(); // e.g. resync the strip if it showed something stale
		return false;
	}

	// Before OBS finishes loading, just remember the choice.
	if (!ready_) {
		store_.active = name;
		return true;
	}

	if (name == store_.active)
		return true;

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

	obs_log(LOG_INFO, "switched to layout '%s' (%s)", name.toUtf8().constData(), sourceName(source));
	saveConfig();
	scheduleRefresh();
	return true;
}

// Snapshot the main window's current dock arrangement.
DockLayout DockDeck::capture(const QString &name) const
{
	DockLayout layout;
	layout.name = name;
	layout.state = main_->saveState();

	// Only docks that belong to the main window itself; floating docks keep
	// the main window as parent, so they are included too. A dock's "shown"
	// state is its Docks-menu checkmark (toggleViewAction), which stays true
	// for docks that are merely behind another tab.
	const auto docks = main_->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *dock : docks) {
		if (!dock->objectName().isEmpty())
			layout.docks.insert(dock->objectName(), dock->toggleViewAction()->isChecked());
	}
	return layout;
}

// Apply a saved arrangement. Missing docks are skipped, never fatal.
void DockDeck::apply(const DockLayout &layout)
{
	const QScopedValueRollback<bool> guard(applying_, true);

	// restoreState() ignores docks it doesn't know and keeps placeholders for
	// saved docks that don't exist right now, so a missing plugin is harmless.
	if (!layout.state.isEmpty() && !main_->restoreState(layout.state))
		obs_log(LOG_WARNING, "layout '%s': Qt could not restore the saved dock state",
			layout.name.toUtf8().constData());

	// Then enforce shown/hidden for every dock the layout knows about.
	const auto docks = main_->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *dock : docks) {
		const auto it = layout.docks.constFind(dock->objectName());
		if (it == layout.docks.constEnd())
			continue; // dock is newer than the layout: leave it alone
		if (dock->toggleViewAction()->isChecked() != it.value())
			dock->setVisible(it.value());
	}

	// The strip is part of saveState(), so put it back the way the user wants.
	if (strip_)
		strip_->setVisible(store_.showTabStrip);
}

void DockDeck::captureActive()
{
	DockLayout *active = store_.find(store_.active);
	if (active)
		*active = capture(active->name);
}

// ---------------------------------------------------------------------------
// OBS lifecycle
// ---------------------------------------------------------------------------

void DockDeck::onFinishedLoading()
{
	// Defer one event-loop turn so plugins that add docks in their own
	// FINISHED_LOADING handler have done so before we restore.
	QTimer::singleShot(0, this, [this]() {
		if (store_.layouts.isEmpty()) {
			// First run: the current arrangement becomes the first tab.
			store_.layouts.append(capture(T("DockDeck.DefaultLayoutName")));
			store_.active = store_.layouts.front().name;
		} else {
			if (!store_.find(store_.active))
				store_.active = store_.layouts.front().name;
			apply(*store_.find(store_.active));
			obs_log(LOG_INFO, "restored layout '%s' (startup)", store_.active.toUtf8().constData());
		}

		ready_ = true;
		if (strip_)
			strip_->setVisible(store_.showTabStrip);
		saveConfig();
		refreshUi();
	});
}

void DockDeck::onExit()
{
	// Dock state was already captured on the Close event; just write it out.
	if (ready_)
		saveConfig();
	ready_ = false; // ignore anything that arrives during teardown
}

bool DockDeck::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == main_ && event->type() == QEvent::Close && ready_ && store_.autoSave) {
		captureActive();
		saveConfig();
	}
	return QObject::eventFilter(watched, event);
}

void DockDeck::saveConfig()
{
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
	if (strip_)
		strip_->setLayouts(store_.names(), store_.active);
}

void DockDeck::rebuildToolsMenu()
{
	toolsMenu_->clear();

	// One checkable entry per layout; picking one switches to it.
	auto *group = new QActionGroup(toolsMenu_);
	for (const DockLayout &layout : store_.layouts) {
		const QString name = layout.name;
		QAction *a = toolsMenu_->addAction(QString(name).replace(QLatin1Char('&'), QStringLiteral("&&")));
		a->setCheckable(true);
		a->setChecked(name == store_.active);
		a->setEnabled(ready_);
		group->addAction(a);
		connect(a, &QAction::triggered, this, [this, name]() { switchToLayout(name, SwitchSource::Menu); });
	}
	if (!store_.layouts.isEmpty())
		toolsMenu_->addSeparator();

	QAction *add = toolsMenu_->addAction(T("DockDeck.Menu.New"));
	add->setEnabled(ready_);
	connect(add, &QAction::triggered, this, &DockDeck::newLayoutFromCurrent);
	QAction *save = toolsMenu_->addAction(T("DockDeck.Menu.SaveActive"));
	connect(save, &QAction::triggered, this, [this]() { overwriteLayout(store_.active); });
	save->setEnabled(ready_ && store_.find(store_.active) != nullptr);
	toolsMenu_->addSeparator();

	QAction *exportAll = toolsMenu_->addAction(T("DockDeck.Menu.Export"));
	exportAll->setEnabled(ready_ && !store_.layouts.isEmpty());
	connect(exportAll, &QAction::triggered, this, [this]() { exportLayouts(); });
	QAction *import = toolsMenu_->addAction(T("DockDeck.Menu.Import"));
	import->setEnabled(ready_);
	connect(import, &QAction::triggered, this, &DockDeck::importLayouts);
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
	toolsMenu_->setToolTipsVisible(true);
}

// ---------------------------------------------------------------------------
// User actions
// ---------------------------------------------------------------------------

QString DockDeck::askForName(const QString &title, const QString &initial, const QString &ignore)
{
	QString name = initial;
	for (;;) {
		bool ok = false;
		name = QInputDialog::getText(main_, title, T("DockDeck.Dialog.NameLabel"), QLineEdit::Normal, name, &ok)
			       .trimmed();
		if (!ok || name.isEmpty())
			return QString();
		if (!store_.nameTaken(name, ignore))
			return name;
		QMessageBox::warning(main_, title, T("DockDeck.Dialog.NameTaken"));
	}
}

void DockDeck::newLayoutFromCurrent()
{
	if (!ready_)
		return;

	// Suggest "Layout N" with the first free number.
	QString suggestion;
	for (int n = static_cast<int>(store_.layouts.size()) + 1;; ++n) {
		suggestion = T("DockDeck.NewLayoutName").arg(n);
		if (!store_.nameTaken(suggestion))
			break;
	}

	const QString name = askForName(T("DockDeck.Dialog.NewTitle"), suggestion, QString());
	if (name.isEmpty())
		return;

	// The current docks become the new layout, which becomes active. The tab
	// we came from keeps its own last saved/auto-saved arrangement.
	if (store_.autoSave)
		captureActive();
	store_.layouts.append(capture(name));
	store_.active = name;
	obs_log(LOG_INFO, "created layout '%s'", name.toUtf8().constData());
	saveConfig();
	scheduleRefresh();
}

void DockDeck::overwriteLayout(const QString &name)
{
	DockLayout *layout = store_.find(name);
	if (!ready_ || !layout)
		return;

	// Overwriting a layout you are not looking at loses what it had, so ask.
	if (name != store_.active &&
	    QMessageBox::question(main_, T("DockDeck.Title"), T("DockDeck.Dialog.OverwriteConfirm").arg(name)) !=
		    QMessageBox::Yes)
		return;

	*layout = capture(name);
	obs_log(LOG_INFO, "saved current docks into layout '%s'", name.toUtf8().constData());
	saveConfig();
}

void DockDeck::renameLayout(const QString &name)
{
	if (!store_.find(name))
		return;

	const QString newName = askForName(T("DockDeck.Dialog.RenameTitle"), name, name);
	DockLayout *layout = store_.find(name); // re-lookup after the modal dialog
	if (newName.isEmpty() || newName == name || !layout)
		return;

	layout->name = newName;
	if (store_.active == name)
		store_.active = newName;
	saveConfig();
	scheduleRefresh();
}

void DockDeck::deleteLayout(const QString &name)
{
	if (!store_.find(name))
		return;

	if (QMessageBox::question(main_, T("DockDeck.Title"), T("DockDeck.Dialog.DeleteConfirm").arg(name)) !=
	    QMessageBox::Yes)
		return;

	const qsizetype index = store_.indexOf(name);
	if (index < 0)
		return;
	store_.layouts.removeAt(index);

	if (store_.active == name) {
		// Move to the neighbouring tab, like closing a browser tab. Clearing
		// active first stops auto-save from writing into the deleted layout.
		store_.active.clear();
		if (!store_.layouts.isEmpty()) {
			const qsizetype next = qMin(index, store_.layouts.size() - 1);
			switchToLayout(store_.layouts[next].name, SwitchSource::Menu);
		}
	}

	obs_log(LOG_INFO, "deleted layout '%s'", name.toUtf8().constData());
	saveConfig();
	scheduleRefresh();
}

void DockDeck::moveLayout(int from, int to)
{
	// Mirrors a drag in the tab bar; no UI rebuild (that would cancel the drag).
	if (from < 0 || to < 0 || from >= store_.layouts.size() || to >= store_.layouts.size())
		return;
	store_.layouts.move(from, to);
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

void DockDeck::exportLayouts(const QString &onlyName)
{
	if (!ready_)
		return;

	// Make sure the active layout reflects what is on screen right now.
	if (store_.autoSave)
		captureActive();

	QVector<DockLayout> layouts;
	if (onlyName.isEmpty()) {
		layouts = store_.layouts;
	} else if (const DockLayout *layout = store_.find(onlyName)) {
		layouts.append(*layout);
	}
	if (layouts.isEmpty())
		return;

	// Suggest a file name: the layout's name (minus characters that are not
	// allowed in file names), or a generic one for "export all".
	QString fileName = QStringLiteral("dock-deck-layouts.json");
	if (!onlyName.isEmpty()) {
		QString safe = onlyName;
		safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
		fileName = QStringLiteral("dock-deck-%1.json").arg(safe.trimmed());
	}
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);

	const QString path = QFileDialog::getSaveFileName(
		main_, T("DockDeck.Dialog.ExportTitle"), QDir(dir).filePath(fileName), T("DockDeck.Dialog.FileFilter"));
	if (path.isEmpty())
		return;

	QString error;
	if (!LayoutFile::write(path, layouts, &error)) {
		QMessageBox::warning(main_, T("DockDeck.Dialog.ExportTitle"),
				     T("DockDeck.Dialog.ExportFailed").arg(path, error));
		return;
	}
	obs_log(LOG_INFO, "exported %d layout(s) to %s", static_cast<int>(layouts.size()), path.toUtf8().constData());
}

void DockDeck::importLayouts()
{
	if (!ready_)
		return;

	const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	const QString path = QFileDialog::getOpenFileName(main_, T("DockDeck.Dialog.ImportTitle"), dir,
							  T("DockDeck.Dialog.FileFilter"));
	if (path.isEmpty())
		return;

	LayoutFile::ReadResult file;
	const LayoutFile::ReadError err = LayoutFile::read(path, &file);
	if (err != LayoutFile::ReadError::None) {
		const char *key = "DockDeck.Import.Error.Invalid";
		switch (err) {
		case LayoutFile::ReadError::CannotOpen:
			key = "DockDeck.Import.Error.CannotOpen";
			break;
		case LayoutFile::ReadError::TooNew:
			key = "DockDeck.Import.Error.TooNew";
			break;
		case LayoutFile::ReadError::NoLayouts:
			key = "DockDeck.Import.Error.NoLayouts";
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

	// Docks that exist here, to report what the imported layouts can't place.
	QSet<QString> localDocks;
	for (QDockWidget *dock : main_->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly))
		localDocks.insert(dock->objectName());
	QSet<QString> missingDocks;

	int added = 0, replaced = 0, skipped = 0;
	bool reapplyActive = false;

	// "Do the same for the rest" remembers the answer for later clashes.
	enum class Choice { Ask, Replace, KeepBoth, Skip };
	Choice remembered = Choice::Ask;

	for (DockLayout layout : file.layouts) {
		// Case-insensitive clash, matching how names are validated.
		DockLayout *existing = nullptr;
		for (DockLayout &l : store_.layouts) {
			if (l.name.compare(layout.name, Qt::CaseInsensitive) == 0) {
				existing = &l;
				break;
			}
		}

		Choice choice = existing ? remembered : Choice::KeepBoth;
		if (existing && choice == Choice::Ask) {
			QMessageBox box(QMessageBox::Question, T("DockDeck.Dialog.ImportTitle"),
					T("DockDeck.Import.Clash").arg(layout.name), QMessageBox::NoButton, main_);
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

		for (auto it = layout.docks.constBegin(); it != layout.docks.constEnd(); ++it) {
			if (!localDocks.contains(it.key()))
				missingDocks.insert(it.key());
		}

		if (existing && choice == Choice::Replace) {
			// Keep the existing tab's name and position; take the docks.
			const QString keepName = existing->name;
			*existing = layout;
			existing->name = keepName;
			if (keepName == store_.active)
				reapplyActive = true;
			++replaced;
		} else {
			if (existing)
				layout.name = uniqueName(layout.name);
			store_.layouts.append(layout);
			++added;
		}
	}

	// The tab you are on was replaced: show its new docks right away. Clearing
	// active first makes switchToLayout() apply it without auto-saving over it.
	if (reapplyActive) {
		const QString name = store_.active;
		store_.active.clear();
		switchToLayout(name, SwitchSource::Import);
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
