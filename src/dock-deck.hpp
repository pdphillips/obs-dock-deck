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

#pragma once

// DockDeck owns the layout list, the tab strip, the Tools menu entry, and the
// one function every layout switch goes through: switchToLayout().

#include "layout-store.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

class QAction;
class QMainWindow;
class QMenu;
class TabStrip;

// Where a switch request came from. Only tabs/menu/startup exist today; the
// rest are reserved so future triggers (and the optional fade) plug into
// switchToLayout() without changing its signature.
enum class SwitchSource { Tab, Menu, Startup, Hotkey, Websocket, SceneLink, Api };

class DockDeck : public QObject {
	Q_OBJECT

public:
	explicit DockDeck(QMainWindow *mainWindow);
	~DockDeck() override;

	// THE layout switch. Safe to call from any thread (it hops to the UI
	// thread), safe to call before OBS finished loading (the request is
	// remembered and applied then), and never crashes on missing docks.
	// Returns false if the layout doesn't exist.
	bool switchToLayout(const QString &name, SwitchSource source);

	// Called from the OBS frontend event callback.
	void onFinishedLoading();
	void onExit();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	DockLayout capture(const QString &name) const;
	void apply(const DockLayout &layout);
	void captureActive();
	void saveConfig();
	void scheduleRefresh();
	void refreshUi();
	void rebuildToolsMenu();

	// User actions (tab strip and Tools menu).
	void newLayoutFromCurrent();
	void overwriteLayout(const QString &name);
	void renameLayout(const QString &name);
	void deleteLayout(const QString &name);
	void moveLayout(int from, int to);
	void setTabStripVisible(bool visible);
	void setAutoSave(bool enabled);

	QString askForName(const QString &title, const QString &initial, const QString &ignore);

	QMainWindow *main_;
	QPointer<TabStrip> strip_;
	QPointer<QAction> toolsAction_;
	QMenu *toolsMenu_ = nullptr;

	LayoutStore store_;
	QString configPath_;
	bool ready_ = false;    // OBS finished loading and the active layout was applied
	bool applying_ = false; // re-entrancy guard for switchToLayout()
	bool refreshQueued_ = false;
};
