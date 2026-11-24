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

// Deck packs (.dockdeck): the shared checklist dialog plus DockDeck's
// exportPack() / importPack(). What goes into a pack is decided in
// pack-core.cpp; the file format is a plain ZIP (pack-zip.cpp).

#include "deck-pack.hpp"
#include "dock-deck.hpp"
#include "scene-organizer.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <atomic>

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString currentCollectionName()
{
	char *name = obs_frontend_get_current_scene_collection();
	const QString out = QString::fromUtf8(name ? name : "");
	bfree(name);
	return out;
}

static QString sizeText(qint64 bytes)
{
	return QLocale().formattedDataSize(bytes);
}

static QString safeBaseName(QString s)
{
	s.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
	s = s.trimmed();
	return s.isEmpty() ? QStringLiteral("Deck Pack") : s.left(120);
}

// ---------------------------------------------------------------------------
// PackDialog
// ---------------------------------------------------------------------------

enum { KeyRole = Qt::UserRole, ColName = 0, ColDetail = 1, ColAction = 2 };

PackDialog::PackDialog(bool importing, const QString &title, const QString &intro, const QVector<PackItem> &items,
		       QWidget *parent)
	: QDialog(parent),
	  importing_(importing)
{
	setWindowTitle(title);
	resize(640, 560);
	auto *layout = new QVBoxLayout(this);
	auto *label = new QLabel(intro, this);
	label->setWordWrap(true);
	layout->addWidget(label);

	master_ = new QCheckBox(T("DockDeck.Dialog.SelectAll"), this);
	master_->setTristate(true);
	layout->addWidget(master_);

	tree_ = new QTreeWidget(this);
	tree_->setColumnCount(importing ? 3 : 2);
	QStringList headers{T("DockDeck.Pack.ColName"), T("DockDeck.Pack.ColDetail")};
	if (importing)
		headers << T("DockDeck.Pack.ColAction");
	tree_->setHeaderLabels(headers);
	tree_->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
	tree_->header()->setStretchLastSection(false);
	layout->addWidget(tree_, 1);

	const char *groupKeys[] = {"DockDeck.Pack.Decks", "DockDeck.Pack.Scenes", "DockDeck.Pack.Sources",
				   "DockDeck.Pack.Media", "DockDeck.Pack.Organizer"};
	for (const char *key : groupKeys) {
		auto *group = new QTreeWidgetItem(tree_, {T(key)});
		group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
		group->setCheckState(ColName, Qt::Unchecked);
		groups_.append(group);
	}

	for (const PackItem &it : items) {
		auto *row = new QTreeWidgetItem(groups_[it.kind], {it.label, it.detail});
		row->setData(ColName, KeyRole, it.key);
		row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
		row->setCheckState(ColName, it.checked ? Qt::Checked : Qt::Unchecked);
		if (!it.tooltip.isEmpty()) {
			row->setToolTip(ColName, it.tooltip);
			row->setToolTip(ColDetail, it.tooltip);
		}
		items_.insert(it.key, row);
		deps_.insert(it.key, it.deps);
		if (it.kind == PackItem::Media)
			sizes_.insert(it.key, it.size);
		if (importing && it.collides) {
			auto *box = new QComboBox(tree_);
			box->addItem(it.kind == PackItem::Deck ? T("DockDeck.Import.KeepBoth")
							       : T("DockDeck.Pack.Rename"),
				     static_cast<int>(PackCore::Action::Rename));
			box->addItem(T("DockDeck.Import.Skip"), static_cast<int>(PackCore::Action::Skip));
			box->addItem(T("DockDeck.Import.Replace"), static_cast<int>(PackCore::Action::Replace));
			box->setToolTip(T("DockDeck.Pack.Exists"));
			tree_->setItemWidget(row, ColAction, box);
			actions_.insert(it.key, box);
			row->setIcon(ColName, style()->standardIcon(QStyle::SP_MessageBoxWarning));
		}
	}
	for (QTreeWidgetItem *group : groups_) {
		group->setHidden(group->childCount() == 0);
		group->setExpanded(true);
	}
	tree_->resizeColumnToContents(ColDetail);
	if (importing)
		tree_->resizeColumnToContents(ColAction);

	total_ = new QLabel(this);
	total_->setWordWrap(true);
	layout->addWidget(total_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	ok_ = buttons->addButton(importing ? T("DockDeck.Pack.ImportButton") : T("DockDeck.Dialog.ExportButton"),
				 QDialogButtonBox::AcceptRole);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	// Pre-checked items bring their dependencies along (a deck's linked
	// scene, a scene's sources, a source's media).
	for (const PackItem &it : items)
		if (it.checked)
			checkWithDeps(it.key);

	connect(tree_, &QTreeWidget::itemChanged, this, &PackDialog::onItemChanged);
	connect(master_, &QCheckBox::clicked, this, [this]() {
		// Qt cycles unchecked -> partial -> checked: from none or some a
		// click selects all, from all it selects none.
		const Qt::CheckState state = master_->checkState() != Qt::Unchecked ? Qt::Checked : Qt::Unchecked;
		for (QTreeWidgetItem *group : groups_)
			for (int i = 0; i < group->childCount(); ++i)
				group->child(i)->setCheckState(ColName, state);
		sync();
	});
	sync();
}

void PackDialog::onItemChanged(QTreeWidgetItem *item)
{
	const QString key = item->data(ColName, KeyRole).toString();
	if (!key.isEmpty() && item->checkState(ColName) == Qt::Checked)
		checkWithDeps(key);
	if (!syncing_)
		sync();
}

void PackDialog::checkWithDeps(const QString &key)
{
	QStringList queue{key};
	QSet<QString> seen;
	while (!queue.isEmpty()) {
		const QString k = queue.takeFirst();
		if (seen.contains(k))
			continue;
		seen.insert(k);
		if (QTreeWidgetItem *row = items_.value(k)) {
			if (row->checkState(ColName) != Qt::Checked)
				row->setCheckState(ColName, Qt::Checked);
		}
		queue.append(deps_.value(k));
	}
}

qint64 PackDialog::checkedBytes() const
{
	qint64 total = 0;
	for (auto it = sizes_.begin(); it != sizes_.end(); ++it)
		if (items_.value(it.key())->checkState(ColName) == Qt::Checked)
			total += it.value();
	return total;
}

void PackDialog::sync()
{
	syncing_ = true;
	int checked = 0;
	for (QTreeWidgetItem *row : items_)
		checked += row->checkState(ColName) == Qt::Checked ? 1 : 0;
	master_->setCheckState(checked == 0 ? Qt::Unchecked
					    : (checked == items_.size() ? Qt::Checked : Qt::PartiallyChecked));
	ok_->setEnabled(checked > 0);
	const qint64 bytes = checkedBytes();
	QString text = T("DockDeck.Pack.Total").arg(checked).arg(sizeText(bytes));
	if (bytes > kPackWarnBytes)
		text += QStringLiteral("  ") + T("DockDeck.Pack.Large");
	total_->setText(text);
	syncing_ = false;
}

QSet<QString> PackDialog::checkedKeys() const
{
	QSet<QString> keys;
	for (auto it = items_.begin(); it != items_.end(); ++it)
		if (it.value()->checkState(ColName) == Qt::Checked)
			keys.insert(it.key());
	return keys;
}

PackCore::Action PackDialog::action(const QString &key) const
{
	QComboBox *box = actions_.value(key);
	if (!box)
		return PackCore::Action::Create;
	return static_cast<PackCore::Action>(box->currentData().toInt());
}

// ---------------------------------------------------------------------------
// Background file work with a progress dialog
// ---------------------------------------------------------------------------

// Runs `work` on a worker thread while a cancellable progress dialog keeps
// the UI responsive. Returns false on failure or cancel (*error empty).
static bool runWithProgress(QWidget *parent, const QString &label, qint64 total,
			    const std::function<bool(const PackZip::Progress &, QString *)> &work, QString *error)
{
	std::atomic<qint64> done{0};
	std::atomic<bool> cancel{false};
	bool ok = false;
	QString err;

	QProgressDialog dialog(label, T("DockDeck.Pack.Cancel"), 0, 1000, parent);
	dialog.setWindowModality(Qt::WindowModal);
	dialog.setMinimumDuration(400);
	dialog.setAutoClose(false);
	dialog.setAutoReset(false);
	dialog.setValue(0);

	QThread *thread = QThread::create([&]() {
		ok = work(
			[&](qint64 d) {
				done = d;
				return !cancel.load();
			},
			&err);
	});
	QEventLoop loop;
	QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
	QObject::connect(&dialog, &QProgressDialog::canceled, &loop, [&]() { cancel = true; });
	QTimer timer;
	timer.setInterval(100);
	QObject::connect(&timer, &QTimer::timeout, &dialog, [&]() {
		if (total > 0 && !cancel.load())
			dialog.setValue(static_cast<int>(qMin<qint64>(1000, done.load() * 1000 / total)));
	});
	thread->start();
	timer.start();
	loop.exec();
	thread->wait();
	delete thread;
	timer.stop();
	// Read the answer first: closing a QProgressDialog emits canceled().
	const bool cancelled = cancel.load();
	dialog.disconnect();
	dialog.close();

	if (cancelled) {
		ok = false;
		err.clear();
	}
	*error = err;
	return ok;
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

static QStringList frontendSceneNames()
{
	QStringList names;
	char **list = obs_frontend_get_scene_names();
	for (char **p = list; p && *p; ++p)
		names.append(QString::fromUtf8(*p));
	bfree(list);
	return names;
}

static QString typeName(const PackCore::Source &s)
{
	if (s.isScene)
		return T("DockDeck.Pack.TypeScene");
	if (s.isGroup)
		return T("DockDeck.Pack.TypeGroup");
	const char *display = obs_source_get_display_name(s.id.toUtf8().constData());
	return display ? QString::fromUtf8(display) : T("DockDeck.Pack.TypeMissing").arg(s.id);
}

static QStringList sourceDeps(const PackCore::Source &s)
{
	QStringList deps;
	for (const QString &d : s.deps)
		deps.append(QStringLiteral("s:") + d);
	for (const int m : s.media)
		deps.append(QStringLiteral("m:%1").arg(m));
	return deps;
}

static QString deckDetail(const Deck &deck, const QString &scene)
{
	QStringList parts;
	if (deck.locked)
		parts << T("DockDeck.Pack.Locked");
	if (!scene.isEmpty())
		parts << T("DockDeck.Scene.Tooltip").arg(scene);
	return parts.join(QStringLiteral(" · "));
}

static int countGroups(const QJsonArray &nodes)
{
	int n = 0;
	for (const QJsonValue &v : nodes) {
		const QJsonObject o = v.toObject();
		if (o.contains(QStringLiteral("group")))
			n += 1 + countGroups(o.value(QStringLiteral("items")).toArray());
	}
	return n;
}

static PackItem organizerItem(int groups)
{
	PackItem it;
	it.kind = PackItem::Organizer;
	it.key = QStringLiteral("o:");
	it.label = T("DockDeck.Pack.OrganizerItem");
	it.detail = T("DockDeck.Pack.OrganizerDetail").arg(groups);
	it.checked = true;
	return it;
}

static QString summarize(const QStringList &list, int max = 12)
{
	QStringList shown = list.mid(0, max);
	QString text = QStringLiteral("• ") + shown.join(QStringLiteral("\n• "));
	if (list.size() > max)
		text += QStringLiteral("\n") + T("DockDeck.Pack.AndMore").arg(list.size() - max);
	return text;
}

void DockDeck::exportPack(const QString &onlyName)
{
	if (!ready_)
		return;
	if (store_.autoSave)
		captureActive();

	const QString collection = currentCollectionName();
	const PackCore::Inventory inv = PackCore::scan(frontendSceneNames());

	QVector<PackItem> items;
	for (const Deck &deck : store_.decks) {
		const QString scene = deck.scenes.value(collection);
		PackItem it;
		it.kind = PackItem::Deck;
		it.key = QStringLiteral("d:") + deck.name;
		it.label = deck.name;
		it.detail = deckDetail(deck, scene);
		if (!scene.isEmpty() && inv.find(scene))
			it.deps << QStringLiteral("s:") + scene;
		it.checked = onlyName.isEmpty() || deck.name == onlyName; // linked scenes follow
		items.append(it);
	}
	for (const QVector<PackCore::Source> *list : {&inv.scenes, &inv.sources}) {
		for (const PackCore::Source &s : *list) {
			PackItem it;
			it.kind = s.isScene ? PackItem::Scene : PackItem::Source;
			it.key = QStringLiteral("s:") + s.name;
			it.label = s.name;
			it.detail = typeName(s);
			it.deps = sourceDeps(s);
			items.append(it);
		}
	}
	for (int i = 0; i < inv.media.size(); ++i) {
		const PackCore::Media &m = inv.media[i];
		PackItem it;
		it.kind = PackItem::Media;
		it.key = QStringLiteral("m:%1").arg(i);
		it.label = QFileInfo(m.path).fileName();
		it.detail = sizeText(m.size);
		it.tooltip = QDir::toNativeSeparators(m.path);
		it.size = m.size;
		items.append(it);
	}
	SceneOrganizer *organizer = SceneOrganizer::instance();
	if (organizer && organizer->groupCount() > 0)
		items.append(organizerItem(organizer->groupCount()));

	PackDialog dialog(false, T("DockDeck.Pack.ExportTitle"), T("DockDeck.Pack.ExportIntro"), items, main_);
	if (dialog.exec() != QDialog::Accepted)
		return;
	const QSet<QString> keys = dialog.checkedKeys();

	QVector<Deck> decks;
	QSet<QString> names;
	QSet<int> media;
	for (const Deck &deck : store_.decks)
		if (keys.contains(QStringLiteral("d:") + deck.name))
			decks.append(deck);
	for (const QString &k : keys) {
		if (k.startsWith(QStringLiteral("s:")))
			names.insert(k.mid(2));
		else if (k.startsWith(QStringLiteral("m:")))
			media.insert(k.mid(2).toInt());
	}

	const QString title = T("DockDeck.Pack.ExportTitle");
	const QJsonArray tree = organizer && keys.contains(QStringLiteral("o:")) ? organizer->packTree() : QJsonArray();
	const PackCore::ExportResult result = PackCore::buildExport(
		inv, decks, names, media, collection, QStringLiteral("Dock Deck ") + PLUGIN_VERSION, tree);
	if (result.mediaBytes > PackZip::kMaxArchiveBytes) {
		QMessageBox::warning(main_, title, T("DockDeck.Pack.TooBig").arg(sizeText(result.mediaBytes)));
		return;
	}
	if (result.mediaBytes > kPackWarnBytes &&
	    QMessageBox::question(main_, title, T("DockDeck.Pack.LargeAsk").arg(sizeText(result.mediaBytes))) !=
		    QMessageBox::Yes)
		return;

	const QString base = safeBaseName(decks.size() == 1 ? decks.front().name : T("DockDeck.Pack.FileName"));
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	QString path = QFileDialog::getSaveFileName(
		main_, title, QDir(dir).filePath(base + QStringLiteral(".dockdeck")), T("DockDeck.Pack.FileFilter"));
	if (path.isEmpty())
		return;
	if (!path.endsWith(QStringLiteral(".dockdeck"), Qt::CaseInsensitive))
		path += QStringLiteral(".dockdeck");

	const QVector<PackZip::Entry> entries = result.entries;
	const qint64 total = result.mediaBytes + result.manifest.size();
	QString error;
	const bool ok = runWithProgress(
		main_, T("DockDeck.Pack.Writing"), total,
		[&](const PackZip::Progress &progress, QString *err) {
			return PackZip::write(path, entries, progress, err);
		},
		&error);
	if (!ok) {
		if (!error.isEmpty())
			QMessageBox::warning(main_, title, T("DockDeck.Dialog.ExportFailed").arg(path, error));
		return;
	}
	obs_log(LOG_INFO, "exported pack %s: %d deck(s), %d scene/source(s), %d media file(s), %d setting(s) stripped",
		path.toUtf8().constData(), static_cast<int>(decks.size()), static_cast<int>(names.size()),
		static_cast<int>(result.entries.size() - 1), static_cast<int>(result.stripped.size()));

	QString summary = T("DockDeck.Pack.Exported")
				  .arg(decks.size())
				  .arg(names.size())
				  .arg(result.entries.size() - 1)
				  .arg(sizeText(QFileInfo(path).size()));
	if (!result.stripped.isEmpty())
		summary += QStringLiteral("\n\n") + T("DockDeck.Pack.Stripped") + QStringLiteral("\n") +
			   summarize(result.stripped);
	if (!result.unpointed.isEmpty())
		summary += QStringLiteral("\n\n") + T("DockDeck.Pack.UnpointedExport") + QStringLiteral("\n") +
			   summarize(result.unpointed);
	if (!result.urls.isEmpty())
		summary += QStringLiteral("\n\n") + T("DockDeck.Pack.Urls") + QStringLiteral("\n") +
			   summarize(result.urls);
	QMessageBox::information(main_, title, summary);
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

static bool deckNameTaken(const DeckStore &store, const QString &name)
{
	for (const Deck &d : store.decks)
		if (d.name.compare(name, Qt::CaseInsensitive) == 0)
			return true;
	return false;
}

// A file name in `dir` that doesn't exist yet: "clip.mp4", "clip (2).mp4"...
static QString freePath(const QDir &dir, const QString &fileName, QSet<QString> *taken)
{
	const QFileInfo fi(fileName);
	const QString stem = fi.completeBaseName();
	const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
	QString candidate = fileName;
	for (int n = 2; QFileInfo::exists(dir.filePath(candidate)) || taken->contains(candidate); ++n)
		candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix);
	taken->insert(candidate);
	return dir.filePath(candidate);
}

void DockDeck::importPack()
{
	if (!ready_)
		return;
	const QString title = T("DockDeck.Pack.ImportTitle");
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	const QString path = QFileDialog::getOpenFileName(main_, title, dir, T("DockDeck.Pack.FileFilter"));
	if (path.isEmpty())
		return;

	PackZip::Reader reader;
	PackCore::Pack pack;
	QString error;
	if (!reader.open(path, &error) || !PackCore::readPack(reader, &pack, &error)) {
		QMessageBox::warning(main_, title,
				     T("DockDeck.Pack.ReadFailed").arg(QFileInfo(path).fileName(), error));
		return;
	}

	QVector<PackItem> items;
	for (const Deck &deck : pack.decks) {
		const QString scene = deck.scenes.value(pack.collection);
		PackItem it;
		it.kind = PackItem::Deck;
		it.key = QStringLiteral("d:") + deck.name;
		it.label = deck.name;
		it.detail = deckDetail(deck, scene);
		if (!scene.isEmpty() && pack.find(scene))
			it.deps << QStringLiteral("s:") + scene;
		it.checked = true;
		it.collides = deckNameTaken(store_, deck.name);
		items.append(it);
	}
	for (const QVector<PackCore::Source> *list : {&pack.scenes, &pack.sources}) {
		for (const PackCore::Source &s : *list) {
			PackItem it;
			it.kind = s.isScene ? PackItem::Scene : PackItem::Source;
			it.key = QStringLiteral("s:") + s.name;
			it.label = s.name;
			it.detail = typeName(s);
			it.deps = sourceDeps(s);
			it.checked = true;
			it.collides = PackCore::sourceExists(s.name);
			items.append(it);
		}
	}
	for (int i = 0; i < pack.media.size(); ++i) {
		PackItem it;
		it.kind = PackItem::Media;
		it.key = QStringLiteral("m:%1").arg(i);
		it.label = pack.media[i].fileName;
		it.detail = sizeText(pack.media[i].size);
		it.size = pack.media[i].size;
		it.checked = true;
		items.append(it);
	}
	if (!pack.organizer.isEmpty() && SceneOrganizer::instance())
		items.append(organizerItem(countGroups(pack.organizer)));

	PackDialog dialog(true, title, T("DockDeck.Pack.ImportIntro").arg(QFileInfo(path).fileName()), items, main_);
	if (dialog.exec() != QDialog::Accepted)
		return;
	const QSet<QString> keys = dialog.checkedKeys();

	PackCore::ImportPlan plan;
	for (const QVector<PackCore::Source> *list : {&pack.scenes, &pack.sources})
		for (const PackCore::Source &s : *list) {
			const QString key = QStringLiteral("s:") + s.name;
			if (keys.contains(key))
				plan.items.insert(s.name, dialog.action(key));
		}

	// Media that a source being created actually uses, extracted to
	// <plugin config>/media/<pack name>/ off the UI thread.
	struct Job {
		int index;
		QString entry;
		QString dest;
	};
	QVector<Job> jobs;
	qint64 jobBytes = 0;
	char *mediaRoot = obs_module_config_path("media");
	const QDir packDir(
		QDir(QString::fromUtf8(mediaRoot)).filePath(safeBaseName(QFileInfo(path).completeBaseName())));
	bfree(mediaRoot);
	QSet<QString> takenFiles;
	for (int i = 0; i < pack.media.size(); ++i) {
		if (!keys.contains(QStringLiteral("m:%1").arg(i)))
			continue;
		bool used = false;
		for (const PackCore::MediaRef &ref : pack.media[i].refs) {
			const auto action = plan.items.value(ref.source, PackCore::Action::Skip);
			used = used || (plan.items.contains(ref.source) && action != PackCore::Action::Skip);
		}
		if (!used)
			continue;
		jobs.append({i, pack.media[i].entry, freePath(packDir, pack.media[i].fileName, &takenFiles)});
		jobBytes += pack.media[i].size;
	}
	if (!jobs.isEmpty()) {
		if (!QDir().mkpath(packDir.absolutePath())) {
			QMessageBox::warning(main_, title,
					     T("DockDeck.Pack.ExtractFailed").arg(packDir.absolutePath()));
			return;
		}
		const bool ok = runWithProgress(
			main_, T("DockDeck.Pack.Extracting"), jobBytes,
			[&](const PackZip::Progress &progress, QString *err) {
				qint64 before = 0;
				for (const Job &job : jobs) {
					const bool done = reader.extract(
						job.entry, job.dest, [&](qint64 d) { return progress(before + d); },
						err);
					if (!done)
						return false;
					before += reader.size(job.entry);
				}
				return true;
			},
			&error);
		if (!ok) {
			for (const Job &job : jobs)
				QFile::remove(job.dest);
			if (!error.isEmpty())
				QMessageBox::warning(main_, title, T("DockDeck.Pack.ExtractFailed").arg(error));
			return;
		}
		for (const Job &job : jobs)
			plan.extractedMedia.insert(job.index, job.dest);
	}

	if (store_.autoSave)
		captureActive();

	const PackCore::ImportResult res = PackCore::importSources(pack, plan);
	QStringList missingTypes;
	for (const QVector<PackCore::Source> *list : {&pack.scenes, &pack.sources})
		for (const PackCore::Source &s : *list)
			if (plan.items.contains(s.name) && !s.isScene && !s.isGroup &&
			    !obs_source_get_display_name(s.id.toUtf8().constData()) && !missingTypes.contains(s.id))
				missingTypes.append(s.id);

	// Decks: Rename (= Keep Both) / Skip / Replace, like a deck file import.
	const QString collection = currentCollectionName();
	const QHash<QString, QDockWidget *> localDocks = realDocks();
	QSet<QString> missingDocks;
	int added = 0, replaced = 0, skipped = 0;
	bool reapplyActive = false;
	for (Deck deck : pack.decks) {
		const QString key = QStringLiteral("d:") + deck.name;
		if (!keys.contains(key))
			continue;
		const PackCore::Action action = dialog.action(key);
		if (action == PackCore::Action::Skip) {
			++skipped;
			continue;
		}
		// The linked scene follows renames and lands in this collection.
		const QString linked = deck.scenes.value(pack.collection);
		if (!linked.isEmpty())
			deck.scenes.insert(collection, res.renamed.value(linked, linked));
		for (auto it = deck.docks.constBegin(); it != deck.docks.constEnd(); ++it)
			if (it.value() && !localDocks.contains(it.key()))
				missingDocks.insert(deck.titles.value(it.key(), it.key()));
		if (mergeImportedDeck(deck, action == PackCore::Action::Replace, &reapplyActive))
			++replaced;
		else
			++added;
	}
	if (reapplyActive) {
		const QString name = store_.active;
		store_.active.clear();
		switchToDeck(name, SwitchSource::Import);
	}
	saveConfig();
	scheduleRefresh();

	// Scene Organizer folders, merged into this collection's (scenes by name).
	SceneOrganizer::PackImport folders;
	const bool importFolders = keys.contains(QStringLiteral("o:")) && SceneOrganizer::instance();
	if (importFolders)
		folders = SceneOrganizer::instance()->importPackTree(pack.organizer, res.renamed);
	obs_frontend_save();

	obs_log(LOG_INFO, "imported pack %s: decks %d added, %d replaced, %d skipped; %d scene/source(s) created",
		path.toUtf8().constData(), added, replaced, skipped, static_cast<int>(res.created.size()));

	QString summary = T("DockDeck.Import.Summary").arg(added).arg(replaced).arg(skipped);
	summary += QStringLiteral("\n") + T("DockDeck.Pack.Created").arg(res.created.size()).arg(jobs.size());
	if (!res.renamed.isEmpty()) {
		QStringList list;
		for (auto it = res.renamed.begin(); it != res.renamed.end(); ++it)
			list << QStringLiteral("%1 → %2").arg(it.key(), it.value());
		list.sort(Qt::CaseInsensitive);
		summary += QStringLiteral("\n\n") + T("DockDeck.Pack.Renamed") + QStringLiteral("\n") + summarize(list);
	}
	if (!res.unpointed.isEmpty())
		summary += QStringLiteral("\n\n") + T("DockDeck.Pack.Unpointed") + QStringLiteral("\n") +
			   summarize(res.unpointed);
	if (!missingTypes.isEmpty())
		summary += QStringLiteral("\n\n") +
			   T("DockDeck.Pack.MissingTypes").arg(missingTypes.join(QStringLiteral(", ")));
	if (!missingDocks.isEmpty()) {
		QStringList names = missingDocks.values();
		names.sort(Qt::CaseInsensitive);
		summary += QStringLiteral("\n\n") +
			   T("DockDeck.Import.MissingDocks").arg(names.join(QStringLiteral(", ")));
	}
	if (importFolders) {
		summary += QStringLiteral("\n\n") +
			   T("DockDeck.Pack.OrganizerImported").arg(folders.newGroups).arg(folders.placed);
		if (!folders.missing.isEmpty())
			summary += QStringLiteral("\n") + T("DockDeck.Pack.OrganizerMissing") + QStringLiteral("\n") +
				   summarize(folders.missing);
	}
	QMessageBox::information(main_, title, summary);
}
