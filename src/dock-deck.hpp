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

// DockDeck owns the deck list, the tab strip, the Tools menu entry, and the
// one function every deck switch goes through: switchToDeck().

#include "deck-store.hpp"

#include <QHash>
#include <QMutex>
#include <QVector>
#include <obs.h>
#include <QObject>
#include <QPointer>
#include <QString>

class QAction;
class QDockWidget;
class QMainWindow;
class QMenu;
class DeckStrip;

// Where a switch request came from. Only tabs/menu/startup exist today; the
// rest are reserved so future triggers (and the optional fade) plug into
// switchToDeck() without changing its signature.
enum class SwitchSource { Tab, Menu, Startup, Import, Hotkey, Websocket, SceneLink, Api };

class DockDeck : public QObject {
	Q_OBJECT

public:
	explicit DockDeck(QMainWindow *mainWindow);
	~DockDeck() override;

	// THE deck switch. Safe to call from any thread (it hops to the UI
	// thread), safe to call before OBS finished loading (the request is
	// remembered and applied then), and never crashes on missing docks.
	// Returns false if the deck doesn't exist.
	bool switchToDeck(const QString &name, SwitchSource source);

	// Like switchToDeck(), by stable deck id. UI thread only.
	bool switchToDeckId(const QString &id, SwitchSource source);
	// Next (+1) / previous (-1) deck in tab order, wrapping around.
	bool stepDeck(int delta, SwitchSource source);
	// Lock or unlock a deck by name. UI thread only.
	bool setDeckLocked(const QString &name, bool locked);

	// Thread-safe copy of the deck list for remote requests, which arrive on
	// obs-websocket's thread and must never wait for the UI thread.
	struct DeckInfo {
		QString id;
		QString name;
		bool locked = false;
	};
	struct Snapshot {
		bool ready = false;
		QVector<DeckInfo> decks; // tab order
		QString activeId;
	};
	Snapshot snapshot() const;

	// Called from the OBS frontend event callback.
	void onFinishedLoading();
	void onExit();

	// Hotkey plumbing called from libobs callbacks (UI thread, queued).
	void onHotkey(obs_hotkey_id id);
	void scheduleHotkeySave();
	void saveHotkeyBindings();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// Snapshot the current docks. Entries from `previous` for docks that
	// don't exist right now are carried over, so a missing plugin's dock is
	// not forgotten just because the deck was auto-saved meanwhile.
	Deck capture(const QString &name, const Deck *previous = nullptr) const;
	void apply(const Deck &deck);

	// Placeholders for missing docks (see dock-deck.cpp for the full story).
	QHash<QString, QDockWidget *> realDocks() const;
	void syncPlaceholders(const Deck *deck);
	QDockWidget *createPlaceholder(const QString &name, const QString &title);
	void removePlaceholder(const QString &name);
	void scheduleAdopt();
	void adoptRealDocks();
	void setShowPlaceholders(bool enabled);
	void captureActive();

	// Deck locking (see dock-deck.cpp). applyLock() makes the docks match
	// the given deck's lock plus OBS's own Docks > Lock Docks.
	void applyLock(const Deck *deck);
	void updateLock();
	bool obsDocksLocked() const;
	void resetSeparatorCursor();
	void toggleLock(const QString &name);
	void setAllLocked(bool locked);
	void saveConfig();
	void publishSnapshot();

	// Hotkeys (deck-hotkeys.cpp): one "Switch to <deck>" per deck plus
	// Next / Previous / Toggle Lock. Bindings are stored per deck id.
	void loadHotkeyBindings();
	void syncHotkeys();
	void unregisterHotkeys();
	obs_hotkey_id registerHotkey(const QString &key, const QString &description);
	void scheduleRefresh();
	void refreshUi();
	void rebuildToolsMenu();

	// User actions (tab strip and Tools menu).
	void newDeckFromCurrent();
	void overwriteDeck(const QString &name);
	void renameDeck(const QString &name);
	void deleteDeck(const QString &name);
	void moveDeck(int from, int to);
	void exportDecks(const QString &onlyName = QString());
	void importDecks();
	void setTabStripVisible(bool visible);
	void setAutoSave(bool enabled);

	// Name prompt. With `importChosen` set, the dialog also offers
	// "Import Deck or Deck Collection..."; picking it returns an empty name
	// and sets *importChosen.
	QString askForName(const QString &title, const QString &initial, const QString &ignore,
			   bool *importChosen = nullptr);
	// Checklist of all decks for Export Deck Collection; empty = cancelled.
	QStringList pickDecksForExport();
	QString uniqueName(const QString &base) const;

	QMainWindow *main_;
	QPointer<DeckStrip> strip_;
	QPointer<QAction> toolsAction_;
	QMenu *toolsMenu_ = nullptr;

	DeckStore store_;
	QString configPath_;
	bool ready_ = false;    // OBS finished loading and the active deck was applied
	bool applying_ = false; // re-entrancy guard for switchToDeck()
	bool refreshQueued_ = false;
	bool adoptQueued_ = false;
	bool lockQueued_ = false;
	bool locked_ = false;             // active deck locked, or OBS's Lock Docks is on
	QPointer<QAction> obsLockAction_; // OBS's Docks > Lock Docks

	// Hotkeys: deck id -> hotkey, plus the three global ones.
	QHash<QString, obs_hotkey_id> deckHotkeys_;
	obs_hotkey_id nextHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id prevHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id lockHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_data_t *hotkeyData_ = nullptr; // saved bindings, keyed by hotkey name
	QString hotkeyPath_;
	bool hotkeySaveQueued_ = false;

	mutable QMutex snapshotMutex_;
	Snapshot snapshot_;

	// objectName -> placeholder dock currently standing in for it.
	QHash<QString, QPointer<QDockWidget>> placeholders_;
};
