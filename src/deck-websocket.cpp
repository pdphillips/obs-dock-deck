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

// Vendor requests (all answer with "ok": true/false and "error" on failure):
//   GetDecks        -> decks: [{name, id, locked, active}], activeName, activeId
//   GetCurrentDeck  -> name, id, locked
//   SetCurrentDeck  {name | id}           -> name, id
//   NextDeck / PreviousDeck               -> name, id (the deck switched to)
//   SetDeckLocked   {name | id, locked}   -> name, id, locked
// Event: DeckChanged {name, id, source}
//
// Requests arrive on obs-websocket's thread. They read DockDeck's
// thread-safe snapshot and queue any change to the UI thread instead of
// waiting for it, so a request can never deadlock OBS (e.g. at shutdown).

#include "deck-websocket.hpp"
#include "dock-deck.hpp"

#include <cassert>
#include <cstring>

#include <obs-module.h>
#include <plugin-support.h>

#include "obs-websocket-api.h"

#include <QMetaObject>

#include <atomic>

namespace {

std::atomic<DockDeck *> g_target{nullptr};
std::atomic<obs_websocket_vendor> g_vendor{nullptr};

using Snapshot = DockDeck::Snapshot;
using DeckInfo = DockDeck::DeckInfo;

void fail(obs_data_t *res, const char *error)
{
	obs_data_set_bool(res, "ok", false);
	obs_data_set_string(res, "error", error);
}

void putDeck(obs_data_t *o, const DeckInfo &deck)
{
	obs_data_set_string(o, "name", deck.name.toUtf8().constData());
	obs_data_set_string(o, "id", deck.id.toUtf8().constData());
	obs_data_set_bool(o, "locked", deck.locked);
}

// Common start of every request: DockDeck alive and OBS done loading.
bool begin(obs_data_t *res, DockDeck **target, Snapshot *snap)
{
	*target = g_target.load();
	if (!*target) {
		fail(res, "Dock Deck is not available");
		return false;
	}
	*snap = (*target)->snapshot();
	if (!snap->ready) {
		fail(res, "OBS is still loading");
		return false;
	}
	return true;
}

qsizetype indexOfId(const Snapshot &snap, const QString &id)
{
	for (qsizetype i = 0; i < snap.decks.size(); ++i) {
		if (snap.decks[i].id == id)
			return i;
	}
	return -1;
}

// {"id": ...} wins over {"name": ...}; names match exactly first, then
// case-insensitively (deck names are unique ignoring case).
qsizetype resolve(const Snapshot &snap, obs_data_t *req, obs_data_t *res)
{
	const QString id = QString::fromUtf8(obs_data_get_string(req, "id")).trimmed();
	const QString name = QString::fromUtf8(obs_data_get_string(req, "name")).trimmed();
	if (!id.isEmpty()) {
		const qsizetype i = indexOfId(snap, id);
		if (i < 0)
			fail(res, "No deck with that id");
		return i;
	}
	if (name.isEmpty()) {
		fail(res, "Give the deck as \"name\" or \"id\"");
		return -1;
	}
	for (qsizetype i = 0; i < snap.decks.size(); ++i) {
		if (snap.decks[i].name == name)
			return i;
	}
	for (qsizetype i = 0; i < snap.decks.size(); ++i) {
		if (snap.decks[i].name.compare(name, Qt::CaseInsensitive) == 0)
			return i;
	}
	fail(res, "No deck with that name");
	return -1;
}

// Queue a switch by id on the UI thread. Posted events of a deleted QObject
// are dropped by Qt, so this is safe even during teardown.
void queueSwitch(DockDeck *target, const QString &id)
{
	QMetaObject::invokeMethod(
		target, [target, id]() { target->switchToDeckId(id, SwitchSource::Websocket); }, Qt::QueuedConnection);
}

void getDecks(obs_data_t *, obs_data_t *res, void *)
{
	DockDeck *target;
	Snapshot snap;
	if (!begin(res, &target, &snap))
		return;
	obs_data_array_t *arr = obs_data_array_create();
	QString activeName;
	for (const DeckInfo &deck : snap.decks) {
		obs_data_t *o = obs_data_create();
		putDeck(o, deck);
		const bool active = deck.id == snap.activeId;
		obs_data_set_bool(o, "active", active);
		if (active)
			activeName = deck.name;
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(res, "decks", arr);
	obs_data_array_release(arr);
	obs_data_set_string(res, "activeName", activeName.toUtf8().constData());
	obs_data_set_string(res, "activeId", snap.activeId.toUtf8().constData());
	obs_data_set_bool(res, "ok", true);
}

void getCurrentDeck(obs_data_t *, obs_data_t *res, void *)
{
	DockDeck *target;
	Snapshot snap;
	if (!begin(res, &target, &snap))
		return;
	const qsizetype i = indexOfId(snap, snap.activeId);
	if (i < 0) {
		fail(res, "No active deck");
		return;
	}
	putDeck(res, snap.decks[i]);
	obs_data_set_bool(res, "ok", true);
}

void setCurrentDeck(obs_data_t *req, obs_data_t *res, void *)
{
	DockDeck *target;
	Snapshot snap;
	if (!begin(res, &target, &snap))
		return;
	const qsizetype i = resolve(snap, req, res);
	if (i < 0)
		return;
	queueSwitch(target, snap.decks[i].id);
	putDeck(res, snap.decks[i]);
	obs_data_set_bool(res, "ok", true);
}

void step(obs_data_t *res, int delta)
{
	DockDeck *target;
	Snapshot snap;
	if (!begin(res, &target, &snap))
		return;
	const qsizetype count = snap.decks.size();
	if (count == 0) {
		fail(res, "There are no decks");
		return;
	}
	const qsizetype current = qMax<qsizetype>(0, indexOfId(snap, snap.activeId));
	const qsizetype i = (current + delta + count) % count;
	queueSwitch(target, snap.decks[i].id);
	putDeck(res, snap.decks[i]);
	obs_data_set_bool(res, "ok", true);
}

void nextDeck(obs_data_t *, obs_data_t *res, void *)
{
	step(res, +1);
}

void previousDeck(obs_data_t *, obs_data_t *res, void *)
{
	step(res, -1);
}

void setDeckLocked(obs_data_t *req, obs_data_t *res, void *)
{
	DockDeck *target;
	Snapshot snap;
	if (!begin(res, &target, &snap))
		return;
	const qsizetype i = resolve(snap, req, res);
	if (i < 0)
		return;
	obs_data_item_t *item = obs_data_item_byname(req, "locked");
	const bool valid = item && obs_data_item_gettype(item) == OBS_DATA_BOOLEAN;
	obs_data_item_release(&item);
	if (!valid) {
		fail(res, "Give \"locked\": true or false");
		return;
	}
	const bool locked = obs_data_get_bool(req, "locked");
	const QString id = snap.decks[i].id;
	QMetaObject::invokeMethod(
		target,
		[target, id, locked]() {
			const DockDeck::Snapshot now = target->snapshot();
			for (const DeckInfo &deck : now.decks) {
				if (deck.id == id) {
					target->setDeckLocked(deck.name, locked);
					break;
				}
			}
		},
		Qt::QueuedConnection);
	DeckInfo result = snap.decks[i];
	result.locked = locked;
	putDeck(res, result);
	obs_data_set_bool(res, "ok", true);
}

} // namespace

void DeckWebsocket::registerVendor(DockDeck *deck)
{
	obs_websocket_vendor vendor = obs_websocket_register_vendor("dock-deck");
	if (!vendor) {
		obs_log(LOG_INFO, "obs-websocket not available; remote control disabled");
		return;
	}
	struct Request {
		const char *type;
		obs_websocket_request_callback_function fn;
	};
	const Request requests[] = {
		{"GetDecks", getDecks}, {"GetCurrentDeck", getCurrentDeck}, {"SetCurrentDeck", setCurrentDeck},
		{"NextDeck", nextDeck}, {"PreviousDeck", previousDeck},     {"SetDeckLocked", setDeckLocked},
	};
	for (const Request &r : requests) {
		if (!obs_websocket_vendor_register_request(vendor, r.type, r.fn, nullptr))
			obs_log(LOG_WARNING, "obs-websocket: could not register request %s", r.type);
	}
	g_target = deck;
	g_vendor = vendor;
	obs_log(LOG_INFO, "obs-websocket vendor 'dock-deck' registered");
}

void DeckWebsocket::emitDeckChanged(const QString &name, const QString &id, const char *source)
{
	obs_websocket_vendor vendor = g_vendor.load();
	if (!vendor)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "name", name.toUtf8().constData());
	obs_data_set_string(data, "id", id.toUtf8().constData());
	obs_data_set_string(data, "source", source);
	obs_websocket_vendor_emit_event(vendor, "DeckChanged", data);
	obs_data_release(data);
}

void DeckWebsocket::shutdown()
{
	g_target = nullptr;
	g_vendor = nullptr;
}
