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

// Plain data model for Dock Deck decks plus JSON persistence.
// A "Deck" is one named dock arrangement (a tab); all decks together are the
// "Deck Collection". JSON keys still say "layouts" for backward compatibility.
// Qt Core only: no widgets and no OBS calls, so it is easy to reason about.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonValue>
#include <QVector>

// One named dock arrangement.
struct Deck {
	// Stable identity (a UUID) that survives renames. Hotkey bindings and
	// remote commands use it. Decks saved before 0.4.0 get one on load.
	QString id;
	QString name;

	// Raw bytes from QMainWindow::saveState(). Stored base64 in the JSON file.
	QByteArray state;

	// objectName -> "shown" for every dock that existed when the deck was
	// saved. Docks that are not listed here (for example a plugin installed
	// later) are left alone when the deck is applied.
	QHash<QString, bool> docks;

	// objectName -> human-readable dock title (as shown in OBS's Docks menu),
	// so placeholders for missing docks can show a friendly name. Optional:
	// decks saved before 0.2.2 have no titles.
	QHash<QString, QString> titles;

	// Locked decks can't be rearranged (docks can't be moved, resized,
	// floated or closed) and are never auto-saved. Optional in files: decks
	// saved before 0.3.0 are unlocked.
	bool locked = false;

	// Optional linked scene per scene collection (collection name -> scene
	// name). Selecting the deck switches OBS to that scene if it exists in
	// the current collection. New in 0.5.0.
	QHash<QString, QString> scenes;
};

// Export/import of decks to a standalone JSON file, so they can be shared
// between machines. Decks reference docks by their internal names, so docks
// from plugins missing on the importing machine are simply skipped on apply.
namespace DeckFile {
// One deck as JSON and back (validated; used by Deck packs).
QJsonObject toJson(const Deck &deck);
bool fromJson(const QJsonValue &value, Deck *out);
} // namespace DeckFile

class DeckStore {
public:
	enum class LoadResult { Ok, Missing, Corrupt };

	// Reads the JSON file at path. A corrupt file is renamed to
	// "<path>.bad" so it is never silently overwritten.
	LoadResult load(const QString &path);

	// Writes atomically (QSaveFile). Returns false on I/O errors.
	bool save(const QString &path) const;

	// Exact-name lookup; -1 when not found.
	qsizetype indexOf(const QString &name) const;
	Deck *find(const QString &name);

	// Case-insensitive check used when naming decks, so "Gaming" and
	// "gaming" can't both exist and confuse hotkeys/remote commands later.
	bool nameTaken(const QString &name, const QString &ignore = QString()) const;

	QStringList names() const;

	Deck *findById(const QString &id);
	bool idTaken(const QString &id) const;

	// Gives every deck without an id (or with a duplicate one) a fresh
	// UUID. Returns true if anything changed.
	bool ensureIds();

	static QString newId();

	QVector<Deck> decks;
	QString active;               // name of the active deck ("" if none)
	bool showTabStrip = true;     // tab strip under the menu bar
	bool autoSave = true;         // keep dock changes in the active deck
	bool showPlaceholders = true; // stand-in panels for docks missing here
};
