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
#include <QPainter>
#include <QPainterPath>
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

// A small padlock drawn in the theme's text colour, so it fits every OBS
// theme without shipping icon files. Unlocked = open shackle, drawn faint.
static QIcon lockIcon(bool locked, QColor color)
{
	if (!locked)
		color.setAlphaF(0.45f);
	QIcon icon;
	for (const int scale : {1, 2}) {
		QPixmap pm(12 * scale, 12 * scale);
		pm.fill(Qt::transparent);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		p.scale(scale, scale);

		QPen pen(color, 1.4);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		QPainterPath shackle;
		if (locked) {
			shackle.moveTo(3.5, 6.0);
			shackle.lineTo(3.5, 4.0);
			shackle.arcTo(QRectF(3.5, 1.0, 5.0, 5.0), 180.0, -180.0);
			shackle.lineTo(8.5, 6.0);
		} else {
			// Shackle lifted and swung open on the right.
			shackle.moveTo(3.5, 6.0);
			shackle.lineTo(3.5, 3.0);
			shackle.arcTo(QRectF(3.5, 0.5, 5.0, 5.0), 180.0, -180.0);
			shackle.lineTo(8.5, 4.0);
		}
		p.drawPath(shackle);

		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawRoundedRect(QRectF(2.0, 5.5, 8.0, 6.0), 1.2, 1.2);
		p.end();
		icon.addPixmap(pm);
	}
	return icon;
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

void DeckStrip::setDecks(const QVector<Tab> &tabs, const QString &active)
{
	tabInfo_ = tabs;

	// Programmatic rebuilds must never look like a user click.
	const QSignalBlocker blocker(tabs_);

	while (tabs_->count() > 0)
		tabs_->removeTab(0);

	int activeIndex = -1;
	for (const Tab &tab : tabs) {
		const QString &name = tab.name;
		const int index = tabs_->addTab(escapeMnemonic(name));
		tabs_->setTabData(index, name); // the real name, unescaped
		QString tip = name;
		if (!tab.linkedScene.isEmpty())
			tip += QStringLiteral("\n") +
			       T(tab.sceneMissing ? "DockDeck.Scene.TooltipMissing" : "DockDeck.Scene.Tooltip")
				       .arg(tab.linkedScene);
		tabs_->setTabToolTip(index, tip);
		if (name == active)
			activeIndex = index;

		// Padlock button on the tab: click toggles the deck's lock.
		const bool isLocked = tab.locked;
		auto *lock = new QToolButton(tabs_);
		lock->setObjectName(QStringLiteral("DockDeckLockButton"));
		lock->setAutoRaise(true);
		lock->setFocusPolicy(Qt::NoFocus);
		lock->setCursor(Qt::PointingHandCursor);
		lock->setIconSize(QSize(12, 12));
		lock->setFixedSize(16, 16);
		lock->setStyleSheet(
			QStringLiteral("QToolButton { border: none; padding: 0; background: transparent; }"));
		lock->setIcon(lockIcon(isLocked, tabs_->palette().color(QPalette::WindowText)));
		lock->setToolTip(isLocked ? T("DockDeck.Lock.Tooltip.Locked") : T("DockDeck.Lock.Tooltip.Unlocked"));
		connect(lock, &QToolButton::clicked, this, [this, name]() { emit lockToggled(name); });
		tabs_->setTabButton(index, QTabBar::RightSide, lock);
	}

	tabs_->setCurrentIndex(activeIndex);
}

const DeckStrip::Tab *DeckStrip::tabFor(const QString &name) const
{
	for (const Tab &tab : tabInfo_) {
		if (tab.name == name)
			return &tab;
	}
	return nullptr;
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
		const Tab *info = tabFor(name);
		const bool isLocked = info && info->locked;
		connect(menu.addAction(isLocked ? T("DockDeck.Menu.Unlock") : T("DockDeck.Menu.Lock")),
			&QAction::triggered, this, [this, name]() { emit lockToggled(name); });
		QAction *overwrite = menu.addAction(T("DockDeck.Menu.Overwrite"));
		overwrite->setEnabled(!isLocked); // unlock first; a lock means "keep as is"
		connect(overwrite, &QAction::triggered, this, [this, name]() { emit overwriteRequested(name); });
		connect(menu.addAction(T("DockDeck.Menu.Rename")), &QAction::triggered, this,
			[this, name]() { emit renameRequested(name); });
		connect(menu.addAction(T("DockDeck.Menu.Delete")), &QAction::triggered, this,
			[this, name]() { emit deleteRequested(name); });
		connect(menu.addAction(T("DockDeck.Menu.ExportPackOne")), &QAction::triggered, this,
			[this, name]() { emit packExportRequested(name); });
		menu.addSeparator();
		// Linked scene: selecting this deck switches OBS to it.
		const QString scene = currentScene ? currentScene() : QString();
		QAction *link = menu.addAction(scene.isEmpty()
						       ? T("DockDeck.Menu.LinkSceneNone")
						       : T("DockDeck.Menu.LinkScene").arg(escapeMnemonic(scene)));
		link->setEnabled(!scene.isEmpty() && !(info && info->linkedScene == scene && !info->sceneMissing));
		connect(link, &QAction::triggered, this, [this, name]() { emit linkSceneRequested(name); });
		if (info && !info->linkedScene.isEmpty()) {
			connect(menu.addAction(T("DockDeck.Menu.UnlinkScene").arg(escapeMnemonic(info->linkedScene))),
				&QAction::triggered, this, [this, name]() { emit unlinkSceneRequested(name); });
		}
		menu.addSeparator();
	}
	connect(menu.addAction(T("DockDeck.Menu.New")), &QAction::triggered, this, &DeckStrip::newDeckRequested);
	connect(menu.addAction(T("DockDeck.Menu.HideStrip")), &QAction::triggered, this, &DeckStrip::hideRequested);

	menu.exec(tabs_->mapToGlobal(pos));
}
