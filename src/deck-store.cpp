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

#include "deck-store.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

// Bump if the file format changes incompatibly.
static constexpr int kFormatVersion = 1;

// Export files carry this marker so random JSON is rejected early.
static constexpr const char *kExportFormat = "dock-deck-layouts";

// Hard limits that keep a hostile or broken file from doing anything silly.
static constexpr qint64 kMaxFileBytes = 16 * 1024 * 1024;
static constexpr int kMaxNameLength = 100;
static constexpr int kMaxTitleLength = 200;
static constexpr int kMaxDecks = 500;

// QMainWindow::saveState() data starts with a 32-bit 0xff marker.
static bool looksLikeMainWindowState(const QByteArray &state)
{
	return state.size() >= 8 && static_cast<unsigned char>(state[0]) == 0 &&
	       static_cast<unsigned char>(state[1]) == 0 && static_cast<unsigned char>(state[2]) == 0 &&
	       static_cast<unsigned char>(state[3]) == 0xff;
}

static QJsonObject deckToJson(const Deck &deck)
{
	QJsonObject docks;
	for (auto it = deck.docks.constBegin(); it != deck.docks.constEnd(); ++it)
		docks.insert(it.key(), it.value());

	QJsonObject o;
	if (!deck.id.isEmpty())
		o.insert(QStringLiteral("id"), deck.id);
	o.insert(QStringLiteral("name"), deck.name);
	o.insert(QStringLiteral("state"), QString::fromLatin1(deck.state.toBase64()));
	o.insert(QStringLiteral("docks"), docks);

	// Titles are an optional extra key, so older Dock Deck versions (which
	// ignore unknown keys) can still read files written by this one.
	if (!deck.titles.isEmpty()) {
		QJsonObject titles;
		for (auto it = deck.titles.constBegin(); it != deck.titles.constEnd(); ++it)
			titles.insert(it.key(), it.value());
		o.insert(QStringLiteral("titles"), titles);
	}
	if (deck.locked)
		o.insert(QStringLiteral("locked"), true);
	if (!deck.scenes.isEmpty()) {
		QJsonObject scenes;
		for (auto it = deck.scenes.constBegin(); it != deck.scenes.constEnd(); ++it)
			scenes.insert(it.key(), it.value());
		o.insert(QStringLiteral("scenes"), scenes);
	}
	return o;
}

// Scene links are optional: bad entries are dropped, never fatal.
static QHash<QString, QString> scenesFromJson(const QJsonValue &value)
{
	QHash<QString, QString> scenes;
	const QJsonObject map = value.toObject();
	for (auto it = map.begin(); it != map.end(); ++it) {
		const QString scene = it.value().toString();
		if (!it.key().isEmpty() && !scene.isEmpty() && it.key().size() <= 256 && scene.size() <= 256)
			scenes.insert(it.key(), scene);
	}
	return scenes;
}

// Titles are cosmetic: bad entries are dropped instead of rejecting the deck.
static QHash<QString, QString> titlesFromJson(const QJsonValue &value)
{
	QHash<QString, QString> titles;
	const QJsonObject o = value.toObject();
	for (auto it = o.begin(); it != o.end(); ++it) {
		const QString title = it.value().toString().simplified();
		if (!it.key().isEmpty() && !title.isEmpty() && title.size() <= kMaxTitleLength)
			titles.insert(it.key(), title);
	}
	return titles;
}

// Strict parse of one deck entry; returns false if anything is off.
static bool deckFromJson(const QJsonValue &value, Deck *out)
{
	if (!value.isObject())
		return false;
	const QJsonObject o = value.toObject();

	const QJsonValue name = o.value(QStringLiteral("name"));
	const QJsonValue state = o.value(QStringLiteral("state"));
	const QJsonValue docks = o.value(QStringLiteral("docks"));
	if (!name.isString() || !state.isString() || !(docks.isObject() || docks.isUndefined()))
		return false;

	Deck deck;
	deck.name = name.toString().simplified();
	if (deck.name.isEmpty() || deck.name.size() > kMaxNameLength)
		return false;
	// Optional; a bad id is dropped (a new one is assigned on import).
	const QString id = o.value(QStringLiteral("id")).toString().trimmed();
	if (id.size() <= 64)
		deck.id = id;

	const auto decoded =
		QByteArray::fromBase64Encoding(state.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
	if (!decoded || !looksLikeMainWindowState(*decoded))
		return false;
	deck.state = *decoded;

	const QJsonObject dockMap = docks.toObject();
	for (auto it = dockMap.begin(); it != dockMap.end(); ++it) {
		if (it.key().isEmpty() || !it.value().isBool())
			return false;
		deck.docks.insert(it.key(), it.value().toBool());
	}
	deck.titles = titlesFromJson(o.value(QStringLiteral("titles")));
	deck.locked = o.value(QStringLiteral("locked")).toBool(false); // non-bool: unlocked
	deck.scenes = scenesFromJson(o.value(QStringLiteral("scenes")));

	*out = deck;
	return true;
}

bool DeckFile::write(const QString &path, const QVector<Deck> &decks, QString *error)
{
	QJsonArray arr;
	for (const Deck &deck : decks)
		arr.append(deckToJson(deck));

	QJsonObject root;
	root.insert(QStringLiteral("format"), QString::fromLatin1(kExportFormat));
	root.insert(QStringLiteral("version"), kFormatVersion);
	root.insert(QStringLiteral("layouts"), arr);

	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 ||
	    !file.commit()) {
		if (error)
			*error = file.errorString();
		return false;
	}
	return true;
}

DeckFile::ReadError DeckFile::read(const QString &path, ReadResult *result)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return ReadError::CannotOpen;
	if (file.size() > kMaxFileBytes)
		return ReadError::TooLarge;

	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(file.read(kMaxFileBytes), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return ReadError::NotJson;

	// Accept export files and also Dock Deck's own layouts.json (no marker).
	const QJsonObject root = doc.object();
	const QJsonValue format = root.value(QStringLiteral("format"));
	if (!format.isUndefined() && format.toString() != QString::fromLatin1(kExportFormat))
		return ReadError::WrongFormat;
	if (root.value(QStringLiteral("version")).toInt(kFormatVersion) > kFormatVersion)
		return ReadError::TooNew;

	const QJsonValue decks = root.value(QStringLiteral("layouts"));
	if (!decks.isArray())
		return ReadError::NoDecks;

	ReadResult out;
	for (const QJsonValue &value : decks.toArray()) {
		Deck deck;
		if (out.decks.size() < kMaxDecks && deckFromJson(value, &deck))
			out.decks.append(deck);
		else
			++out.invalidEntries;
	}

	if (out.decks.isEmpty())
		return ReadError::NoDecks;
	*result = out;
	return ReadError::None;
}

DeckStore::LoadResult DeckStore::load(const QString &path)
{
	QFile file(path);
	if (!file.exists())
		return LoadResult::Missing;

	QJsonParseError err{};
	QJsonDocument doc;
	if (file.open(QIODevice::ReadOnly)) {
		doc = QJsonDocument::fromJson(file.readAll(), &err);
		file.close();
	} else {
		err.error = QJsonParseError::UnterminatedObject;
	}

	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		// Keep the broken file for inspection instead of overwriting it.
		const QString bad = path + QStringLiteral(".bad");
		QFile::remove(bad);
		QFile::rename(path, bad);
		return LoadResult::Corrupt;
	}

	const QJsonObject root = doc.object();
	decks.clear();
	active = root.value(QStringLiteral("active")).toString();
	showTabStrip = root.value(QStringLiteral("showTabStrip")).toBool(true);
	autoSave = root.value(QStringLiteral("autoSave")).toBool(true);
	showPlaceholders = root.value(QStringLiteral("showPlaceholders")).toBool(true);

	const QJsonArray arr = root.value(QStringLiteral("layouts")).toArray();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		Deck deck;
		deck.name = o.value(QStringLiteral("name")).toString().trimmed();
		if (deck.name.isEmpty() || indexOf(deck.name) >= 0)
			continue; // skip unnamed or duplicate entries
		deck.state = QByteArray::fromBase64(o.value(QStringLiteral("state")).toString().toLatin1());

		const QJsonObject docks = o.value(QStringLiteral("docks")).toObject();
		for (auto it = docks.begin(); it != docks.end(); ++it)
			deck.docks.insert(it.key(), it.value().toBool());
		deck.titles = titlesFromJson(o.value(QStringLiteral("titles")));
		deck.locked = o.value(QStringLiteral("locked")).toBool(false);
		deck.scenes = scenesFromJson(o.value(QStringLiteral("scenes")));
		const QString id = o.value(QStringLiteral("id")).toString().trimmed();
		if (id.size() <= 64)
			deck.id = id;

		decks.append(deck);
	}
	ensureIds();
	return LoadResult::Ok;
}

QString DeckStore::newId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

Deck *DeckStore::findById(const QString &id)
{
	if (id.isEmpty())
		return nullptr;
	for (Deck &deck : decks) {
		if (deck.id == id)
			return &deck;
	}
	return nullptr;
}

bool DeckStore::idTaken(const QString &id) const
{
	for (const Deck &deck : decks) {
		if (deck.id == id)
			return true;
	}
	return false;
}

bool DeckStore::ensureIds()
{
	bool changed = false;
	QSet<QString> seen;
	for (Deck &deck : decks) {
		if (deck.id.isEmpty() || seen.contains(deck.id)) {
			deck.id = newId();
			changed = true;
		}
		seen.insert(deck.id);
	}
	return changed;
}

QJsonObject DeckFile::toJson(const Deck &deck)
{
	return deckToJson(deck);
}

bool DeckFile::fromJson(const QJsonValue &value, Deck *out)
{
	return deckFromJson(value, out);
}

bool DeckStore::save(const QString &path) const
{
	QJsonArray arr;
	for (const Deck &deck : decks)
		arr.append(deckToJson(deck));

	QJsonObject root;
	root.insert(QStringLiteral("version"), kFormatVersion);
	root.insert(QStringLiteral("active"), active);
	root.insert(QStringLiteral("showTabStrip"), showTabStrip);
	root.insert(QStringLiteral("autoSave"), autoSave);
	root.insert(QStringLiteral("showPlaceholders"), showPlaceholders);
	root.insert(QStringLiteral("layouts"), arr);

	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return file.commit();
}

qsizetype DeckStore::indexOf(const QString &name) const
{
	for (qsizetype i = 0; i < decks.size(); ++i) {
		if (decks[i].name == name)
			return i;
	}
	return -1;
}

Deck *DeckStore::find(const QString &name)
{
	const qsizetype i = indexOf(name);
	return i < 0 ? nullptr : &decks[i];
}

bool DeckStore::nameTaken(const QString &name, const QString &ignore) const
{
	for (const Deck &deck : decks) {
		if (deck.name == ignore)
			continue;
		if (deck.name.compare(name, Qt::CaseInsensitive) == 0)
			return true;
	}
	return false;
}

QStringList DeckStore::names() const
{
	QStringList out;
	for (const Deck &deck : decks)
		out.append(deck.name);
	return out;
}
