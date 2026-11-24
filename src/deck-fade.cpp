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

#include "deck-fade.hpp"

#include <QMainWindow>
#include <QPaintEvent>
#include <QPainter>
#include <QVariantAnimation>

DeckFade::DeckFade(QWidget *parent, const QPixmap &picture) : QWidget(parent), picture_(picture)
{
	setObjectName(QStringLiteral("dockDeckFade"));
	// Purely visual: clicks and hovers go to the docks underneath.
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_NoSystemBackground);
	setFocusPolicy(Qt::NoFocus);
	setAutoFillBackground(false);
}

DeckFade *DeckFade::begin(QMainWindow *window, const QRect &area)
{
	if (!window || area.isEmpty())
		return nullptr;
	// Renders the widgets into a pixmap (no screen capture, so it works the
	// same on Wayland, X11 and Windows, and needs no permission).
	const QPixmap grabbed = window->grab(area);
	if (grabbed.isNull())
		return nullptr;
	// A grab has no alpha channel; holes need one.
	QImage picture = grabbed.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);

	// Video displays are native windows drawn by libobs; a grab only sees
	// black there. Clear them so the live picture stays visible.
	QPainter painter(&picture);
	painter.setCompositionMode(QPainter::CompositionMode_Clear);
	// Only the innermost ones: making a widget native can make its parents
	// native too, and those are ordinary widgets that must stay in the picture.
	auto isNative = [](const QWidget *w) {
		return w->isVisible() && !w->isWindow() && w->testAttribute(Qt::WA_NativeWindow);
	};
	for (QWidget *w : window->findChildren<QWidget *>()) {
		if (!isNative(w))
			continue;
		bool hasNativeChild = false;
		for (const QWidget *c : w->findChildren<QWidget *>())
			hasNativeChild = hasNativeChild || isNative(c);
		if (hasNativeChild)
			continue;
		const QRect r(w->mapTo(window, QPoint(0, 0)), w->size());
		painter.fillRect(r.translated(-area.topLeft()), Qt::transparent);
	}
	painter.end();

	auto *fade = new DeckFade(window, QPixmap::fromImage(picture));
	fade->setGeometry(area);
	fade->show();
	fade->raise();
	return fade;
}

void DeckFade::start(int ms)
{
	raise(); // restoreState() may have shown docks above us
	auto *animation = new QVariantAnimation(this);
	animation_ = animation;
	animation->setStartValue(1.0);
	animation->setEndValue(0.0);
	animation->setDuration(ms);
	animation->setEasingCurve(QEasingCurve::InOutQuad);
	connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
		opacity_ = value.toReal();
		update();
	});
	connect(animation, &QVariantAnimation::finished, this, &DeckFade::snap);
	animation->start();
}

void DeckFade::snap()
{
	done_ = true;
	if (animation_)
		animation_->stop();
	hide();
	deleteLater();
}

void DeckFade::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);
	painter.setOpacity(opacity_);
	painter.drawPixmap(event->rect(), picture_,
			   QRectF(QPointF(event->rect().topLeft()) * picture_.devicePixelRatio(),
				  QSizeF(event->rect().size()) * picture_.devicePixelRatio()));
}
