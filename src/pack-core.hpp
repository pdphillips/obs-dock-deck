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

// Deck packs, the data side (Qt Core + libobs, no widgets): which scenes,
// sources and media files the current scene collection has, what goes into
// a .dockdeck file, and how its scenes/sources are created again on import.
// The dialogs live in deck-pack.cpp.

#include "deck-store.hpp"
#include "pack-zip.hpp"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace PackCore {

constexpr int kFormatVersion = 1;

// Setting names that are never exported (see README "What is never in a pack").
bool isSecretKey(const QString &key);
// Removes secrets from a saved source (settings, filters, private settings),
// including secret-looking query parameters of URLs. Returns what was removed,
// as "Source: path".
QStringList scrubSecrets(QJsonObject &source);

// A place inside a saved source that holds a file path, as a JSON-pointer-like
// path, e.g. "settings/local_file" or "filters/0/settings/image_path".
struct MediaRef {
	QString source;
	QString pointer;
};

struct Media {
	QString path; // absolute path on this computer
	qint64 size = 0;
	QVector<MediaRef> refs;
};

struct Source {
	QString name;
	QString id;
	bool isScene = false;
	bool isGroup = false;
	QJsonObject data;   // obs_save_source(), without uuids and hotkeys
	QStringList deps;   // scenes, groups and sources it shows (scenes/groups)
	QVector<int> media; // indexes into Inventory::media / Pack::media
};

// Everything the given scenes (in that order) use, recursively.
struct Inventory {
	QVector<Source> scenes;  // every scene of the collection
	QVector<Source> sources; // sources and groups used by any scene
	QVector<Media> media;    // files used by those scenes/sources
	const Source *find(const QString &name) const;
};
Inventory scan(const QStringList &sceneNames);

// Names of the given scenes/sources plus everything they show, recursively.
QSet<QString> closure(const QVector<Source> &scenes, const QVector<Source> &sources, const QSet<QString> &roots);

struct ExportResult {
	QByteArray manifest;
	QVector<PackZip::Entry> entries; // manifest.json + media files
	QStringList stripped;            // secrets removed
	QStringList unpointed;           // "Source: file" left empty
	QStringList urls;                // browser/URL settings that were kept
	qint64 mediaBytes = 0;
};
ExportResult buildExport(const Inventory &inv, const QVector<Deck> &decks, const QSet<QString> &names,
			 const QSet<int> &media, const QString &collection, const QString &generator);

struct Pack {
	QString collection; // scene collection the pack was made from
	QVector<Deck> decks;
	QVector<Source> scenes;
	QVector<Source> sources;
	struct File {
		QString entry;    // path inside the pack
		QString fileName; // original file name
		qint64 size = 0;
		QVector<MediaRef> refs;
	};
	QVector<File> media;
	QVector<MediaRef> unpointed;            // left out when the pack was made
	QHash<QString, QString> unpointedNames; // pointer key -> original file name
	const Source *find(const QString &name) const;
};
bool readPack(const PackZip::Reader &reader, Pack *pack, QString *error);

enum class Action { Create, Rename, Skip, Replace };

struct ImportPlan {
	QHash<QString, Action> items;       // scene/source name -> action; missing = not imported
	QHash<int, QString> extractedMedia; // Pack::media index -> file on disk
};

struct ImportResult {
	QStringList created;
	QHash<QString, QString> renamed; // pack name -> new name
	QStringList replaced;
	QStringList unpointed; // "Source: file name"
};

bool sourceExists(const QString &name);
// "Name (2)", "Name (3)"... not used by any source and not in `taken`.
QString uniqueSourceName(const QString &base, const QSet<QString> &taken);
// Creates the planned scenes/sources in the current scene collection.
ImportResult importSources(const Pack &pack, const ImportPlan &plan);

// Marker for a removed source's temporary name during Replace.
constexpr const char *kReplacedSuffix = " (replaced by Dock Deck)";

} // namespace PackCore
