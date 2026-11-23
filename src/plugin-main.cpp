/*
DockDeck for OBS
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

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QLabel>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static constexpr const char *kDockId = "obs-dock-deck-main";

const char *obs_module_name(void)
{
	return "DockDeck";
}

const char *obs_module_description(void)
{
	return obs_module_text("DockDeck.Description");
}

static QWidget *create_placeholder_dock(QWidget *parent)
{
	auto *widget = new QWidget(parent);
	auto *layout = new QVBoxLayout(widget);
	auto *label = new QLabel(QString::fromUtf8(obs_module_text("DockDeck.Placeholder")), widget);
	label->setWordWrap(true);
	label->setAlignment(Qt::AlignCenter);
	layout->addWidget(label);
	return widget;
}

bool obs_module_load(void)
{
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QWidget *dock = create_placeholder_dock(main_window);

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("DockDeck.Title"), dock)) {
		obs_log(LOG_WARNING, "failed to add placeholder dock '%s'", kDockId);
		delete dock;
	}

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
