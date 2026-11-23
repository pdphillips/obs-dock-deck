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

// obs-websocket vendor "dock-deck": remote control for Streamer.bot, SAMMI,
// Touch Portal, scripts... Uses obs-websocket's proc-handler API, so there
// is no link-time dependency; without obs-websocket everything is a no-op.

#include <QString>

class DockDeck;

namespace DeckWebsocket {
// Call from obs_module_post_load() (obs-websocket must be loaded first).
void registerVendor(DockDeck *deck);
// Vendor event "DeckChanged" {name, id, source}. UI thread.
void emitDeckChanged(const QString &name, const QString &id, const char *source);
// OBS is exiting: answer requests with an error and queue nothing.
void shutdown();
} // namespace DeckWebsocket
