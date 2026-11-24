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

// The optional Deck switch fade: a picture of the old docks, laid over the
// main window while the new Deck is restored underneath, then faded out.
// Only this overlay animates; the real docks and OBS's preview/program
// displays (native windows, drawn by libobs) are never touched, so the fade
// costs a single widget grab plus a few cheap repaints and can't slow down
// rendering or encoding.

#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QWidget>

class QMainWindow;
class QVariantAnimation;

class DeckFade : public QWidget {
	Q_OBJECT

public:
	// Grabs `area` (main window coordinates) of `window` and shows it on
	// top. Native windows (video displays) are cut out of the picture so the
	// live video shows through instead of a black box. Null if nothing could
	// be grabbed.
	static DeckFade *begin(QMainWindow *window, const QRect &area);

	// Fades out over `ms` and deletes itself.
	void start(int ms);
	// Ends at once (a new switch arrived mid-fade) and deletes itself.
	void snap();

	qreal opacity() const { return opacity_; }
	// True once finished or snapped (deletion follows from the event loop).
	bool done() const { return done_; }

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	DeckFade(QWidget *parent, const QPixmap &picture);

	QPixmap picture_;
	qreal opacity_ = 1.0;
	bool done_ = false;
	QPointer<QVariantAnimation> animation_;
};
