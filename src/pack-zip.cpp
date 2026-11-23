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

#include "pack-zip.hpp"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>

#include <array>

namespace {

const std::array<quint32, 256> &crcTable()
{
	static const std::array<quint32, 256> table = [] {
		std::array<quint32, 256> t{};
		for (quint32 i = 0; i < 256; ++i) {
			quint32 c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			t[i] = c;
		}
		return t;
	}();
	return table;
}

quint32 crcUpdate(quint32 crc, const char *data, qint64 len)
{
	const auto &t = crcTable();
	crc = ~crc;
	for (qint64 i = 0; i < len; ++i)
		crc = t[(crc ^ static_cast<quint8>(data[i])) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

void put16(QByteArray &b, quint16 v)
{
	char x[2];
	qToLittleEndian(v, x);
	b.append(x, 2);
}

void put32(QByteArray &b, quint32 v)
{
	char x[4];
	qToLittleEndian(v, x);
	b.append(x, 4);
}

quint16 get16(const char *p)
{
	return qFromLittleEndian<quint16>(p);
}

quint32 get32(const char *p)
{
	return qFromLittleEndian<quint32>(p);
}

constexpr quint16 kFlagUtf8 = 0x0800;
constexpr quint16 kDosTime = 0;      // 00:00:00
constexpr quint16 kDosDate = 0x5221; // 2021-01-01; times don't matter here
constexpr qint64 kChunk = 1 << 20;

QByteArray localHeader(const QByteArray &name, quint32 crc, quint32 size)
{
	QByteArray h;
	put32(h, 0x04034b50);
	put16(h, 20);
	put16(h, kFlagUtf8);
	put16(h, 0); // stored
	put16(h, kDosTime);
	put16(h, kDosDate);
	put32(h, crc);
	put32(h, size);
	put32(h, size);
	put16(h, static_cast<quint16>(name.size()));
	put16(h, 0);
	h.append(name);
	return h;
}

} // namespace

bool PackZip::write(const QString &path, const QVector<Entry> &entries, const Progress &progress, QString *error)
{
	QSaveFile out(path);
	if (!out.open(QIODevice::WriteOnly)) {
		*error = out.errorString();
		return false;
	}

	struct Written {
		QByteArray name;
		quint32 crc;
		quint32 size;
		quint32 offset;
	};
	QVector<Written> written;
	qint64 done = 0;

	for (const Entry &e : entries) {
		const QByteArray name = e.name.toUtf8();
		if (name.isEmpty() || name.size() > 0xFFFF) {
			*error = QStringLiteral("bad entry name");
			out.cancelWriting();
			return false;
		}
		const qint64 offset = out.pos();
		QFile in;
		qint64 size = e.data.size();
		if (!e.sourceFile.isEmpty()) {
			in.setFileName(e.sourceFile);
			if (!in.open(QIODevice::ReadOnly)) {
				*error = QStringLiteral("%1: %2").arg(e.sourceFile, in.errorString());
				out.cancelWriting();
				return false;
			}
			size = in.size();
		}
		if (offset + size + 1024 > kMaxArchiveBytes) {
			*error = QStringLiteral("the pack would be larger than 4 GB");
			out.cancelWriting();
			return false;
		}

		// Header with a placeholder CRC, patched after the data is written.
		out.write(localHeader(name, 0, static_cast<quint32>(size)));
		quint32 crc = 0;
		if (e.sourceFile.isEmpty()) {
			crc = crcUpdate(0, e.data.constData(), e.data.size());
			out.write(e.data);
			done += e.data.size();
		} else {
			QByteArray buf;
			qint64 left = size;
			while (left > 0) {
				buf = in.read(qMin(left, kChunk));
				if (buf.isEmpty()) {
					*error = QStringLiteral("%1: read error").arg(e.sourceFile);
					out.cancelWriting();
					return false;
				}
				crc = crcUpdate(crc, buf.constData(), buf.size());
				if (out.write(buf) != buf.size()) {
					*error = out.errorString();
					out.cancelWriting();
					return false;
				}
				left -= buf.size();
				done += buf.size();
				if (progress && !progress(done)) {
					*error = QStringLiteral("cancelled");
					out.cancelWriting();
					return false;
				}
			}
		}
		const qint64 end = out.pos();
		char crcBytes[4];
		qToLittleEndian(crc, crcBytes);
		out.seek(offset + 14);
		out.write(crcBytes, 4);
		out.seek(end);
		written.append({name, crc, static_cast<quint32>(size), static_cast<quint32>(offset)});
		if (progress && !progress(done)) {
			*error = QStringLiteral("cancelled");
			out.cancelWriting();
			return false;
		}
	}

	const qint64 cdOffset = out.pos();
	QByteArray cd;
	for (const Written &w : written) {
		put32(cd, 0x02014b50);
		put16(cd, 20);
		put16(cd, 20);
		put16(cd, kFlagUtf8);
		put16(cd, 0);
		put16(cd, kDosTime);
		put16(cd, kDosDate);
		put32(cd, w.crc);
		put32(cd, w.size);
		put32(cd, w.size);
		put16(cd, static_cast<quint16>(w.name.size()));
		put16(cd, 0);
		put16(cd, 0);
		put16(cd, 0);
		put16(cd, 0);
		put32(cd, 0);
		put32(cd, w.offset);
		cd.append(w.name);
	}
	QByteArray eocd;
	put32(eocd, 0x06054b50);
	put16(eocd, 0);
	put16(eocd, 0);
	put16(eocd, static_cast<quint16>(written.size()));
	put16(eocd, static_cast<quint16>(written.size()));
	put32(eocd, static_cast<quint32>(cd.size()));
	put32(eocd, static_cast<quint32>(cdOffset));
	put16(eocd, 0);
	out.write(cd);
	out.write(eocd);
	if (!out.commit()) {
		*error = out.errorString();
		return false;
	}
	return true;
}

bool PackZip::Reader::open(const QString &path, QString *error)
{
	items_.clear();
	path_ = path;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		*error = f.errorString();
		return false;
	}
	fileSize_ = f.size();
	if (fileSize_ < 22) {
		*error = QStringLiteral("not a Deck pack");
		return false;
	}

	// End of central directory: within the last 64 KiB + 22 bytes.
	const qint64 tailSize = qMin<qint64>(fileSize_, 0xFFFF + 22);
	f.seek(fileSize_ - tailSize);
	const QByteArray tail = f.read(tailSize);
	qint64 eocd = -1;
	for (qint64 i = tail.size() - 22; i >= 0; --i) {
		if (get32(tail.constData() + i) == 0x06054b50) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		*error = QStringLiteral("not a Deck pack");
		return false;
	}
	const char *e = tail.constData() + eocd;
	const quint16 count = get16(e + 10);
	const quint32 cdSize = get32(e + 12);
	const quint32 cdOffset = get32(e + 16);
	if (static_cast<qint64>(cdOffset) + cdSize > fileSize_ || cdSize > (64u << 20)) {
		*error = QStringLiteral("damaged pack");
		return false;
	}
	f.seek(cdOffset);
	const QByteArray cd = f.read(cdSize);
	qint64 p = 0;
	for (int i = 0; i < count; ++i) {
		if (p + 46 > cd.size() || get32(cd.constData() + p) != 0x02014b50) {
			*error = QStringLiteral("damaged pack");
			return false;
		}
		const char *h = cd.constData() + p;
		const quint16 method = get16(h + 10);
		Item item;
		item.crc = get32(h + 16);
		const quint32 csize = get32(h + 20);
		item.size = get32(h + 24);
		const quint16 nameLen = get16(h + 28);
		const quint16 extraLen = get16(h + 30);
		const quint16 commentLen = get16(h + 32);
		item.headerOffset = get32(h + 42);
		if (p + 46 + nameLen > cd.size()) {
			*error = QStringLiteral("damaged pack");
			return false;
		}
		const QString name = QString::fromUtf8(h + 46, nameLen);
		p += 46 + nameLen + extraLen + commentLen;
		// Only our own format: stored entries that fit in the file.
		if (method != 0 || csize != item.size || item.headerOffset + 30 + item.size > fileSize_)
			continue;
		items_.insert(name, item);
	}
	return true;
}

bool PackZip::Reader::dataOffset(const Item &item, qint64 *offset, QString *error) const
{
	QFile f(path_);
	if (!f.open(QIODevice::ReadOnly) || !f.seek(item.headerOffset)) {
		*error = f.errorString();
		return false;
	}
	const QByteArray h = f.read(30);
	if (h.size() != 30 || get32(h.constData()) != 0x04034b50) {
		*error = QStringLiteral("damaged pack");
		return false;
	}
	*offset = item.headerOffset + 30 + get16(h.constData() + 26) + get16(h.constData() + 28);
	if (*offset + item.size > fileSize_) {
		*error = QStringLiteral("damaged pack");
		return false;
	}
	return true;
}

bool PackZip::Reader::read(const QString &name, qint64 maxBytes, QByteArray *out, QString *error) const
{
	const auto it = items_.constFind(name);
	if (it == items_.constEnd()) {
		*error = QStringLiteral("%1 missing from pack").arg(name);
		return false;
	}
	if (it->size > maxBytes) {
		*error = QStringLiteral("%1 is too large").arg(name);
		return false;
	}
	qint64 offset = 0;
	if (!dataOffset(*it, &offset, error))
		return false;
	QFile f(path_);
	if (!f.open(QIODevice::ReadOnly) || !f.seek(offset)) {
		*error = f.errorString();
		return false;
	}
	*out = f.read(it->size);
	if (out->size() != it->size || crcUpdate(0, out->constData(), out->size()) != it->crc) {
		*error = QStringLiteral("%1 is damaged").arg(name);
		return false;
	}
	return true;
}

bool PackZip::Reader::extract(const QString &name, const QString &dest, const Progress &progress, QString *error) const
{
	const auto it = items_.constFind(name);
	if (it == items_.constEnd()) {
		*error = QStringLiteral("%1 missing from pack").arg(name);
		return false;
	}
	qint64 offset = 0;
	if (!dataOffset(*it, &offset, error))
		return false;
	QFile in(path_);
	if (!in.open(QIODevice::ReadOnly) || !in.seek(offset)) {
		*error = in.errorString();
		return false;
	}
	QSaveFile out(dest);
	if (!out.open(QIODevice::WriteOnly)) {
		*error = QStringLiteral("%1: %2").arg(dest, out.errorString());
		return false;
	}
	quint32 crc = 0;
	qint64 left = it->size, done = 0;
	while (left > 0) {
		const QByteArray buf = in.read(qMin(left, kChunk));
		if (buf.isEmpty()) {
			*error = QStringLiteral("%1 is damaged").arg(name);
			out.cancelWriting();
			return false;
		}
		crc = crcUpdate(crc, buf.constData(), buf.size());
		out.write(buf);
		left -= buf.size();
		done += buf.size();
		if (progress && !progress(done)) {
			*error = QStringLiteral("cancelled");
			out.cancelWriting();
			return false;
		}
	}
	if (crc != it->crc) {
		*error = QStringLiteral("%1 is damaged").arg(name);
		out.cancelWriting();
		return false;
	}
	if (!out.commit()) {
		*error = out.errorString();
		return false;
	}
	return true;
}
