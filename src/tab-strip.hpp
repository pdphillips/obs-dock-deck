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

#include <QString>
#include <QStringList>
#include <QToolBar>

class QTabBar;
class QToolButton;

class TabStrip : public QToolBar {
	Q_OBJECT

public:
	explicit TabStrip(QWidget *parent);

	// Rebuilds the tabs without emitting layoutActivated().
	void setLayouts(const QStringList &names, const QString &active);

signals:
	void layoutActivated(const QString &name);
	void newLayoutRequested();
	void overwriteRequested(const QString &name);
	void renameRequested(const QString &name);
	void deleteRequested(const QString &name);
	void hideRequested();
	void layoutMoved(int from, int to);

private:
	void showContextMenu(const QPoint &pos);
	QString nameAt(int index) const;

	QTabBar *tabs_ = nullptr;
	QToolButton *addButton_ = nullptr;
};
