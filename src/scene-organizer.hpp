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

// Scene Organizer dock: the current scene collection's scenes with a search
// box and user-made folders ("groups"). Groups are Dock Deck's own data: they
// never rename, reorder or otherwise change OBS scenes. Scenes are remembered
// by their OBS uuid, so renames need no bookkeeping; removed scenes drop out
// and new ones appear at the bottom. Saved per scene collection in
// plugin_config/obs-dock-deck/organizer.json.
//
// The right-click menu also carries OBS's own Scenes-dock actions for the
// clicked scene (rename, duplicate, remove, filters, projectors, ...). OBS's
// real menu acts on the scene selected in OBS's list, and selecting one
// switches to it, so these are rebuilt on the frontend API with OBS's labels
// and confirmations instead (Add Scene, and Remove for the scene OBS has
// selected, still run OBS's own actions so they get OBS's undo).

#include <obs-frontend-api.h>
#include <obs.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTreeWidget>
#include <QWidget>

class QLineEdit;
class QMenu;

class OrganizerTree : public QTreeWidget {
	Q_OBJECT
public:
	using QTreeWidget::QTreeWidget;
signals:
	void dropped();

protected:
	void dropEvent(QDropEvent *event) override;
};

class SceneOrganizer : public QWidget {
	Q_OBJECT

public:
	explicit SceneOrganizer(QWidget *parent = nullptr);
	~SceneOrganizer() override;

	void onFrontendEvent(enum obs_frontend_event event);

	// Public for tests and the context menu.
	QTreeWidgetItem *addGroup(const QString &name, QTreeWidgetItem *parent = nullptr);
	void deleteGroup(QTreeWidgetItem *group); // its contents move up one level
	void setFilter(const QString &text);
	void refresh(); // sync with OBS's scene list
	void save();
	void loadCollection(); // (re)load groups for the current collection
	OrganizerTree *tree() const { return tree_; }
	QJsonArray toJson() const;

	// Scene actions (public for tests). Names are checked like OBS does:
	// returns "" when fine, else the locale key prefix of the problem.
	static QString nameProblem(obs_source_t *scene, const QString &name);
	bool renameScene(QTreeWidgetItem *item, const QString &name);
	bool duplicateScene(QTreeWidgetItem *item, const QString &name); // copy goes right below
	bool removeScene(QTreeWidgetItem *item);
	QMenu *buildMenu(QTreeWidgetItem *item, QWidget *parent);
	bool prompts = true; // false in tests: no dialogs

private:
	static void onSourceRename(void *data, calldata_t *cd);
	void activate(QTreeWidgetItem *item);
	void showMenu(const QPoint &pos);
	void markCurrent();
	void buildFrom(const QJsonArray &nodes, QTreeWidgetItem *parent);
	QJsonObject readFile() const;
	QTreeWidgetItem *findKey(const QString &key) const;
	void addScene(QTreeWidgetItem *at);
	void placeNew(const QSet<QString> &before, const QString &anchorKey);
	QString askSceneName(const QString &suggestion);
	bool warn(const QString &problem);
	void addSceneActions(QMenu *menu, QTreeWidgetItem *item, obs_source_t *scene);

	QLineEdit *search_ = nullptr;
	OrganizerTree *tree_ = nullptr;
	QString path_;
	QString collection_;
	bool ready_ = false;   // OBS finished loading and no collection switch is running
	bool syncing_ = false; // tree changes made by us, not by the user
	obs_weak_source_t *copiedFilters_ = nullptr;
};
