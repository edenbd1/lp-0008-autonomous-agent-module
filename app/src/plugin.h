// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The Qt plugin object Basecamp loads for the LP-0008 owner console.
//
// Everything interesting is in AgentConsole; this file exists to satisfy the
// one interface the host casts to, and to hand the console the `LogosAPI*` the
// host constructs — which is the only thing in the process that can reach the
// `agent` core module, because a core module runs in its own `logos_host`
// process and is reached over the runtime's transport, not by a function call.

#pragma once

#include <QObject>
#include <QString>
#include <QWidget>

class LogosAPI;
class AgentConsole;

// Basecamp's IComponent interface, declared here rather than included, so this
// plugin builds against a plain Qt with no Logos SDK headers on the path.
//
// This declaration is ABI-critical and is not a guess. It mirrors, slot for
// slot, the secondary vtable that Basecamp 0.2.2's own `main_ui` plugin emits
// for IComponent: offset-to-top, typeinfo, the two destructors, then
// createWidget and destroyWidget. An extra virtual here — a `name()` accessor,
// say — shifts every later slot, so the host would call the wrong function
// through a pointer that cast successfully. Confirmed against
// LogosBasecamp 0.2.2 and reused unchanged from LP-0002, whose plugin loads in
// that host.
class IComponent {
public:
    virtual ~IComponent() = default;
    virtual QWidget *createWidget(LogosAPI *api) = 0;
    virtual void destroyWidget(QWidget *widget) = 0;
};

// The IID is what `qobject_cast<IComponent*>` compares across the plugin
// boundary, so it has to be Basecamp's own. A private IID makes the cast return
// null and the host logs "Plugin does not implement IComponent" — the plugin
// loads, and nothing appears.
#define IComponent_IID "com.logos.component.IComponent"

Q_DECLARE_INTERFACE(IComponent, IComponent_IID)

class AgentUiPlugin : public QObject, public IComponent
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IComponent_IID FILE "metadata.json")
    Q_INTERFACES(IComponent)

public:
    explicit AgentUiPlugin(QObject *parent = nullptr);
    ~AgentUiPlugin() override;

    QWidget *createWidget(LogosAPI *api) override;
    void destroyWidget(QWidget *widget) override;

private:
    AgentConsole *console_ = nullptr;
};
