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

// Frontend hotkeys (Settings > Hotkeys):
//   "Dock Deck: Switch to <deck>"   one per deck
//   "Dock Deck: Next Deck", "Dock Deck: Previous Deck",
//   "Dock Deck: Toggle Lock on Current Deck"
//
// Per-deck hotkeys are named "DockDeck.Deck.<deck id>", so a rename only
// updates the description and the binding stays. Bindings are saved with
// obs_hotkey_save() into hotkeys.json in the plugin config folder (OBS does
// not save frontend hotkeys registered by plugins). Bindings of a deleted
// deck are kept in that file, so re-importing the same deck brings its
// hotkey back.

#include "dock-deck.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QMetaObject>
#include <QTimer>

static const char *kNextKey = "DockDeck.NextDeck";
static const char *kPrevKey = "DockDeck.PreviousDeck";
static const char *kLockKey = "DockDeck.ToggleLock";

static QString deckKey(const QString &id)
{
	return QStringLiteral("DockDeck.Deck.") + id;
}

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Runs on libobs' hotkey thread: hop to the UI thread. Hotkeys are
// unregistered on OBS exit, before DockDeck is destroyed.
static void hotkeyCallback(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<DockDeck *>(data);
	QMetaObject::invokeMethod(self, [self, id]() { self->onHotkey(id); }, Qt::QueuedConnection);
}

// libobs signal; may arrive on any thread. Coalesced save on the UI thread.
static void bindingsChanged(void *data, calldata_t *)
{
	auto *self = static_cast<DockDeck *>(data);
	QMetaObject::invokeMethod(self, [self]() { self->scheduleHotkeySave(); }, Qt::QueuedConnection);
}

void DockDeck::loadHotkeyBindings()
{
	if (hotkeyData_)
		return;
	if (!hotkeyPath_.isEmpty())
		hotkeyData_ = obs_data_create_from_json_file_safe(hotkeyPath_.toUtf8().constData(), "bak");
	if (!hotkeyData_)
		hotkeyData_ = obs_data_create();
	signal_handler_connect(obs_get_signal_handler(), "hotkey_bindings_changed", bindingsChanged, this);
}

obs_hotkey_id DockDeck::registerHotkey(const QString &key, const QString &description)
{
	const QByteArray k = key.toUtf8();
	const obs_hotkey_id id =
		obs_hotkey_register_frontend(k.constData(), description.toUtf8().constData(), hotkeyCallback, this);
	if (id == OBS_INVALID_HOTKEY_ID)
		return id;
	if (hotkeyData_) {
		obs_data_array_t *saved = obs_data_get_array(hotkeyData_, k.constData());
		if (saved) {
			obs_hotkey_load(id, saved);
			obs_data_array_release(saved);
		}
	}
	return id;
}

void DockDeck::syncHotkeys()
{
	if (!hotkeyData_)
		return; // not loaded yet (before OBS finished loading) or after exit

	if (nextHotkey_ == OBS_INVALID_HOTKEY_ID) {
		nextHotkey_ = registerHotkey(QString::fromLatin1(kNextKey), T("DockDeck.Hotkey.Next"));
		prevHotkey_ = registerHotkey(QString::fromLatin1(kPrevKey), T("DockDeck.Hotkey.Previous"));
		lockHotkey_ = registerHotkey(QString::fromLatin1(kLockKey), T("DockDeck.Hotkey.ToggleLock"));
	}

	// Decks that are gone: keep their bindings in the file, then unregister.
	bool removed = false;
	for (auto it = deckHotkeys_.begin(); it != deckHotkeys_.end();) {
		if (!store_.findById(it.key())) {
			obs_data_array_t *bindings = obs_hotkey_save(it.value());
			obs_data_set_array(hotkeyData_, deckKey(it.key()).toUtf8().constData(), bindings);
			obs_data_array_release(bindings);
			obs_hotkey_unregister(it.value());
			it = deckHotkeys_.erase(it);
			removed = true;
		} else {
			++it;
		}
	}

	// New decks get a hotkey; renamed ones get a new description.
	for (const Deck &deck : store_.decks) {
		const QString description = T("DockDeck.Hotkey.Switch").arg(deck.name);
		const auto it = deckHotkeys_.constFind(deck.id);
		if (it == deckHotkeys_.constEnd()) {
			const obs_hotkey_id id = registerHotkey(deckKey(deck.id), description);
			if (id != OBS_INVALID_HOTKEY_ID)
				deckHotkeys_.insert(deck.id, id);
		} else {
			obs_hotkey_set_description(it.value(), description.toUtf8().constData());
		}
	}
	if (removed)
		saveHotkeyBindings();
}

void DockDeck::scheduleHotkeySave()
{
	if (hotkeySaveQueued_)
		return;
	hotkeySaveQueued_ = true;
	QTimer::singleShot(250, this, [this]() {
		hotkeySaveQueued_ = false;
		saveHotkeyBindings();
	});
}

void DockDeck::saveHotkeyBindings()
{
	if (!hotkeyData_ || hotkeyPath_.isEmpty())
		return;

	auto store = [this](const QString &key, obs_hotkey_id id) {
		if (id == OBS_INVALID_HOTKEY_ID)
			return;
		obs_data_array_t *bindings = obs_hotkey_save(id);
		obs_data_set_array(hotkeyData_, key.toUtf8().constData(), bindings);
		obs_data_array_release(bindings);
	};
	store(QString::fromLatin1(kNextKey), nextHotkey_);
	store(QString::fromLatin1(kPrevKey), prevHotkey_);
	store(QString::fromLatin1(kLockKey), lockHotkey_);
	for (auto it = deckHotkeys_.constBegin(); it != deckHotkeys_.constEnd(); ++it)
		store(deckKey(it.key()), it.value());

	if (!obs_data_save_json_safe(hotkeyData_, hotkeyPath_.toUtf8().constData(), "tmp", "bak"))
		obs_log(LOG_WARNING, "could not write %s", hotkeyPath_.toUtf8().constData());
}

void DockDeck::unregisterHotkeys()
{
	if (hotkeyData_)
		signal_handler_disconnect(obs_get_signal_handler(), "hotkey_bindings_changed", bindingsChanged, this);
	for (const obs_hotkey_id id : deckHotkeys_)
		obs_hotkey_unregister(id);
	deckHotkeys_.clear();
	for (obs_hotkey_id *id : {&nextHotkey_, &prevHotkey_, &lockHotkey_}) {
		if (*id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(*id);
		*id = OBS_INVALID_HOTKEY_ID;
	}
}

void DockDeck::onHotkey(obs_hotkey_id id)
{
	if (!ready_)
		return;
	if (id == nextHotkey_) {
		stepDeck(+1, SwitchSource::Hotkey);
	} else if (id == prevHotkey_) {
		stepDeck(-1, SwitchSource::Hotkey);
	} else if (id == lockHotkey_) {
		if (const Deck *active = store_.find(store_.active))
			setDeckLocked(active->name, !active->locked);
	} else {
		for (auto it = deckHotkeys_.constBegin(); it != deckHotkeys_.constEnd(); ++it) {
			if (it.value() == id) {
				switchToDeckId(it.key(), SwitchSource::Hotkey);
				break;
			}
		}
	}
}
