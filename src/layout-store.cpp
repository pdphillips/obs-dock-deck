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

#include "layout-store.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

// Bump if the file format changes incompatibly.
static constexpr int kFormatVersion = 1;

LayoutStore::LoadResult LayoutStore::load(const QString &path)
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
	layouts.clear();
	active = root.value(QStringLiteral("active")).toString();
	showTabStrip = root.value(QStringLiteral("showTabStrip")).toBool(true);
	autoSave = root.value(QStringLiteral("autoSave")).toBool(true);

	const QJsonArray arr = root.value(QStringLiteral("layouts")).toArray();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		DockLayout layout;
		layout.name = o.value(QStringLiteral("name")).toString().trimmed();
		if (layout.name.isEmpty() || indexOf(layout.name) >= 0)
			continue; // skip unnamed or duplicate entries
		layout.state = QByteArray::fromBase64(o.value(QStringLiteral("state")).toString().toLatin1());

		const QJsonObject docks = o.value(QStringLiteral("docks")).toObject();
		for (auto it = docks.begin(); it != docks.end(); ++it)
			layout.docks.insert(it.key(), it.value().toBool());

		layouts.append(layout);
	}
	return LoadResult::Ok;
}

bool LayoutStore::save(const QString &path) const
{
	QJsonArray arr;
	for (const DockLayout &layout : layouts) {
		QJsonObject docks;
		for (auto it = layout.docks.constBegin(); it != layout.docks.constEnd(); ++it)
			docks.insert(it.key(), it.value());

		QJsonObject o;
		o.insert(QStringLiteral("name"), layout.name);
		o.insert(QStringLiteral("state"), QString::fromLatin1(layout.state.toBase64()));
		o.insert(QStringLiteral("docks"), docks);
		arr.append(o);
	}

	QJsonObject root;
	root.insert(QStringLiteral("version"), kFormatVersion);
	root.insert(QStringLiteral("active"), active);
	root.insert(QStringLiteral("showTabStrip"), showTabStrip);
	root.insert(QStringLiteral("autoSave"), autoSave);
	root.insert(QStringLiteral("layouts"), arr);

	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return file.commit();
}

qsizetype LayoutStore::indexOf(const QString &name) const
{
	for (qsizetype i = 0; i < layouts.size(); ++i) {
		if (layouts[i].name == name)
			return i;
	}
	return -1;
}

DockLayout *LayoutStore::find(const QString &name)
{
	const qsizetype i = indexOf(name);
	return i < 0 ? nullptr : &layouts[i];
}

bool LayoutStore::nameTaken(const QString &name, const QString &ignore) const
{
	for (const DockLayout &layout : layouts) {
		if (layout.name == ignore)
			continue;
		if (layout.name.compare(name, Qt::CaseInsensitive) == 0)
			return true;
	}
	return false;
}

QStringList LayoutStore::names() const
{
	QStringList out;
	for (const DockLayout &layout : layouts)
		out.append(layout.name);
	return out;
}
