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

// The slim tab strip that sits directly under OBS's menu bar.
//
// It is a fixed (non-movable, non-floatable) QToolBar in the main window's
// top toolbar area holding a compact QTabBar and a small "+" button. The
// strip is pure view: it only emits signals, and DockDeck decides what to do.

#include <QSet>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QVector>

#include <functional>

class QTabBar;
class QToolButton;

class DeckStrip : public QToolBar {
	Q_OBJECT

public:
	explicit DeckStrip(QWidget *parent);

	struct Tab {
		QString name;
		bool locked = false;
		QString linkedScene; // in the current scene collection; "" = none
		bool sceneMissing = false;
	};

	// Rebuilds the tabs without emitting deckActivated().
	void setDecks(const QVector<Tab> &tabs, const QString &active);

	// Name of OBS's current scene, shown in "Link to Current Scene".
	std::function<QString()> currentScene;

signals:
	void deckActivated(const QString &name);
	void newDeckRequested();
	void overwriteRequested(const QString &name);
	void renameRequested(const QString &name);
	void deleteRequested(const QString &name);
	void exportRequested(const QString &name);
	void lockToggled(const QString &name);
	void linkSceneRequested(const QString &name);
	void unlinkSceneRequested(const QString &name);
	void hideRequested();
	void deckMoved(int from, int to);

private:
	void showContextMenu(const QPoint &pos);
	QString nameAt(int index) const;

	QTabBar *tabs_ = nullptr;
	QToolButton *addButton_ = nullptr;
	QVector<Tab> tabInfo_;
	const Tab *tabFor(const QString &name) const;
};
