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

// Module entry points. All real work lives in DockDeck (dock-deck.cpp).

#include "deck-websocket.hpp"
#include "dock-deck.hpp"
#include "scene-organizer.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QPointer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// Owned by the OBS main window (Qt parent), so Qt deletes it with the window.
// QPointer turns into nullptr if that already happened before unload.
static QPointer<DockDeck> g_deck;
// The Scene Organizer dock's widget, owned by OBS once added as a dock.
static QPointer<SceneOrganizer> g_organizer;

const char *obs_module_name(void)
{
	return "Dock Deck";
}

const char *obs_module_description(void)
{
	return obs_module_text("DockDeck.Description");
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (g_organizer && event != OBS_FRONTEND_EVENT_EXIT)
		g_organizer->onFrontendEvent(event);
	if (!g_deck)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		g_deck->onFinishedLoading();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		g_deck->onExit();
		break;
	default:
		g_deck->onFrontendEvent(event);
		break;
	}
}

bool obs_module_load(void)
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window) {
		obs_log(LOG_ERROR, "no OBS main window; Dock Deck disabled");
		return true; // stay loaded but inert rather than failing OBS startup
	}

	g_deck = new DockDeck(main_window);

	auto *organizer = new SceneOrganizer();
	if (obs_frontend_add_dock_by_id("dock-deck-scene-organizer", obs_module_text("DockDeck.Organizer.Title"),
					organizer))
		g_organizer = organizer;
	else
		delete organizer;
	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

// obs-websocket registers its API during its own load, so vendors must be
// registered here, after every module has loaded.
void obs_module_post_load(void)
{
	if (g_deck)
		DeckWebsocket::registerVendor(g_deck);
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
