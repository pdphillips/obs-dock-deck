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

#include "scene-organizer.hpp"

#include <obs-module.h>
#include <obs.h>

#include <QAction>
#include <QDropEvent>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMessageBox>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QWidgetAction>
#include <QFile>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QSaveFile>
#include <QPointer>
#include <QUuid>
#include <QVBoxLayout>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>

#include <functional>

namespace {

// kind: 0 scene, 1 group; key: scene uuid / group id; open: the user's
// expand state (searching expands groups temporarily).
enum Role { KindRole = Qt::UserRole, KeyRole, OpenRole };
enum Kind { SceneItem = 0, GroupItem = 1 };

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool isGroup(const QTreeWidgetItem *item)
{
	return item && item->data(0, KindRole).toInt() == GroupItem;
}

QString currentCollection()
{
	char *name = obs_frontend_get_current_scene_collection();
	const QString out = QString::fromUtf8(name ? name : "");
	bfree(name);
	return out;
}

struct SceneInfo {
	QString uuid;
	QString name;
};

// OBS's scene list, in OBS's order.
QVector<SceneInfo> obsScenes()
{
	QVector<SceneInfo> out;
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		obs_source_t *s = list.sources.array[i];
		out.append({QString::fromUtf8(obs_source_get_uuid(s)), QString::fromUtf8(obs_source_get_name(s))});
	}
	obs_frontend_source_list_free(&list);
	return out;
}

QTreeWidgetItem *makeScene(const QString &uuid, const QString &name)
{
	auto *item = new QTreeWidgetItem({name});
	item->setData(0, KindRole, SceneItem);
	item->setData(0, KeyRole, uuid);
	// Editable = F2 / Rename renames the OBS scene. No drops into scenes.
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsEditable);
	return item;
}

void allItems(QTreeWidgetItem *root, QVector<QTreeWidgetItem *> *out)
{
	for (int i = 0; i < root->childCount(); ++i) {
		out->append(root->child(i));
		allItems(root->child(i), out);
	}
}

// The scene OBS's own list has selected: Preview in Studio Mode, else Program.
QString obsSelectedUuid()
{
	obs_source_t *scene = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									 : obs_frontend_get_current_scene();
	const QString uuid = QString::fromUtf8(scene ? obs_source_get_uuid(scene) : "");
	obs_source_release(scene);
	return uuid;
}

// One of OBS's own main-window actions, when running inside OBS.
QAction *obsAction(const char *name)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	return main ? main->findChild<QAction *>(QString::fromLatin1(name)) : nullptr;
}

void showScene(obs_source_t *scene) // switch like a click in OBS's list
{
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(scene);
	else
		obs_frontend_set_current_scene(scene);
}

QString uniqueName(const QString &format) // "<format with %1>" counting from 2
{
	for (int i = 2;; ++i) {
		const QString name = format.arg(i);
		obs_source_t *s = obs_get_source_by_name(name.toUtf8().constData());
		if (!s)
			return name;
		obs_source_release(s);
	}
}

} // namespace

void OrganizerTree::dropEvent(QDropEvent *event)
{
	QTreeWidget::dropEvent(event);
	emit dropped();
}

SceneOrganizer::SceneOrganizer(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("dockDeckSceneOrganizer"));
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);

	search_ = new QLineEdit(this);
	search_->setPlaceholderText(T("DockDeck.Organizer.Search"));
	search_->setClearButtonEnabled(true);
	layout->addWidget(search_);

	tree_ = new OrganizerTree(this);
	tree_->setHeaderHidden(true);
	tree_->setDragDropMode(QAbstractItemView::InternalMove);
	tree_->setDefaultDropAction(Qt::MoveAction);
	tree_->setSelectionMode(QAbstractItemView::SingleSelection);
	tree_->setEditTriggers(QAbstractItemView::EditKeyPressed); // F2 renames a group
	tree_->setContextMenuPolicy(Qt::CustomContextMenu);
	tree_->invisibleRootItem()->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
	layout->addWidget(tree_);

	char *file = obs_module_config_path("organizer.json");
	path_ = QString::fromUtf8(file ? file : "");
	bfree(file);

	connect(search_, &QLineEdit::textChanged, this, &SceneOrganizer::setFilter);
	connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) { activate(item); });
	connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { activate(item); });
	connect(tree_, &OrganizerTree::dropped, this, &SceneOrganizer::save);
	connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
		if (syncing_)
			return;
		if (!isGroup(item)) {
			renameScene(item, item->text(0));
			return;
		}
		if (item->text(0).trimmed().isEmpty()) {
			const QSignalBlocker block(tree_);
			item->setText(0, T("DockDeck.Organizer.NewGroup"));
		}
		save(); // renamed
	});
	connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
		if (!syncing_ && search_->text().trimmed().isEmpty()) {
			item->setData(0, OpenRole, true);
			save();
		}
	});
	connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
		if (!syncing_ && search_->text().trimmed().isEmpty()) {
			item->setData(0, OpenRole, false);
			save();
		}
	});
	connect(tree_, &QWidget::customContextMenuRequested, this, &SceneOrganizer::showMenu);

	signal_handler_connect(obs_get_signal_handler(), "source_rename", onSourceRename, this);
}

SceneOrganizer::~SceneOrganizer()
{
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", onSourceRename, this);
	obs_weak_source_release(copiedFilters_);
}

// libobs "source_rename" (any thread): show a renamed scene's new name.
void SceneOrganizer::onSourceRename(void *data, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	QPointer<SceneOrganizer> self(static_cast<SceneOrganizer *>(data));
	QMetaObject::invokeMethod(
		self,
		[self]() {
			if (self)
				self->refresh();
		},
		Qt::QueuedConnection);
}

void SceneOrganizer::onFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		ready_ = true;
		loadCollection();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		if (ready_)
			save();
		ready_ = false; // the scene list is torn down and rebuilt now
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_RENAMED: {
		const QString now = currentCollection();
		if (!collection_.isEmpty() && now != collection_) {
			QJsonObject root = readFile();
			QJsonObject all = root.value(QStringLiteral("collections")).toObject();
			all.insert(now, all.take(collection_));
			root.insert(QStringLiteral("collections"), all);
			QSaveFile f(path_);
			if (f.open(QIODevice::WriteOnly)) {
				f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
				f.commit();
			}
			collection_ = now;
		}
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		refresh();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		markCurrent();
		break;
	default:
		break;
	}
}

QJsonObject SceneOrganizer::readFile() const
{
	QFile f(path_);
	if (!f.open(QIODevice::ReadOnly) || f.size() > (8 << 20))
		return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}

void SceneOrganizer::buildFrom(const QJsonArray &nodes, QTreeWidgetItem *parent)
{
	for (const QJsonValue &v : nodes) {
		const QJsonObject o = v.toObject();
		if (o.contains(QStringLiteral("scene"))) {
			const QString uuid = o.value(QStringLiteral("scene")).toString();
			if (!uuid.isEmpty())
				parent->addChild(makeScene(uuid, QString()));
		} else if (o.contains(QStringLiteral("group"))) {
			QTreeWidgetItem *g = addGroup(o.value(QStringLiteral("group")).toString(), parent);
			const QString id = o.value(QStringLiteral("id")).toString();
			if (!id.isEmpty())
				g->setData(0, KeyRole, id);
			buildFrom(o.value(QStringLiteral("items")).toArray(), g);
			g->setData(0, OpenRole, o.value(QStringLiteral("open")).toBool(true));
			g->setExpanded(g->data(0, OpenRole).toBool());
		}
	}
}

void SceneOrganizer::loadCollection()
{
	collection_ = currentCollection();
	syncing_ = true;
	tree_->clear();
	const QJsonObject all = readFile().value(QStringLiteral("collections")).toObject();
	buildFrom(all.value(collection_).toObject().value(QStringLiteral("items")).toArray(),
		  tree_->invisibleRootItem());
	syncing_ = false;
	refresh();
}

QTreeWidgetItem *SceneOrganizer::addGroup(const QString &name, QTreeWidgetItem *parent)
{
	const bool wasSyncing = syncing_;
	syncing_ = true;
	auto *item = new QTreeWidgetItem({name.trimmed().isEmpty() ? T("DockDeck.Organizer.NewGroup") : name});
	item->setData(0, KindRole, GroupItem);
	item->setData(0, KeyRole, QUuid::createUuid().toString(QUuid::WithoutBraces));
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled |
		       Qt::ItemIsDropEnabled);
	item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
	(parent ? parent : tree_->invisibleRootItem())->addChild(item);
	item->setData(0, OpenRole, true);
	item->setExpanded(true);
	syncing_ = wasSyncing;
	return item;
}

void SceneOrganizer::deleteGroup(QTreeWidgetItem *group)
{
	if (!isGroup(group))
		return;
	QTreeWidgetItem *parent = group->parent() ? group->parent() : tree_->invisibleRootItem();
	int at = parent->indexOfChild(group);
	syncing_ = true;
	const QList<QTreeWidgetItem *> kids = group->takeChildren();
	for (QTreeWidgetItem *kid : kids)
		parent->insertChild(++at, kid);
	delete group;
	syncing_ = false;
	save();
}

void SceneOrganizer::refresh()
{
	if (!ready_)
		return;
	const QVector<SceneInfo> scenes = obsScenes();
	if (scenes.isEmpty())
		return; // mid-teardown; a collection always has a scene

	QHash<QString, QString> names;
	for (const SceneInfo &s : scenes)
		names.insert(s.uuid, s.name);

	syncing_ = true;
	QVector<QTreeWidgetItem *> items;
	allItems(tree_->invisibleRootItem(), &items);
	QSet<QString> seen;
	for (QTreeWidgetItem *item : items) {
		if (isGroup(item))
			continue;
		const QString uuid = item->data(0, KeyRole).toString();
		if (!names.contains(uuid) || seen.contains(uuid)) {
			delete item; // scene removed (or a duplicate entry)
			continue;
		}
		seen.insert(uuid);
		item->setText(0, names.value(uuid));
	}
	// New scenes go to the bottom, in OBS's order.
	for (const SceneInfo &s : scenes)
		if (!seen.contains(s.uuid))
			tree_->addTopLevelItem(makeScene(s.uuid, s.name));
	syncing_ = false;

	setFilter(search_->text());
	markCurrent();
	save();
}

void SceneOrganizer::setFilter(const QString &text)
{
	const QString needle = text.trimmed();
	// Returns whether `item` (or something inside it) matches.
	std::function<bool(QTreeWidgetItem *, bool)> apply = [&](QTreeWidgetItem *item, bool parentMatches) {
		const bool self = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
		bool child = false;
		for (int i = 0; i < item->childCount(); ++i)
			child = apply(item->child(i), parentMatches || (isGroup(item) && self && !needle.isEmpty())) ||
				child;
		const bool visible = self || child || parentMatches;
		item->setHidden(!visible);
		if (!needle.isEmpty() && (child || (self && isGroup(item))))
			item->setExpanded(true);
		return self || child;
	};
	syncing_ = true;
	for (int i = 0; i < tree_->topLevelItemCount(); ++i)
		apply(tree_->topLevelItem(i), false);
	if (needle.isEmpty()) { // back to the user's own expand state
		QVector<QTreeWidgetItem *> items;
		allItems(tree_->invisibleRootItem(), &items);
		for (QTreeWidgetItem *item : items)
			if (isGroup(item))
				item->setExpanded(item->data(0, OpenRole).toBool());
	}
	syncing_ = false;
	// Moving things around while some are hidden would be confusing.
	tree_->setDragDropMode(needle.isEmpty() ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
}

static QJsonArray nodesToJson(const QTreeWidgetItem *parent)
{
	QJsonArray out;
	for (int i = 0; i < parent->childCount(); ++i) {
		const QTreeWidgetItem *item = parent->child(i);
		if (isGroup(item))
			out.append(QJsonObject{{QStringLiteral("group"), item->text(0)},
					       {QStringLiteral("id"), item->data(0, KeyRole).toString()},
					       {QStringLiteral("open"), item->data(0, OpenRole).toBool()},
					       {QStringLiteral("items"), nodesToJson(item)}});
		else
			out.append(QJsonObject{{QStringLiteral("scene"), item->data(0, KeyRole).toString()}});
	}
	return out;
}

QJsonArray SceneOrganizer::toJson() const
{
	return nodesToJson(tree_->invisibleRootItem());
}

void SceneOrganizer::save()
{
	if (!ready_ || collection_.isEmpty() || path_.isEmpty())
		return;
	QJsonObject root = readFile();
	QJsonObject all = root.value(QStringLiteral("collections")).toObject();
	all.insert(collection_, QJsonObject{{QStringLiteral("items"), toJson()}});
	root.insert(QStringLiteral("version"), 1);
	root.insert(QStringLiteral("collections"), all);
	QSaveFile f(path_);
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
		f.commit();
	}
}

// Clicking a scene switches to it; in Studio Mode it goes to Preview, like
// OBS's own scene list.
void SceneOrganizer::activate(QTreeWidgetItem *item)
{
	if (!item || isGroup(item))
		return;
	obs_source_t *scene = obs_get_source_by_uuid(item->data(0, KeyRole).toString().toUtf8().constData());
	if (!scene)
		return;
	showScene(scene);
	obs_source_release(scene);
}

// Selects (without switching) the scene OBS shows: Preview in Studio Mode.
void SceneOrganizer::markCurrent()
{
	const QString uuid = obsSelectedUuid();
	QVector<QTreeWidgetItem *> items;
	allItems(tree_->invisibleRootItem(), &items);
	const QSignalBlocker block(tree_);
	tree_->clearSelection();
	for (QTreeWidgetItem *item : items) {
		if (!isGroup(item) && item->data(0, KeyRole).toString() == uuid) {
			item->setSelected(true);
			tree_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
			break;
		}
	}
}

QTreeWidgetItem *SceneOrganizer::findKey(const QString &key) const
{
	QVector<QTreeWidgetItem *> items;
	allItems(tree_->invisibleRootItem(), &items);
	for (QTreeWidgetItem *item : items)
		if (item->data(0, KeyRole).toString() == key)
			return item;
	return nullptr;
}

QString SceneOrganizer::nameProblem(obs_source_t *scene, const QString &name)
{
	const QString n = name.trimmed();
	if (n.isEmpty())
		return QStringLiteral("DockDeck.Obs.NoName");
	obs_source_t *other = obs_get_source_by_name(n.toUtf8().constData());
	const bool taken = other && other != scene;
	obs_source_release(other);
	return taken ? QStringLiteral("DockDeck.Obs.NameExists") : QString();
}

bool SceneOrganizer::warn(const QString &problem)
{
	if (prompts)
		QMessageBox::warning(this, T((problem + ".Title").toUtf8().constData()),
				     T((problem + ".Text").toUtf8().constData()));
	return false;
}

// OBS's "Add Scene" name prompt: re-asks on an empty or taken name. "" = cancel.
QString SceneOrganizer::askSceneName(const QString &suggestion)
{
	for (;;) {
		bool ok = false;
		const QString name = QInputDialog::getText(this, T("DockDeck.Obs.AddSceneTitle"),
							   T("DockDeck.Obs.AddSceneText"), QLineEdit::Normal,
							   suggestion, &ok)
					     .trimmed();
		if (!ok)
			return QString();
		const QString problem = nameProblem(nullptr, name);
		if (problem.isEmpty())
			return name;
		warn(problem);
	}
}

bool SceneOrganizer::renameScene(QTreeWidgetItem *item, const QString &name)
{
	obs_source_t *scene = obs_get_source_by_uuid(item->data(0, KeyRole).toString().toUtf8().constData());
	if (!scene)
		return false;
	const QString problem = nameProblem(scene, name);
	if (problem.isEmpty() && name.trimmed() != QString::fromUtf8(obs_source_get_name(scene)))
		obs_source_set_name(scene, name.trimmed().toUtf8().constData()); // OBS and we follow source_rename
	syncing_ = true;
	item->setText(0, QString::fromUtf8(obs_source_get_name(scene)));
	syncing_ = false;
	obs_source_release(scene);
	if (!problem.isEmpty()) // after the editor has closed
		QTimer::singleShot(0, this, [this, problem]() { warn(problem); });
	return problem.isEmpty();
}

// New scenes land at the bottom; move them next to where the user asked:
// below a scene, or at the end of a group.
void SceneOrganizer::placeNew(const QSet<QString> &before, const QString &anchorKey)
{
	refresh();
	QTreeWidgetItem *anchor = findKey(anchorKey);
	if (!anchor)
		return;
	for (const SceneInfo &s : obsScenes()) {
		QTreeWidgetItem *item = before.contains(s.uuid) ? nullptr : findKey(s.uuid);
		if (!item || item == anchor)
			continue;
		QTreeWidgetItem *from = item->parent() ? item->parent() : tree_->invisibleRootItem();
		from->removeChild(item);
		if (isGroup(anchor)) {
			anchor->addChild(item);
			anchor->setExpanded(true);
			anchor->setData(0, OpenRole, true);
		} else {
			QTreeWidgetItem *to = anchor->parent() ? anchor->parent() : tree_->invisibleRootItem();
			to->insertChild(to->indexOfChild(anchor) + 1, item);
			anchor = item;
		}
	}
	setFilter(search_->text());
	markCurrent();
	save();
}

void SceneOrganizer::addScene(QTreeWidgetItem *at)
{
	QSet<QString> before;
	for (const SceneInfo &s : obsScenes())
		before.insert(s.uuid);
	const QString anchorKey = at ? at->data(0, KeyRole).toString() : QString();
	if (QAction *add = obsAction("actionAddScene")) {
		add->trigger(); // OBS's own dialog, undo and switch
	} else {
		const QString name = askSceneName(uniqueName(T("DockDeck.Obs.DefaultSceneName")));
		if (name.isEmpty())
			return;
		obs_scene_t *scene = obs_scene_create(name.toUtf8().constData());
		showScene(obs_scene_get_source(scene));
		obs_scene_release(scene);
	}
	placeNew(before, anchorKey);
}

bool SceneOrganizer::duplicateScene(QTreeWidgetItem *item, const QString &name)
{
	obs_source_t *source = obs_get_source_by_uuid(item->data(0, KeyRole).toString().toUtf8().constData());
	if (!source)
		return false;
	const QString problem = nameProblem(nullptr, name);
	bool ok = false;
	if (problem.isEmpty()) {
		QSet<QString> before;
		for (const SceneInfo &s : obsScenes())
			before.insert(s.uuid);
		obs_scene_t *copy = obs_scene_duplicate(obs_scene_from_source(source), name.toUtf8().constData(),
							OBS_SCENE_DUP_REFS);
		ok = copy != nullptr;
		if (copy)
			showScene(obs_scene_get_source(copy)); // OBS switches to the copy too
		obs_scene_release(copy);
		placeNew(before, item->data(0, KeyRole).toString());
	}
	obs_source_release(source);
	return problem.isEmpty() ? ok : warn(problem);
}

bool SceneOrganizer::removeScene(QTreeWidgetItem *item)
{
	const QString uuid = item->data(0, KeyRole).toString();
	if (obsScenes().size() <= 1) {
		if (prompts)
			QMessageBox::information(this, T("DockDeck.Obs.FinalScene.Title"),
						 T("DockDeck.Obs.FinalScene.Text"));
		return false;
	}
	// The scene OBS has selected: let OBS remove it (same dialog, plus undo).
	QAction *obsRemove = prompts ? obsAction("actionRemoveScene") : nullptr;
	if (obsRemove && uuid == obsSelectedUuid()) {
		obsRemove->trigger();
		refresh();
		return !findKey(uuid);
	}
	obs_source_t *scene = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!scene)
		return false;
	bool yes = !prompts;
	if (prompts)
		yes = QMessageBox::question(
			      this, T("DockDeck.Obs.ConfirmRemove.Title"),
			      T("DockDeck.Obs.ConfirmRemove.Text").arg(QString::fromUtf8(obs_source_get_name(scene)))) ==
		      QMessageBox::Yes;
	if (yes) {
		obs_source_remove(scene); // what OBS does, minus its undo entry
		obs_enum_scenes(
			[](void *, obs_source_t *s) {
				if (!obs_source_is_group(s))
					obs_scene_prune_sources(obs_scene_from_source(s));
				return true;
			},
			nullptr);
	}
	obs_source_release(scene);
	refresh();
	return yes;
}

// OBS's Scenes-dock menu for one scene, in OBS's order, minus the Order
// submenu (the organizer's order is drag and drop) and Grid Mode.
void SceneOrganizer::addSceneActions(QMenu *menu, QTreeWidgetItem *item, obs_source_t *scene)
{
	// Items aren't QObjects: actions look the scene up by uuid when they run.
	const QString uuid = item->data(0, KeyRole).toString();
	auto withScene = [uuid](const std::function<void(obs_source_t *)> &fn) {
		return [uuid, fn]() {
			obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (s)
				fn(s);
			obs_source_release(s);
		};
	};
	menu->addSeparator();
	connect(menu->addAction(T("DockDeck.Obs.Duplicate")), &QAction::triggered, this, [this, uuid]() {
		QTreeWidgetItem *it = findKey(uuid);
		if (!it)
			return;
		const QString name = askSceneName(uniqueName(it->text(0) + QStringLiteral(" %1")));
		if (!name.isEmpty() && findKey(uuid))
			duplicateScene(findKey(uuid), name);
	});
	QAction *copy = menu->addAction(T("DockDeck.Obs.CopyFilters"));
	copy->setEnabled(obs_source_filter_count(scene) > 0);
	connect(copy, &QAction::triggered, this, withScene([this](obs_source_t *s) {
			obs_weak_source_release(copiedFilters_);
			copiedFilters_ = obs_source_get_weak_source(s);
		}));
	QAction *paste = menu->addAction(T("DockDeck.Obs.PasteFilters"));
	paste->setEnabled(copiedFilters_ && !obs_weak_source_expired(copiedFilters_));
	connect(paste, &QAction::triggered, this, withScene([this](obs_source_t *s) {
			obs_source_t *from = obs_weak_source_get_source(copiedFilters_);
			if (from && from != s)
				obs_source_copy_filters(s, from);
			obs_source_release(from);
		}));
	menu->addSeparator();
	connect(menu->addAction(T("DockDeck.Obs.Rename")), &QAction::triggered, this, [this, uuid]() {
		if (QTreeWidgetItem *it = findKey(uuid))
			tree_->editItem(it);
	});
	connect(menu->addAction(T("DockDeck.Obs.Remove")), &QAction::triggered, this, [this, uuid]() {
		if (QTreeWidgetItem *it = findKey(uuid))
			removeScene(it);
	});
	menu->addSeparator();
	QMenu *projector = menu->addMenu(T("DockDeck.Obs.Projector"));
	const QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); ++i) { // OBS's monitor labels
		const QRect g = screens[i]->geometry();
		const qreal r = screens[i]->devicePixelRatio();
#if defined(_WIN32) || defined(__APPLE__)
		QString name = screens[i]->name().simplified();
#else
		QString name = screens[i]->model().simplified();
		if (name.length() > 1 && name.endsWith('-'))
			name.chop(1);
#endif
		if (name.isEmpty())
			name = T("DockDeck.Obs.Display").arg(i + 1);
		const QString label = QStringLiteral("%1: %2x%3 @ %4,%5")
					      .arg(name)
					      .arg(int(g.width() * r))
					      .arg(int(g.height() * r))
					      .arg(g.x())
					      .arg(g.y());
		connect(projector->addAction(label), &QAction::triggered, this, withScene([i](obs_source_t *s) {
				obs_frontend_open_projector("Scene", i, "", obs_source_get_name(s));
			}));
	}
	projector->addSeparator();
	connect(projector->addAction(T("DockDeck.Obs.ProjectorWindow")), &QAction::triggered, this,
		withScene(
			[](obs_source_t *s) { obs_frontend_open_projector("Scene", -1, "", obs_source_get_name(s)); }));
	menu->addSeparator();
	connect(menu->addAction(T("DockDeck.Obs.Screenshot")), &QAction::triggered, this,
		withScene([](obs_source_t *s) { obs_frontend_take_source_screenshot(s); }));
	menu->addSeparator();
	connect(menu->addAction(T("DockDeck.Obs.Filters")), &QAction::triggered, this,
		withScene([](obs_source_t *s) { obs_frontend_open_source_filters(s); }));
	menu->addSeparator();

	// Transition Override: stored where OBS keeps it (scene private settings).
	obs_data_t *priv = obs_source_get_private_settings(scene);
	obs_data_set_default_int(priv, "transition_duration", 300);
	obs_data_set_default_bool(priv, "show_in_multiview", true);
	const QString current = QString::fromUtf8(obs_data_get_string(priv, "transition"));
	QMenu *override = menu->addMenu(T("DockDeck.Obs.TransitionOverride"));
	QStringList names{QString()};
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; ++i)
		names.append(QString::fromUtf8(obs_source_get_name(transitions.sources.array[i])));
	obs_frontend_source_list_free(&transitions);
	for (const QString &name : names) {
		QAction *a = override->addAction(name.isEmpty() ? T("DockDeck.Obs.None") : name);
		a->setCheckable(true);
		a->setChecked(name == current);
		connect(a, &QAction::triggered, this, withScene([name](obs_source_t *s) {
				obs_data_t *d = obs_source_get_private_settings(s);
				obs_data_set_string(d, "transition", name.toUtf8().constData());
				obs_data_release(d);
			}));
	}
	override->addSeparator();
	auto *duration = new QSpinBox(override);
	duration->setRange(50, 20000);
	duration->setSingleStep(50);
	duration->setSuffix(QStringLiteral(" ms"));
	duration->setValue(int(obs_data_get_int(priv, "transition_duration")));
	connect(duration, &QSpinBox::valueChanged, this, [uuid](int ms) {
		obs_source_t *s = obs_get_source_by_uuid(uuid.toUtf8().constData());
		obs_data_t *d = obs_source_get_private_settings(s);
		obs_data_set_int(d, "transition_duration", ms);
		obs_data_release(d);
		obs_source_release(s);
	});
	auto *durationAction = new QWidgetAction(override);
	durationAction->setDefaultWidget(duration);
	override->addAction(durationAction);

	QAction *multiview = menu->addAction(T("DockDeck.Obs.ShowInMultiview"));
	multiview->setCheckable(true);
	multiview->setChecked(obs_data_get_bool(priv, "show_in_multiview"));
	connect(multiview, &QAction::toggled, this, withScene([](obs_source_t *s) {
			obs_data_t *d = obs_source_get_private_settings(s);
			obs_data_set_bool(d, "show_in_multiview", !obs_data_get_bool(d, "show_in_multiview"));
			obs_data_release(d);
		}));
	obs_data_release(priv);
}

// One menu: OBS's scene actions, then the organizer's group actions.
QMenu *SceneOrganizer::buildMenu(QTreeWidgetItem *item, QWidget *parent)
{
	QTreeWidgetItem *group = isGroup(item) ? item : (item ? item->parent() : nullptr);
	auto *menu = new QMenu(parent);
	const QString atKey = item ? item->data(0, KeyRole).toString() : QString();
	connect(menu->addAction(T("DockDeck.Obs.AddScene")), &QAction::triggered, this,
		[this, atKey]() { addScene(atKey.isEmpty() ? nullptr : findKey(atKey)); });
	if (item && !isGroup(item)) {
		obs_source_t *scene = obs_get_source_by_uuid(atKey.toUtf8().constData());
		if (scene)
			addSceneActions(menu, item, scene);
		obs_source_release(scene);
	}
	menu->addSeparator();
	connect(menu->addAction(T("DockDeck.Organizer.AddGroup")), &QAction::triggered, this, [this]() {
		tree_->editItem(addGroup(QString()));
		save();
	});
	if (group) {
		connect(menu->addAction(T("DockDeck.Organizer.AddSubgroup").arg(group->text(0))), &QAction::triggered,
			this, [this, group]() {
				tree_->editItem(addGroup(QString(), group));
				save();
			});
	}
	if (isGroup(item)) {
		menu->addSeparator();
		connect(menu->addAction(T("DockDeck.Organizer.RenameGroup")), &QAction::triggered, this,
			[this, item]() { tree_->editItem(item); });
		connect(menu->addAction(T("DockDeck.Organizer.DeleteGroup")), &QAction::triggered, this,
			[this, item]() { deleteGroup(item); });
	}
	menu->addSeparator();
	connect(menu->addAction(T("DockDeck.Organizer.ExpandAll")), &QAction::triggered, tree_, &QTreeView::expandAll);
	connect(menu->addAction(T("DockDeck.Organizer.CollapseAll")), &QAction::triggered, tree_,
		&QTreeView::collapseAll);
	return menu;
}

void SceneOrganizer::showMenu(const QPoint &pos)
{
	QMenu *menu = buildMenu(tree_->itemAt(pos), this);
	menu->exec(tree_->viewport()->mapToGlobal(pos));
	delete menu;
}
