// SPDX-License-Identifier: MIT OR Apache-2.0
//
// The owner-facing surface for the LP-0008 agent, as a Basecamp window.
//
// What this is *not*: a second implementation of the agent. Every button here
// is one call on the loaded `agent` core module's published method table —
// `configure`, `start`, `skills`, `status`, `invoke`, `approveSpend` — issued
// over the same Logos Core transport `module/tests/logos_core_load_test.cpp`
// drives, and the same one Basecamp's own `main_ui` uses to reach
// `package_manager`. Nothing is computed here that the module does not compute.
//
// The owner channel is the pair at the bottom of that list, and it is the
// reason this window exists rather than a CLI: the module publishes
// `ownerApprovalRequested` every time a spend falls outside its anchored
// envelope, and answers `approveSpend(requestId, verdict)`. That channel is the
// RUNTIME's — an event out, a method back — and it reaches whatever loaded the
// module, which is this same app instance.
//
// The second panel is the one the criterion is actually about, and it is not
// that channel. "The owner can interact with the agent in real time from a
// separate Logos app instance using Logos Messaging, with no intermediary
// server": two Basecamps, each with its own user directory, its own loaded
// module and its own Logos Delivery node, talking over the public relays. The
// header of this file used to say the module's Delivery-backed `OwnerChannel`
// was "unreachable from here, because a plugin cannot be handed a
// `std::function` port across the boundary". The premise is right and the
// conclusion was wrong for the same reason it was wrong three times elsewhere
// in this repository: a module that cannot be HANDED a port can BUILD one.
// `module/src/delivery_runtime.cpp` builds it, `module/src/owner_skills.cpp`
// puts the owner's end of the channel behind three ordinary skills, and this
// window calls them with `invoke` like any other — which is all a `ui` plugin
// can do, and now all it needs.

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;

class LogosAPI;
class LogosAPIClient;
class LogosObject;

class AgentConsole : public QWidget
{
    Q_OBJECT

public:
    explicit AgentConsole(LogosAPI *api, QWidget *parent = nullptr);
    ~AgentConsole() override;

private slots:
    void onConnect();
    void onConfigure();
    void onStart();
    void onStop();
    void onRefreshStatus();
    void onRefreshSkills();
    void onInvoke();
    void onApprove();
    void onDeny();

    /// Bind this module to the two accounts the owner channel is named after
    /// and start its own Logos Delivery node: three `meta.configure` calls, and
    /// then a poll until the node reports `ready`. Pressed on BOTH instances —
    /// the agent's and the owner's — because the channel id is derived from the
    /// same pair on both sides and nothing is exchanged to agree on it.
    void onJoinMessaging();
    /// `owner.watch` — become the owner end of that channel. Pressed on the
    /// owner's instance only; the agent's end is opened by the module itself
    /// when a spend needs approving.
    void onWatchAsOwner();
    /// `owner.answer` — approve or deny over Logos Messaging, which on the
    /// owner's instance is the only route to the agent that exists at all.
    void onDeliveryApprove();
    void onDeliveryDeny();

private:
    /// Append one line to the transcript. Every call and every answer goes
    /// through here, so what the window shows is auditable against Basecamp's
    /// own stderr rather than being a summary of it.
    void log(const QString &line);
    void logCall(const QString &method, const QString &detail = QString());

    /// Promote the installed `agent` module from known to loaded.
    ///
    /// Basecamp auto-loads only `package_manager` and `package_downloader`;
    /// `logos_core_load_module` is what promotes anything else, and it is
    /// exported by the `liblogos_core` already in this process. Looked up with
    /// dlsym rather than linked, exactly as Basecamp's own `main_ui` resolves
    /// `logos_core_get_loaded_modules`. Returns false and says why on failure.
    bool ensureModuleLoaded(QString *why);

    /// Answer the pending approval with `verdict`. Shared by Approve and Deny
    /// so the two cannot drift apart.
    void answerApproval(const QString &verdict);

    /// `owner.answer` with `decision`. Shared by the two Delivery buttons, for
    /// the same reason.
    void answerOverDelivery(const QString &decision);

    /// One `owner.pending` call. Driven by a one-second timer once this window
    /// is watching, which is what makes the arrival of a request from the other
    /// app visible without anybody pressing anything — the "in real time" half
    /// of the criterion.
    void pollOwnerChannel();

    /// `meta.status`'s delivery block, polled while the node is coming up.
    /// Joining the public network takes tens of seconds and a window that said
    /// nothing for those seconds would read as a window that did nothing.
    void pollDeliveryState();

    /// A JSON skeleton for `skill`, built from the parameter schema the module
    /// published for it — so what lands in the box is the module's own idea of
    /// the call, not this window's.
    QString templateFor(const QString &skill) const;

    /// Read a LogosResult out of a reply, or explain why there was not one.
    /// A call that never arrived and a call that was refused are different
    /// things, and the transcript says which.
    static bool resultOk(const QVariant &reply, QString *why);

    LogosAPI *api_ = nullptr;
    LogosAPIClient *client_ = nullptr;
    LogosObject *object_ = nullptr;
    bool subscribed_ = false;

    /// A second LogosAPI, used for one thing: `approveSpend`.
    ///
    /// It exists because of a measurement, not a preference. While the module
    /// is inside `invoke("wallet.send")` waiting for the owner, its event loop
    /// is executing a call that arrived on the host's connection — and Qt
    /// disables a socket's read notifier for the duration of the handler it is
    /// running. So a verdict sent down *that* connection cannot be read until
    /// the call it is answering has already returned. Measured: the answer was
    /// clicked at T+23 s of a 60 s wait and reached the module at T+60.0 s,
    /// which is the moment the wait gave up.
    ///
    /// A second LogosAPI dials the module's registry again, so the module's
    /// QLocalServer hands it a second socket with a notifier of its own — one
    /// that is not inside a handler and is therefore still live while the first
    /// is blocked. That is what lets the answer in. Both share the process's
    /// TokenManager, so the capability token Basecamp obtained for this plugin
    /// covers it.
    LogosAPI *answerApi_ = nullptr;
    LogosAPIClient *answerClient_ = nullptr;

    QLineEdit *ownerEdit_ = nullptr;
    QLineEdit *policyEdit_ = nullptr;
    QLineEdit *skillEdit_ = nullptr;
    QLineEdit *paramsEdit_ = nullptr;
    QLabel *stateLabel_ = nullptr;
    QLabel *approvalLabel_ = nullptr;
    QListWidget *skillList_ = nullptr;
    QPlainTextEdit *transcript_ = nullptr;
    QPushButton *approveButton_ = nullptr;
    QPushButton *denyButton_ = nullptr;

    // ---- the second app's half -------------------------------------------
    QLineEdit *ownerAccountEdit_ = nullptr;
    QLineEdit *agentAccountEdit_ = nullptr;
    QLabel *messagingLabel_ = nullptr;
    QLabel *deliveryApprovalLabel_ = nullptr;
    QPushButton *watchButton_ = nullptr;
    QPushButton *deliveryApproveButton_ = nullptr;
    QPushButton *deliveryDenyButton_ = nullptr;
    QTimer *ownerPollTimer_ = nullptr;
    QTimer *deliveryStateTimer_ = nullptr;
    /// The request id `owner.pending` last offered for an answer, and the last
    /// one this window reported — so a poll running once a second says
    /// something when the answer changes and nothing when it does not.
    QString deliveryRequestId_;
    QString lastLoggedDeliveryId_;
    QString lastDeliveryState_;
    bool watching_ = false;

    /// The request id of the approval the owner is currently being asked
    /// about, and the whole request as the module published it. Empty when
    /// nothing is waiting — the Approve/Deny buttons are disabled then, because
    /// `approveSpend` refuses a request nobody is waiting on and a button that
    /// only ever produces that refusal is a lie about what the owner can do.
    QString pendingRequestId_;

    /// The parameter schema each listed skill published, keyed by skill name.
    QHash<QString, QJsonObject> schemas_;
};
