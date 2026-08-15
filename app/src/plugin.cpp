// SPDX-License-Identifier: MIT OR Apache-2.0
#include "plugin.h"

#include "agent_console.h"

AgentUiPlugin::AgentUiPlugin(QObject *parent) : QObject(parent) {}

AgentUiPlugin::~AgentUiPlugin() = default;

QWidget *AgentUiPlugin::createWidget(LogosAPI *api)
{
    // `api` is the host's own LogosAPI — the same object main_ui prints as
    // `MainUIPlugin::createWidget: logosAPI: LogosAPI(0x…)`. It carries the
    // capability tokens Basecamp's PluginLoader obtained for this plugin, and
    // its `getClient()` is the only route to a module in another process.
    console_ = new AgentConsole(api);
    return console_;
}

void AgentUiPlugin::destroyWidget(QWidget *widget)
{
    if (widget == console_) {
        console_ = nullptr;
    }
    if (widget) {
        widget->deleteLater();
    }
}
