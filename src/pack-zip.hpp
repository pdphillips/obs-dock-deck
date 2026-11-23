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

// Minimal ZIP reader/writer for Deck packs: "stored" entries only (no
// compression; media is already compressed), no ZIP64, so a pack must stay
// below 4 GB. Files are valid ZIPs that any unzip tool can open.

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace PackZip {

// Reports bytes processed so far; return false to cancel.
using Progress = std::function<bool(qint64 done)>;

struct Entry {
	QString name;       // path inside the archive, '/' separated
	QByteArray data;    // used when sourceFile is empty
	QString sourceFile; // copied from disk (big media) when set
};

constexpr qint64 kMaxArchiveBytes = 0xFFFFFFFFll - (64ll << 20); // keep headroom for headers

// Writes atomically (temp file + rename). Runs on any thread.
bool write(const QString &path, const QVector<Entry> &entries, const Progress &progress, QString *error);

class Reader {
public:
	bool open(const QString &path, QString *error);
	bool contains(const QString &name) const { return items_.contains(name); }
	qint64 size(const QString &name) const { return items_.value(name).size; }
	QStringList names() const { return items_.keys(); }

	// Reads a whole entry into memory (refuses entries above maxBytes).
	bool read(const QString &name, qint64 maxBytes, QByteArray *out, QString *error) const;
	// Streams an entry to a file, checking its CRC. Runs on any thread.
	bool extract(const QString &name, const QString &dest, const Progress &progress, QString *error) const;

private:
	struct Item {
		qint64 headerOffset = 0;
		qint64 size = 0;
		quint32 crc = 0;
	};
	bool dataOffset(const Item &item, qint64 *offset, QString *error) const;

	QString path_;
	qint64 fileSize_ = 0;
	QHash<QString, Item> items_;
};

} // namespace PackZip
