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

#include "pack-core.hpp"

#include <obs.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace {

// --- JSON pointers ("settings/files/0/value"; '~' and '/' escaped) --------

QString escapeSegment(QString s)
{
	s.replace(QLatin1Char('~'), QStringLiteral("~0"));
	s.replace(QLatin1Char('/'), QStringLiteral("~1"));
	return s;
}

QStringList splitPointer(const QString &pointer)
{
	QStringList segs = pointer.split(QLatin1Char('/'));
	for (QString &s : segs) {
		s.replace(QStringLiteral("~1"), QStringLiteral("/"));
		s.replace(QStringLiteral("~0"), QStringLiteral("~"));
	}
	return segs;
}

QJsonValue setAt(const QJsonValue &node, const QStringList &segs, int i, const QJsonValue &value)
{
	if (i >= segs.size())
		return value;
	if (node.isObject()) {
		QJsonObject o = node.toObject();
		if (!o.contains(segs[i]))
			return node; // path no longer there: leave unchanged
		o.insert(segs[i], setAt(o.value(segs[i]), segs, i + 1, value));
		return o;
	}
	if (node.isArray()) {
		QJsonArray a = node.toArray();
		bool ok = false;
		const int idx = segs[i].toInt(&ok);
		if (!ok || idx < 0 || idx >= a.size())
			return node;
		a[idx] = setAt(a.at(idx), segs, i + 1, value);
		return a;
	}
	return node;
}

QJsonObject setPointer(const QJsonObject &root, const QString &pointer, const QJsonValue &value)
{
	return setAt(root, splitPointer(pointer), 0, value).toObject();
}

// Calls fn(pointer, string) for every string inside `node`.
template<typename Fn> void walkStrings(const QJsonValue &node, const QString &pointer, Fn &&fn)
{
	if (node.isString()) {
		fn(pointer, node.toString());
	} else if (node.isObject()) {
		const QJsonObject o = node.toObject();
		for (auto it = o.begin(); it != o.end(); ++it)
			walkStrings(it.value(), pointer + QLatin1Char('/') + escapeSegment(it.key()), fn);
	} else if (node.isArray()) {
		const QJsonArray a = node.toArray();
		for (int i = 0; i < a.size(); ++i)
			walkStrings(a.at(i), pointer + QLatin1Char('/') + QString::number(i), fn);
	}
}

// Media: settings (and filter settings) that point at an existing file.
template<typename Fn> void walkFilePaths(const QJsonObject &source, Fn &&fn)
{
	auto check = [&](const QString &pointer, const QString &value) {
		if (value.size() < 2 || value.size() > 4096 ||
		    !(value.contains(QLatin1Char('/')) || value.contains(QLatin1Char('\\'))))
			return;
		const QFileInfo fi(value);
		if (fi.isAbsolute() && fi.isFile())
			fn(pointer, fi.absoluteFilePath(), fi.size());
	};
	walkStrings(source.value(QStringLiteral("settings")), QStringLiteral("settings"), check);
	walkStrings(source.value(QStringLiteral("filters")), QStringLiteral("filters"), check);
}

QStringList keyTokens(const QString &key)
{
	QString k = key;
	k.replace(QRegularExpression(QStringLiteral("([a-z0-9])([A-Z])")), QStringLiteral("\\1_\\2"));
	return k.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts);
}

// http(s), ws(s), rtmp(s), srt... anything "scheme://".
bool isUrl(const QString &s)
{
	static const QRegularExpression re(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]{1,15}://\\S+$"));
	return s.size() < 8192 && re.match(s).hasMatch() &&
	       !s.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive);
}

QJsonValue scrubValue(const QJsonValue &node, const QString &path, const QString &source, QStringList *removed)
{
	if (node.isObject()) {
		QJsonObject o = node.toObject();
		for (const QString &key : o.keys()) {
			const QString here = path.isEmpty() ? key : path + QLatin1Char('/') + key;
			if (PackCore::isSecretKey(key)) {
				o.remove(key);
				removed->append(QStringLiteral("%1: %2").arg(source, here));
			} else {
				o.insert(key, scrubValue(o.value(key), here, source, removed));
			}
		}
		return o;
	}
	if (node.isArray()) {
		QJsonArray a = node.toArray();
		for (int i = 0; i < a.size(); ++i)
			a[i] = scrubValue(a.at(i), path + QLatin1Char('/') + QString::number(i), source, removed);
		return a;
	}
	if (node.isString() && isUrl(node.toString())) {
		QUrl url(node.toString());
		QUrlQuery query(url);
		bool changed = false;
		for (const auto &item : query.queryItems()) {
			if (PackCore::isSecretKey(item.first)) {
				query.removeAllQueryItems(item.first);
				removed->append(
					QStringLiteral("%1: %2 (URL parameter \"%3\")").arg(source, path, item.first));
				changed = true;
			}
		}
		// user:password@host
		if (!url.password().isEmpty() || !url.userName().isEmpty()) {
			url.setUserInfo(QString());
			removed->append(QStringLiteral("%1: %2 (URL login)").arg(source, path));
			changed = true;
		}
		if (changed) {
			url.setQuery(query);
			return url.toString();
		}
	}
	return node;
}

QJsonObject cleanSaved(obs_source_t *src)
{
	obs_data_t *d = obs_save_source(src);
	QJsonObject o = QJsonDocument::fromJson(QByteArray(obs_data_get_json(d))).object();
	obs_data_release(d);
	// Machine-specific or useless elsewhere: ids, hotkey bindings, canvas.
	o.remove(QStringLiteral("uuid"));
	o.remove(QStringLiteral("hotkeys"));
	o.remove(QStringLiteral("canvas_uuid"));
	QJsonArray filters = o.value(QStringLiteral("filters")).toArray();
	for (int i = 0; i < filters.size(); ++i) {
		QJsonObject filter = filters.at(i).toObject();
		filter.remove(QStringLiteral("uuid"));
		filter.remove(QStringLiteral("hotkeys"));
		filters[i] = filter;
	}
	if (!filters.isEmpty())
		o.insert(QStringLiteral("filters"), filters);
	QJsonObject settings = o.value(QStringLiteral("settings")).toObject();
	if (settings.contains(QStringLiteral("items"))) {
		QJsonArray items = settings.value(QStringLiteral("items")).toArray();
		for (int i = 0; i < items.size(); ++i) {
			QJsonObject item = items.at(i).toObject();
			item.remove(QStringLiteral("source_uuid"));
			items[i] = item;
		}
		settings.insert(QStringLiteral("items"), items);
		o.insert(QStringLiteral("settings"), settings);
	}
	return o;
}

QStringList itemNames(obs_scene_t *scene)
{
	QStringList names;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_source_t *s = obs_sceneitem_get_source(item);
			const QString n = QString::fromUtf8(obs_source_get_name(s));
			auto *list = static_cast<QStringList *>(param);
			if (!n.isEmpty() && !list->contains(n))
				list->append(n);
			return true;
		},
		&names);
	return names;
}

QJsonArray toArray(const QStringList &list)
{
	QJsonArray a;
	for (const QString &s : list)
		a.append(s);
	return a;
}

QJsonArray refsToJson(const QVector<PackCore::MediaRef> &refs)
{
	QJsonArray a;
	for (const auto &r : refs)
		a.append(QJsonObject{{QStringLiteral("source"), r.source}, {QStringLiteral("pointer"), r.pointer}});
	return a;
}

QVector<PackCore::MediaRef> refsFromJson(const QJsonValue &v)
{
	QVector<PackCore::MediaRef> refs;
	for (const QJsonValue &r : v.toArray()) {
		const QJsonObject o = r.toObject();
		const QString source = o.value(QStringLiteral("source")).toString();
		const QString pointer = o.value(QStringLiteral("pointer")).toString();
		if (!source.isEmpty() &&
		    (pointer.startsWith(QStringLiteral("settings")) || pointer.startsWith(QStringLiteral("filters"))))
			refs.append({source, pointer});
	}
	return refs;
}

QString safeFileName(const QString &name)
{
	QString n = QFileInfo(name).fileName();
	n.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("_"));
	n = n.trimmed();
	if (n.isEmpty() || n == QStringLiteral(".") || n == QStringLiteral(".."))
		n = QStringLiteral("media");
	return n.left(200);
}

} // namespace

bool PackCore::isSecretKey(const QString &key)
{
	static const QSet<QString> words = {
		QStringLiteral("password"),   QStringLiteral("passwd"),        QStringLiteral("pwd"),
		QStringLiteral("passphrase"), QStringLiteral("secret"),        QStringLiteral("secrets"),
		QStringLiteral("token"),      QStringLiteral("tokens"),        QStringLiteral("auth"),
		QStringLiteral("oauth"),      QStringLiteral("authorization"), QStringLiteral("cookie"),
		QStringLiteral("cookies"),    QStringLiteral("credential"),    QStringLiteral("credentials"),
		QStringLiteral("session"),    QStringLiteral("sessionid"),     QStringLiteral("apikey"),
		QStringLiteral("streamkey"),  QStringLiteral("accesskey"),     QStringLiteral("privatekey"),
		QStringLiteral("bearer"),     QStringLiteral("jwt"),
	};
	const QStringList tokens = keyTokens(key);
	if (tokens.isEmpty())
		return false;
	for (const QString &t : tokens) {
		if (words.contains(t))
			return true;
	}
	// "key", "stream_key", "api_key"... but not "key_color" (chroma key).
	if (tokens.last() == QStringLiteral("key"))
		return true;
	const QString joined = tokens.join(QString());
	return joined.contains(QStringLiteral("password")) || joined.contains(QStringLiteral("secret")) ||
	       joined.contains(QStringLiteral("apikey")) || joined.contains(QStringLiteral("streamkey"));
}

QStringList PackCore::scrubSecrets(QJsonObject &source)
{
	QStringList removed;
	const QString name = source.value(QStringLiteral("name")).toString();
	for (const QString &key : source.keys()) {
		if (key == QStringLiteral("name") || key == QStringLiteral("id") ||
		    key == QStringLiteral("versioned_id"))
			continue;
		if (isSecretKey(key)) {
			source.remove(key);
			removed.append(QStringLiteral("%1: %2").arg(name, key));
			continue;
		}
		source.insert(key, scrubValue(source.value(key), key, name, &removed));
	}
	return removed;
}

const PackCore::Source *PackCore::Inventory::find(const QString &name) const
{
	for (const Source &s : scenes)
		if (s.name == name)
			return &s;
	for (const Source &s : sources)
		if (s.name == name)
			return &s;
	return nullptr;
}

const PackCore::Source *PackCore::Pack::find(const QString &name) const
{
	for (const Source &s : scenes)
		if (s.name == name)
			return &s;
	for (const Source &s : sources)
		if (s.name == name)
			return &s;
	return nullptr;
}

PackCore::Inventory PackCore::scan(const QStringList &sceneNames)
{
	Inventory inv;
	QHash<QString, int> mediaIndex;

	auto describe = [&](obs_source_t *src, Source *out) {
		out->name = QString::fromUtf8(obs_source_get_name(src));
		out->id = QString::fromUtf8(obs_source_get_id(src));
		out->isScene = obs_source_is_scene(src);
		out->isGroup = obs_source_is_group(src);
		out->data = cleanSaved(src);
		if (obs_scene_t *scene = out->isGroup ? obs_group_from_source(src) : obs_scene_from_source(src))
			out->deps = itemNames(scene);
		walkFilePaths(out->data, [&](const QString &pointer, const QString &path, qint64 size) {
			int idx = mediaIndex.value(path, -1);
			if (idx < 0) {
				idx = static_cast<int>(inv.media.size());
				inv.media.append({path, size, {}});
				mediaIndex.insert(path, idx);
			}
			inv.media[idx].refs.append({out->name, pointer});
			if (!out->media.contains(idx))
				out->media.append(idx);
		});
	};

	QStringList queue;
	QSet<QString> seen;
	for (const QString &name : sceneNames) {
		obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
		if (src && obs_source_is_scene(src) && !seen.contains(name)) {
			Source s;
			describe(src, &s);
			seen.insert(name);
			queue.append(s.deps);
			inv.scenes.append(s);
		}
		obs_source_release(src);
	}
	while (!queue.isEmpty()) {
		const QString name = queue.takeFirst();
		if (seen.contains(name))
			continue;
		seen.insert(name);
		obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
		if (src && !obs_source_is_scene(src)) { // nested scenes are listed already
			Source s;
			describe(src, &s);
			queue.append(s.deps);
			inv.sources.append(s);
		}
		obs_source_release(src);
	}
	return inv;
}

QSet<QString> PackCore::closure(const QVector<Source> &scenes, const QVector<Source> &sources,
				const QSet<QString> &roots)
{
	QHash<QString, QStringList> deps;
	for (const Source &s : scenes)
		deps.insert(s.name, s.deps);
	for (const Source &s : sources)
		deps.insert(s.name, s.deps);
	QSet<QString> out;
	QStringList queue = roots.values();
	while (!queue.isEmpty()) {
		const QString n = queue.takeFirst();
		if (out.contains(n))
			continue;
		out.insert(n);
		queue.append(deps.value(n));
	}
	return out;
}

PackCore::ExportResult PackCore::buildExport(const Inventory &inv, const QVector<Deck> &decks,
					     const QSet<QString> &names, const QSet<int> &media,
					     const QString &collection, const QString &generator,
					     const QJsonArray &organizer)
{
	ExportResult result;
	QJsonArray scenesArr, sourcesArr, mediaArr, unpointedArr;
	QHash<int, QVector<MediaRef>> packed;

	auto add = [&](const Source &src, QJsonArray *into) {
		if (!names.contains(src.name))
			return;
		QJsonObject d = src.data;
		for (const int idx : src.media) {
			for (const MediaRef &ref : inv.media[idx].refs) {
				if (ref.source != src.name)
					continue;
				d = setPointer(d, ref.pointer, QString()); // never ship local paths
				if (media.contains(idx)) {
					packed[idx].append(ref);
				} else {
					const QString file = QFileInfo(inv.media[idx].path).fileName();
					unpointedArr.append(QJsonObject{{QStringLiteral("source"), ref.source},
									{QStringLiteral("pointer"), ref.pointer},
									{QStringLiteral("fileName"), file}});
					result.unpointed.append(QStringLiteral("%1: %2").arg(src.name, file));
				}
			}
		}
		result.stripped += scrubSecrets(d);
		walkStrings(d, QString(), [&](const QString &pointer, const QString &value) {
			if (isUrl(value))
				result.urls.append(
					QStringLiteral("%1: %2 (%3)").arg(src.name, pointer.mid(1), QUrl(value).host()));
		});
		into->append(QJsonObject{{QStringLiteral("name"), src.name},
					 {QStringLiteral("id"), src.id},
					 {QStringLiteral("scene"), src.isScene},
					 {QStringLiteral("group"), src.isGroup},
					 {QStringLiteral("deps"), toArray(src.deps)},
					 {QStringLiteral("data"), d}});
	};
	for (const Source &s : inv.scenes)
		add(s, &scenesArr);
	for (const Source &s : inv.sources)
		add(s, &sourcesArr);

	int n = 0;
	for (int idx = 0; idx < inv.media.size(); ++idx) {
		if (!packed.contains(idx))
			continue;
		const Media &m = inv.media[idx];
		const QString entry = QStringLiteral("media/%1/%2").arg(++n).arg(safeFileName(m.path));
		mediaArr.append(QJsonObject{{QStringLiteral("entry"), entry},
					    {QStringLiteral("fileName"), safeFileName(m.path)},
					    {QStringLiteral("size"), static_cast<double>(m.size)},
					    {QStringLiteral("refs"), refsToJson(packed.value(idx))}});
		result.entries.append({entry, QByteArray(), m.path});
		result.mediaBytes += m.size;
	}

	QJsonArray decksArr;
	for (const Deck &d : decks)
		decksArr.append(DeckFile::toJson(d));

	QJsonObject manifest{
		{QStringLiteral("format"), QStringLiteral("dock-deck-pack")},
		{QStringLiteral("version"), kFormatVersion},
		{QStringLiteral("generator"), generator},
		{QStringLiteral("collection"), collection},
		{QStringLiteral("decks"), decksArr},
		{QStringLiteral("scenes"), scenesArr},
		{QStringLiteral("sources"), sourcesArr},
		{QStringLiteral("media"), mediaArr},
		{QStringLiteral("unpointed"), unpointedArr},
		{QStringLiteral("stripped"), toArray(result.stripped)},
	};
	if (!organizer.isEmpty())
		manifest.insert(QStringLiteral("organizer"), organizer);
	result.manifest = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
	result.entries.prepend({QStringLiteral("manifest.json"), result.manifest, QString()});
	return result;
}

bool PackCore::readPack(const PackZip::Reader &reader, Pack *pack, QString *error)
{
	QByteArray raw;
	if (!reader.read(QStringLiteral("manifest.json"), 64ll << 20, &raw, error))
		return false;
	QJsonParseError perr;
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
	const QJsonObject m = doc.object();
	if (perr.error != QJsonParseError::NoError ||
	    m.value(QStringLiteral("format")).toString() != QStringLiteral("dock-deck-pack")) {
		*error = QStringLiteral("not a Deck pack");
		return false;
	}
	if (m.value(QStringLiteral("version")).toInt(0) > kFormatVersion) {
		*error = QStringLiteral("made by a newer Dock Deck");
		return false;
	}

	*pack = Pack();
	pack->collection = m.value(QStringLiteral("collection")).toString();
	pack->organizer = m.value(QStringLiteral("organizer")).toArray();
	for (const QJsonValue &v : m.value(QStringLiteral("decks")).toArray()) {
		Deck d;
		if (pack->decks.size() < 500 && DeckFile::fromJson(v, &d))
			pack->decks.append(d);
	}
	auto readSources = [&](const QString &key, QVector<Source> *out) {
		QSet<QString> names;
		for (const QJsonValue &v : m.value(key).toArray()) {
			const QJsonObject o = v.toObject();
			Source s;
			s.name = o.value(QStringLiteral("name")).toString();
			s.id = o.value(QStringLiteral("id")).toString();
			s.isScene = o.value(QStringLiteral("scene")).toBool();
			s.isGroup = o.value(QStringLiteral("group")).toBool();
			s.data = o.value(QStringLiteral("data")).toObject();
			for (const QJsonValue &d : o.value(QStringLiteral("deps")).toArray())
				if (d.isString())
					s.deps.append(d.toString());
			if (s.name.isEmpty() || s.id.isEmpty() || s.data.isEmpty() || names.contains(s.name))
				continue;
			s.data.insert(QStringLiteral("name"), s.name);
			names.insert(s.name);
			out->append(s);
		}
	};
	readSources(QStringLiteral("scenes"), &pack->scenes);
	readSources(QStringLiteral("sources"), &pack->sources);
	for (const QJsonValue &v : m.value(QStringLiteral("media")).toArray()) {
		const QJsonObject o = v.toObject();
		Pack::File f;
		f.entry = o.value(QStringLiteral("entry")).toString();
		if (!reader.contains(f.entry))
			continue;
		f.fileName = safeFileName(o.value(QStringLiteral("fileName")).toString());
		f.size = reader.size(f.entry);
		f.refs = refsFromJson(o.value(QStringLiteral("refs")));
		pack->media.append(f);
	}
	for (const QJsonValue &v : m.value(QStringLiteral("unpointed")).toArray()) {
		const QJsonObject o = v.toObject();
		const QVector<MediaRef> r = refsFromJson(QJsonArray{o});
		if (r.isEmpty())
			continue;
		pack->unpointed.append(r.front());
		pack->unpointedNames.insert(r.front().source + QLatin1Char('|') + r.front().pointer,
					    safeFileName(o.value(QStringLiteral("fileName")).toString()));
	}
	// Media slots in sources that came with a file are only indexed here.
	for (int i = 0; i < pack->media.size(); ++i) {
		for (const MediaRef &r : pack->media[i].refs) {
			for (QVector<Source> *list : {&pack->scenes, &pack->sources})
				for (Source &s : *list)
					if (s.name == r.source && !s.media.contains(i))
						s.media.append(i);
		}
	}
	return true;
}

bool PackCore::sourceExists(const QString &name)
{
	obs_source_t *s = obs_get_source_by_name(name.toUtf8().constData());
	const bool exists = s != nullptr;
	obs_source_release(s);
	return exists;
}

QString PackCore::uniqueSourceName(const QString &base, const QSet<QString> &taken)
{
	if (!sourceExists(base) && !taken.contains(base))
		return base;
	for (int n = 2;; ++n) {
		const QString candidate = QStringLiteral("%1 (%2)").arg(base).arg(n);
		if (!sourceExists(candidate) && !taken.contains(candidate))
			return candidate;
	}
}

PackCore::ImportResult PackCore::importSources(const Pack &pack, const ImportPlan &plan)
{
	ImportResult result;
	QHash<QString, QString> finalName;
	QSet<QString> taken;
	QVector<const Source *> chosen;

	for (const QVector<Source> *list : {&pack.sources, &pack.scenes}) {
		for (const Source &s : *list) {
			if (!plan.items.contains(s.name))
				continue;
			Action action = plan.items.value(s.name);
			if (action == Action::Skip)
				continue;
			const bool exists = sourceExists(s.name) || taken.contains(s.name);
			if (action == Action::Create && exists)
				action = Action::Rename; // never two sources with one name
			if (action == Action::Replace && !exists)
				action = Action::Create;
			QString name = s.name;
			if (action == Action::Rename) {
				name = uniqueSourceName(s.name, taken);
				result.renamed.insert(s.name, name);
			} else if (action == Action::Replace) {
				obs_source_t *old = obs_get_source_by_name(s.name.toUtf8().constData());
				if (old) {
					obs_source_remove(old);
					taken.insert(s.name);
					obs_source_set_name(
						old,
						uniqueSourceName(s.name + QString::fromLatin1(kReplacedSuffix), taken)
							.toUtf8()
							.constData());
					obs_source_release(old);
					result.replaced.append(s.name);
				}
			}
			taken.insert(name);
			finalName.insert(s.name, name);
			chosen.append(&s);
		}
	}

	obs_data_array_t *array = obs_data_array_create();
	for (const Source *s : chosen) {
		const QString name = finalName.value(s->name);
		QJsonObject d = s->data;
		d.insert(QStringLiteral("name"), name);
		d.remove(QStringLiteral("uuid"));
		d.remove(QStringLiteral("canvas_uuid"));
		if (s->isScene || s->isGroup) {
			QJsonObject settings = d.value(QStringLiteral("settings")).toObject();
			QJsonArray items = settings.value(QStringLiteral("items")).toArray();
			for (int i = 0; i < items.size(); ++i) {
				QJsonObject item = items.at(i).toObject();
				const QString ref = item.value(QStringLiteral("name")).toString();
				if (finalName.contains(ref))
					item.insert(QStringLiteral("name"), finalName.value(ref));
				item.remove(QStringLiteral("source_uuid"));
				items[i] = item;
			}
			settings.insert(QStringLiteral("items"), items);
			d.insert(QStringLiteral("settings"), settings);
		}
		for (int i = 0; i < pack.media.size(); ++i) {
			for (const MediaRef &ref : pack.media[i].refs) {
				if (ref.source != s->name)
					continue;
				if (plan.extractedMedia.contains(i))
					d = setPointer(d, ref.pointer, plan.extractedMedia.value(i));
				else
					result.unpointed.append(
						QStringLiteral("%1: %2").arg(name, pack.media[i].fileName));
			}
		}
		for (const MediaRef &ref : pack.unpointed) {
			if (ref.source == s->name)
				result.unpointed.append(QStringLiteral("%1: %2").arg(
					name, pack.unpointedNames.value(ref.source + QLatin1Char('|') + ref.pointer)));
		}
		obs_data_t *od = obs_data_create_from_json(QJsonDocument(d).toJson(QJsonDocument::Compact).constData());
		if (od) {
			obs_data_array_push_back(array, od);
			obs_data_release(od);
			result.created.append(name);
		}
	}
	// Two passes inside libobs: create everything, then load (scene items
	// find their sources by name, including ones created in this batch).
	obs_load_sources(array, nullptr, nullptr);
	obs_data_array_release(array);
	return result;
}
