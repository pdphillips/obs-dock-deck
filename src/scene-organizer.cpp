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

#include <QDropEvent>
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
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled); // no drops into scenes
	return item;
}

void allItems(QTreeWidgetItem *root, QVector<QTreeWidgetItem *> *out)
{
	for (int i = 0; i < root->childCount(); ++i) {
		out->append(root->child(i));
		allItems(root->child(i), out);
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
		if (syncing_ || !isGroup(item))
			return;
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
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(scene);
	else
		obs_frontend_set_current_scene(scene);
	obs_source_release(scene);
}

// Selects (without switching) the scene OBS shows: Preview in Studio Mode.
void SceneOrganizer::markCurrent()
{
	obs_source_t *scene = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									 : obs_frontend_get_current_scene();
	const QString uuid = QString::fromUtf8(scene ? obs_source_get_uuid(scene) : "");
	obs_source_release(scene);
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

void SceneOrganizer::showMenu(const QPoint &pos)
{
	QTreeWidgetItem *item = tree_->itemAt(pos);
	QTreeWidgetItem *group = isGroup(item) ? item : (item ? item->parent() : nullptr);
	QMenu menu(this);
	connect(menu.addAction(T("DockDeck.Organizer.AddGroup")), &QAction::triggered, this, [this]() {
		tree_->editItem(addGroup(QString()));
		save();
	});
	if (group) {
		connect(menu.addAction(T("DockDeck.Organizer.AddSubgroup").arg(group->text(0))), &QAction::triggered,
			this, [this, group]() {
				tree_->editItem(addGroup(QString(), group));
				save();
			});
	}
	if (isGroup(item)) {
		menu.addSeparator();
		connect(menu.addAction(T("DockDeck.Organizer.RenameGroup")), &QAction::triggered, this,
			[this, item]() { tree_->editItem(item); });
		connect(menu.addAction(T("DockDeck.Organizer.DeleteGroup")), &QAction::triggered, this,
			[this, item]() { deleteGroup(item); });
	}
	menu.addSeparator();
	connect(menu.addAction(T("DockDeck.Organizer.ExpandAll")), &QAction::triggered, tree_, &QTreeView::expandAll);
	connect(menu.addAction(T("DockDeck.Organizer.CollapseAll")), &QAction::triggered, tree_,
		&QTreeView::collapseAll);
	menu.exec(tree_->viewport()->mapToGlobal(pos));
}
