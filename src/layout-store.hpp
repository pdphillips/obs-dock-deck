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

// Plain data model for Dock Deck layouts plus JSON persistence.
// Qt Core only: no widgets and no OBS calls, so it is easy to reason about.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// One named dock arrangement.
struct DockLayout {
	QString name;

	// Raw bytes from QMainWindow::saveState(). Stored base64 in the JSON file.
	QByteArray state;

	// objectName -> "shown" for every dock that existed when the layout was
	// saved. Docks that are not listed here (for example a plugin installed
	// later) are left alone when the layout is applied.
	QHash<QString, bool> docks;
};

class LayoutStore {
public:
	enum class LoadResult { Ok, Missing, Corrupt };

	// Reads the JSON file at path. A corrupt file is renamed to
	// "<path>.bad" so it is never silently overwritten.
	LoadResult load(const QString &path);

	// Writes atomically (QSaveFile). Returns false on I/O errors.
	bool save(const QString &path) const;

	// Exact-name lookup; -1 when not found.
	qsizetype indexOf(const QString &name) const;
	DockLayout *find(const QString &name);

	// Case-insensitive check used when naming layouts, so "Gaming" and
	// "gaming" can't both exist and confuse hotkeys/remote commands later.
	bool nameTaken(const QString &name, const QString &ignore = QString()) const;

	QStringList names() const;

	QVector<DockLayout> layouts;
	QString active;           // name of the active layout ("" if none)
	bool showTabStrip = true; // tab strip under the menu bar
	bool autoSave = true;     // keep dock changes in the active layout
};
