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

// The Deck pack checklist (export and import share it): a tree grouped as
// Decks / Scenes / Sources / Media with tri-state group boxes, a master
// "Select all / none" box, dependency auto-checking and, on import, a
// Rename / Skip / Replace choice for names that already exist.

#include "pack-core.hpp"

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

struct PackItem {
	enum Kind { Deck, Scene, Source, Media, Organizer };
	Kind kind = Deck;
	QString key; // "d:<deck>", "s:<scene or source>", "m:<media index>", "o:" organizer folders
	QString label;
	QString detail;
	QString tooltip;
	QStringList deps; // keys checked along with this one
	bool checked = false;
	bool collides = false; // import: the name exists here already
	qint64 size = 0;       // media
};

class PackDialog : public QDialog {
	Q_OBJECT

public:
	PackDialog(bool importing, const QString &title, const QString &intro, const QVector<PackItem> &items,
		   QWidget *parent);

	QSet<QString> checkedKeys() const;
	// Import, for colliding items: Rename (default), Skip or Replace.
	PackCore::Action action(const QString &key) const;

	// For tests.
	QTreeWidget *tree() const { return tree_; }
	QCheckBox *master() const { return master_; }
	QPushButton *okButton() const { return ok_; }
	QComboBox *actionBox(const QString &key) const { return actions_.value(key); }
	QTreeWidgetItem *item(const QString &key) const { return items_.value(key); }
	qint64 checkedBytes() const;

private:
	void onItemChanged(QTreeWidgetItem *item);
	void checkWithDeps(const QString &key);
	void sync();

	bool importing_;
	QTreeWidget *tree_ = nullptr;
	QCheckBox *master_ = nullptr;
	QLabel *total_ = nullptr;
	QPushButton *ok_ = nullptr;
	QVector<QTreeWidgetItem *> groups_;
	QHash<QString, QTreeWidgetItem *> items_;
	QHash<QString, QStringList> deps_;
	QHash<QString, qint64> sizes_;
	QHash<QString, QComboBox *> actions_;
	bool syncing_ = false;
};

// Big packs: warn above this many bytes of media.
constexpr qint64 kPackWarnBytes = 500ll << 20;
