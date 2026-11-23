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

#include "deck-strip.hpp"

#include <obs-module.h>

#include <QAction>
#include <QMenu>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolButton>

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// QTabBar treats '&' as a mnemonic marker; double it so names show verbatim.
static QString escapeMnemonic(QString text)
{
	return text.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

DeckStrip::DeckStrip(QWidget *parent) : QToolBar(T("DockDeck.Title"), parent)
{
	// A stable objectName lets QMainWindow::saveState() record the bar
	// without warnings; DockDeck re-applies our own visibility setting after
	// every restoreState(), so saved decks can't hide or show it.
	setObjectName(QStringLiteral("DockDeckTabStrip"));
	setMovable(false);
	setFloatable(false);
	setAllowedAreas(Qt::TopToolBarArea);
	setContentsMargins(0, 0, 0, 0);

	// Keep the bar out of QMainWindow's right-click "toolbars/docks" menu;
	// visibility is controlled from Tools > Dock Deck instead.
	toggleViewAction()->setVisible(false);
	setContextMenuPolicy(Qt::PreventContextMenu);

	tabs_ = new QTabBar(this);
	tabs_->setObjectName(QStringLiteral("DockDeckTabs"));
	tabs_->setDocumentMode(true);
	tabs_->setDrawBase(false);
	tabs_->setExpanding(false);
	tabs_->setMovable(true);
	tabs_->setUsesScrollButtons(true);
	tabs_->setElideMode(Qt::ElideNone);
	tabs_->setFocusPolicy(Qt::NoFocus);
	tabs_->setContextMenuPolicy(Qt::CustomContextMenu);
	addWidget(tabs_);

	addButton_ = new QToolButton(this);
	addButton_->setObjectName(QStringLiteral("DockDeckAddButton"));
	addButton_->setText(QStringLiteral("+"));
	addButton_->setToolTip(T("DockDeck.NewDeck.Tooltip"));
	addButton_->setAutoRaise(true);
	addButton_->setFocusPolicy(Qt::NoFocus);
	addWidget(addButton_);

	connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
		const QString name = nameAt(index);
		if (!name.isEmpty())
			emit deckActivated(name);
	});
	connect(tabs_, &QTabBar::tabMoved, this, &DeckStrip::deckMoved);
	connect(tabs_, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
		const QString name = nameAt(index);
		if (!name.isEmpty())
			emit renameRequested(name);
	});
	connect(tabs_, &QWidget::customContextMenuRequested, this, &DeckStrip::showContextMenu);
	connect(addButton_, &QToolButton::clicked, this, &DeckStrip::newDeckRequested);
}

void DeckStrip::setDecks(const QStringList &names, const QString &active)
{
	// Programmatic rebuilds must never look like a user click.
	const QSignalBlocker blocker(tabs_);

	while (tabs_->count() > 0)
		tabs_->removeTab(0);

	for (const QString &name : names) {
		const int index = tabs_->addTab(escapeMnemonic(name));
		tabs_->setTabData(index, name); // the real name, unescaped
		tabs_->setTabToolTip(index, name);
	}

	const qsizetype activeIndex = names.indexOf(active);
	tabs_->setCurrentIndex(activeIndex < 0 ? -1 : static_cast<int>(activeIndex));
}

QString DeckStrip::nameAt(int index) const
{
	if (index < 0 || index >= tabs_->count())
		return QString();
	return tabs_->tabData(index).toString();
}

void DeckStrip::showContextMenu(const QPoint &pos)
{
	const QString name = nameAt(tabs_->tabAt(pos));
	QMenu menu(this);

	if (!name.isEmpty()) {
		connect(menu.addAction(T("DockDeck.Menu.Overwrite")), &QAction::triggered, this,
			[this, name]() { emit overwriteRequested(name); });
		connect(menu.addAction(T("DockDeck.Menu.Rename")), &QAction::triggered, this,
			[this, name]() { emit renameRequested(name); });
		connect(menu.addAction(T("DockDeck.Menu.Delete")), &QAction::triggered, this,
			[this, name]() { emit deleteRequested(name); });
		connect(menu.addAction(T("DockDeck.Menu.ExportOne")), &QAction::triggered, this,
			[this, name]() { emit exportRequested(name); });
		menu.addSeparator();
	}
	connect(menu.addAction(T("DockDeck.Menu.New")), &QAction::triggered, this, &DeckStrip::newDeckRequested);
	connect(menu.addAction(T("DockDeck.Menu.HideStrip")), &QAction::triggered, this, &DeckStrip::hideRequested);

	menu.exec(tabs_->mapToGlobal(pos));
}
