// SPDX-License-Identifier: MIT OR Apache-2.0
#include "agent_console.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <dlfcn.h>

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_object.h>
#include <logos_types.h>

namespace {

constexpr const char *kModule = "agent";

/// The transport gives up at 20 s by default. Every call this window makes is
/// asynchronous, so a long one does not freeze the window — which matters for
/// exactly one of them: an above-threshold `wallet.send` blocks inside the
/// module until the owner answers or the wait times out, and the owner it is
/// waiting for is the person looking at this window. A synchronous call there
/// would block the event loop that has to deliver `ownerApprovalRequested`.
constexpr int kCallTimeoutMs = 20000;

QString compact(const QString &s, int limit = 400)
{
    QString flat = s;
    flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return flat.size() > limit ? flat.left(limit) + QStringLiteral(" …") : flat;
}

} // namespace

AgentConsole::AgentConsole(LogosAPI *api, QWidget *parent)
    : QWidget(parent), api_(api)
{
    setObjectName(QStringLiteral("lp0008AgentConsole"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("LP-0008 — Agent owner console"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *subtitle = new QLabel(
        QStringLiteral("Every button below is one call on the loaded <b>agent</b> core module, "
                       "over Logos Core's own transport. Nothing here reimplements the agent."),
        this);
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    stateLabel_ = new QLabel(QStringLiteral("not connected"), this);
    stateLabel_->setWordWrap(true);
    root->addWidget(stateLabel_);

    // ---- lifecycle -------------------------------------------------------
    auto *lifecycle = new QHBoxLayout();
    auto *connectButton = new QPushButton(QStringLiteral("Connect"), this);
    auto *startButton = new QPushButton(QStringLiteral("Start"), this);
    auto *stopButton = new QPushButton(QStringLiteral("Stop"), this);
    auto *statusButton = new QPushButton(QStringLiteral("Status"), this);
    auto *skillsButton = new QPushButton(QStringLiteral("Skills"), this);
    lifecycle->addWidget(connectButton);
    lifecycle->addWidget(startButton);
    lifecycle->addWidget(stopButton);
    lifecycle->addWidget(statusButton);
    lifecycle->addWidget(skillsButton);
    lifecycle->addStretch();
    root->addLayout(lifecycle);

    // ---- binding ---------------------------------------------------------
    auto *binding = new QGridLayout();
    binding->addWidget(new QLabel(QStringLiteral("Owner address"), this), 0, 0);
    ownerEdit_ = new QLineEdit(
        QStringLiteral("0x00000000000000000000000000000000000000a9"), this);
    binding->addWidget(ownerEdit_, 0, 1);
    binding->addWidget(new QLabel(QStringLiteral("Policy hash (32 bytes hex)"), this), 1, 0);
    policyEdit_ = new QLineEdit(QString(64, QLatin1Char('a')), this);
    binding->addWidget(policyEdit_, 1, 1);
    auto *configureButton = new QPushButton(QStringLiteral("Configure (once)"), this);
    binding->addWidget(configureButton, 1, 2);
    binding->setColumnStretch(1, 1);
    root->addLayout(binding);

    // ---- skills ----------------------------------------------------------
    root->addWidget(new QLabel(QStringLiteral("Agent Card — skills the loaded module publishes"), this));
    skillList_ = new QListWidget(this);
    skillList_->setMinimumHeight(120);
    root->addWidget(skillList_, 1);

    auto *invokeRow = new QHBoxLayout();
    skillEdit_ = new QLineEdit(QStringLiteral("meta.skills"), this);
    skillEdit_->setPlaceholderText(QStringLiteral("skill name"));
    paramsEdit_ = new QLineEdit(QStringLiteral("{}"), this);
    paramsEdit_->setPlaceholderText(QStringLiteral("JSON parameters"));
    auto *invokeButton = new QPushButton(QStringLiteral("Invoke"), this);
    invokeRow->addWidget(skillEdit_, 1);
    invokeRow->addWidget(paramsEdit_, 2);
    invokeRow->addWidget(invokeButton);
    root->addLayout(invokeRow);

    // ---- the owner channel ----------------------------------------------
    auto *rule = new QFrame(this);
    rule->setFrameShape(QFrame::HLine);
    root->addWidget(rule);

    auto *ownerTitle = new QLabel(QStringLiteral("Owner channel"), this);
    QFont ownerFont = ownerTitle->font();
    ownerFont.setBold(true);
    ownerTitle->setFont(ownerFont);
    root->addWidget(ownerTitle);

    approvalLabel_ = new QLabel(QStringLiteral("no spend is waiting for you"), this);
    approvalLabel_->setWordWrap(true);
    approvalLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(approvalLabel_);

    auto *verdictRow = new QHBoxLayout();
    approveButton_ = new QPushButton(QStringLiteral("Approve"), this);
    denyButton_ = new QPushButton(QStringLiteral("Deny"), this);
    approveButton_->setEnabled(false);
    denyButton_->setEnabled(false);
    verdictRow->addWidget(approveButton_);
    verdictRow->addWidget(denyButton_);
    verdictRow->addStretch();
    root->addLayout(verdictRow);

    // ---- the owner channel over Logos Messaging ---------------------------
    //
    // The panel above reaches the module that this app instance loaded. This
    // one reaches an agent in ANOTHER app instance, over Logos Delivery, with
    // nothing between the two but the public relays. Both instances press
    // "Join Messaging"; the owner's presses "Watch as owner" as well, and from
    // then on a request minted in the other app appears here without anybody
    // pressing anything.
    auto *deliveryRule = new QFrame(this);
    deliveryRule->setFrameShape(QFrame::HLine);
    root->addWidget(deliveryRule);

    auto *messagingTitle =
        new QLabel(QStringLiteral("Owner channel over Logos Messaging — the other app"), this);
    messagingTitle->setFont(ownerFont);
    root->addWidget(messagingTitle);

    auto *accounts = new QGridLayout();
    accounts->addWidget(new QLabel(QStringLiteral("Owner account"), this), 0, 0);
    ownerAccountEdit_ =
        new QLineEdit(QStringLiteral("BzYks91aGenEmpDoowdi3UUUjjyww1eMPMzibhH2wLnu"), this);
    accounts->addWidget(ownerAccountEdit_, 0, 1);
    accounts->addWidget(new QLabel(QStringLiteral("Agent account"), this), 1, 0);
    agentAccountEdit_ =
        new QLineEdit(QStringLiteral("5Sa13NyNFsTqAj3AtdoQ7kzC6ZZJJN57AYqhNddHtjnZ"), this);
    accounts->addWidget(agentAccountEdit_, 1, 1);
    accounts->setColumnStretch(1, 1);
    root->addLayout(accounts);

    auto *messagingRow = new QHBoxLayout();
    auto *joinButton = new QPushButton(QStringLiteral("Join Messaging"), this);
    watchButton_ = new QPushButton(QStringLiteral("Watch as owner"), this);
    messagingRow->addWidget(joinButton);
    messagingRow->addWidget(watchButton_);
    messagingRow->addStretch();
    root->addLayout(messagingRow);

    messagingLabel_ = new QLabel(QStringLiteral("no node: this app has joined nothing"), this);
    messagingLabel_->setWordWrap(true);
    messagingLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(messagingLabel_);

    deliveryApprovalLabel_ =
        new QLabel(QStringLiteral("nothing has arrived over Logos Messaging"), this);
    deliveryApprovalLabel_->setWordWrap(true);
    deliveryApprovalLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(deliveryApprovalLabel_);

    auto *deliveryVerdictRow = new QHBoxLayout();
    deliveryApproveButton_ = new QPushButton(QStringLiteral("Approve over Delivery"), this);
    deliveryDenyButton_ = new QPushButton(QStringLiteral("Deny over Delivery"), this);
    deliveryApproveButton_->setEnabled(false);
    deliveryDenyButton_->setEnabled(false);
    deliveryVerdictRow->addWidget(deliveryApproveButton_);
    deliveryVerdictRow->addWidget(deliveryDenyButton_);
    deliveryVerdictRow->addStretch();
    root->addLayout(deliveryVerdictRow);

    // ---- transcript ------------------------------------------------------
    transcript_ = new QPlainTextEdit(this);
    transcript_->setReadOnly(true);
    transcript_->setMinimumHeight(160);
    transcript_->setLineWrapMode(QPlainTextEdit::NoWrap);
    root->addWidget(transcript_, 1);

    connect(connectButton, &QPushButton::clicked, this, &AgentConsole::onConnect);
    connect(configureButton, &QPushButton::clicked, this, &AgentConsole::onConfigure);
    connect(startButton, &QPushButton::clicked, this, &AgentConsole::onStart);
    connect(stopButton, &QPushButton::clicked, this, &AgentConsole::onStop);
    connect(statusButton, &QPushButton::clicked, this, &AgentConsole::onRefreshStatus);
    connect(skillsButton, &QPushButton::clicked, this, &AgentConsole::onRefreshSkills);
    connect(invokeButton, &QPushButton::clicked, this, &AgentConsole::onInvoke);
    connect(approveButton_, &QPushButton::clicked, this, &AgentConsole::onApprove);
    connect(denyButton_, &QPushButton::clicked, this, &AgentConsole::onDeny);
    connect(joinButton, &QPushButton::clicked, this, &AgentConsole::onJoinMessaging);
    connect(watchButton_, &QPushButton::clicked, this, &AgentConsole::onWatchAsOwner);
    connect(deliveryApproveButton_, &QPushButton::clicked, this,
            &AgentConsole::onDeliveryApprove);
    connect(deliveryDenyButton_, &QPushButton::clicked, this, &AgentConsole::onDeliveryDeny);

    // Two timers, both stopped. The first is the "in real time" half of the
    // criterion: once this window is watching, a request minted in the other
    // app appears here on its own. The second exists because joining the public
    // network takes tens of seconds, and a window that says nothing for those
    // seconds reads as one that did nothing.
    ownerPollTimer_ = new QTimer(this);
    ownerPollTimer_->setInterval(1000);
    connect(ownerPollTimer_, &QTimer::timeout, this, &AgentConsole::pollOwnerChannel);
    deliveryStateTimer_ = new QTimer(this);
    deliveryStateTimer_->setInterval(2000);
    connect(deliveryStateTimer_, &QTimer::timeout, this, &AgentConsole::pollDeliveryState);
    // Selecting a skill fills the invoke row from the schema the module itself
    // published for it, so the parameter names in the box are the module's and
    // not this window's idea of them.
    connect(skillList_, &QListWidget::currentTextChanged, this,
            [this](const QString &text) {
                const QString name = text.section(QLatin1Char('('), 0, 0);
                if (name.isEmpty()) {
                    return;
                }
                skillEdit_->setText(name);
                paramsEdit_->setText(templateFor(name));
            });

    log(QStringLiteral("LP-0008 owner console opened. Press Connect."));

    // Connect on the next event-loop turn rather than in the constructor: the
    // host is still inside createWidget() here, the widget is not parented yet,
    // and loading a core module spawns a process. Deferring keeps the click
    // that opened this window from appearing to hang.
    QPointer<AgentConsole> self(this);
    QTimer::singleShot(0, this, [self]() {
        if (self) {
            self->onConnect();
        }
    });
}

AgentConsole::~AgentConsole() = default;

void AgentConsole::log(const QString &line)
{
    transcript_->appendPlainText(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz "))
        + line);
    // Pinned to the newest line. Without this the pane keeps showing whatever
    // was on screen when it filled up, which during an approval wait is the
    // moment before anything interesting happened.
    QScrollBar *bar = transcript_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void AgentConsole::logCall(const QString &method, const QString &detail)
{
    log(QStringLiteral("→ %1.%2(%3)")
            .arg(QString::fromUtf8(kModule), method, compact(detail, 160)));
}

bool AgentConsole::resultOk(const QVariant &reply, QString *why)
{
    if (!reply.canConvert<LogosResult>()) {
        *why = QStringLiteral("no LogosResult came back (got %1)")
                   .arg(QString::fromUtf8(reply.typeName() ? reply.typeName() : "nothing"));
        return false;
    }
    const LogosResult result = reply.value<LogosResult>();
    if (!result.success) {
        *why = result.error.toString();
    }
    return result.success;
}

bool AgentConsole::ensureModuleLoaded(QString *why)
{
    using GetModules = char **(*)();
    using LoadModule = int (*)(const char *, bool);

    // RTLD_DEFAULT: the runtime is already in this process — Basecamp links
    // liblogos_core — so this resolves the host's own copy rather than a second
    // one. Anything else would be a different runtime with a different module
    // registry and a different TokenManager.
    auto loaded = reinterpret_cast<GetModules>(dlsym(RTLD_DEFAULT, "logos_core_get_loaded_modules"));
    auto known = reinterpret_cast<GetModules>(dlsym(RTLD_DEFAULT, "logos_core_get_known_modules"));
    auto load = reinterpret_cast<LoadModule>(dlsym(RTLD_DEFAULT, "logos_core_load_module"));
    if (!loaded || !known || !load) {
        *why = QStringLiteral("this process has no Logos Core runtime "
                              "(logos_core_load_module is not resolvable)");
        return false;
    }

    QStringList loadedNames;
    if (char **names = loaded()) {
        for (char **p = names; *p; ++p) {
            loadedNames << QString::fromUtf8(*p);
        }
    }
    if (loadedNames.contains(QString::fromUtf8(kModule))) {
        log(QStringLiteral("<- already loaded: %1").arg(loadedNames.join(QStringLiteral(" "))));
        return true;
    }

    QStringList knownNames;
    if (char **names = known()) {
        for (char **p = names; *p; ++p) {
            knownNames << QString::fromUtf8(*p);
        }
    }
    log(QStringLiteral("<- known modules: %1").arg(knownNames.join(QStringLiteral(" "))));
    if (!knownNames.contains(QString::fromUtf8(kModule))) {
        *why = QStringLiteral("the runtime does not know a module called '%1'. Install "
                              "agent.lgx into the user modules directory first — see "
                              "docs/basecamp.md.")
                   .arg(QString::fromUtf8(kModule));
        return false;
    }

    logCall(QStringLiteral("logos_core_load_module"), QStringLiteral("with_dependencies=true"));
    if (load(kModule, true) != 1) {
        *why = QStringLiteral("logos_core_load_module('%1') refused. Basecamp's stderr carries "
                              "the reason — a Qt version above the host's is the usual one.")
                   .arg(QString::fromUtf8(kModule));
        return false;
    }
    log(QStringLiteral("<- logos_core_load_module reported success"));
    return true;
}

void AgentConsole::onConnect()
{
    if (!api_) {
        stateLabel_->setText(QStringLiteral("the host handed this plugin no LogosAPI — "
                                            "nothing can be reached"));
        log(QStringLiteral("!! createWidget received a null LogosAPI"));
        return;
    }

    QString why;
    if (!ensureModuleLoaded(&why)) {
        stateLabel_->setText(why);
        log(QStringLiteral("!! %1").arg(why));
        return;
    }

    client_ = api_->getClient(QString::fromUtf8(kModule));
    if (!client_) {
        stateLabel_->setText(QStringLiteral("the SDK handed out no client for '%1'")
                                 .arg(QString::fromUtf8(kModule)));
        return;
    }
    log(QStringLiteral("<- got a LogosAPIClient for '%1'").arg(QString::fromUtf8(kModule)));

    // The answer path gets its own connection, opened here rather than at the
    // moment it is needed: a connection dialled while the module is blocked
    // would have to be accepted by the loop that is blocked. See the note on
    // answerApi_ in the header for the measurement this is here for.
    if (!answerApi_) {
        answerApi_ = new LogosAPI(QStringLiteral("agent_owner_console"), this);
        answerClient_ = answerApi_->getClient(QString::fromUtf8(kModule));
        log(answerClient_
                ? QStringLiteral("<- opened a second connection for the owner's verdict, so an "
                                 "answer does not queue behind the spend it answers")
                : QStringLiteral("!! no second client: a verdict will have to wait for the "
                                 "spend it answers to give up"));
    }

    // The owner channel's inbound half. Subscribed once — a second subscription
    // would show every request twice and make one payment look like two.
    if (!subscribed_) {
        object_ = client_->requestObject(QString::fromUtf8(kModule), Timeout(kCallTimeoutMs));
        if (!object_) {
            log(QStringLiteral("!! no LogosObject for '%1': approval requests will not arrive")
                    .arg(QString::fromUtf8(kModule)));
        } else {
            QPointer<AgentConsole> self(this);
            client_->onEvent(
                object_, QStringLiteral("ownerApprovalRequested"),
                [self](const QString &name, const QVariantList &args) {
                    if (!self) {
                        return;
                    }
                    const QString requestJson = args.value(0).toString();
                    const qint64 attempt = args.value(1).toLongLong();
                    const QJsonObject request =
                        QJsonDocument::fromJson(requestJson.toUtf8()).object();
                    const QString id = request.value(QStringLiteral("id")).toString();

                    // Marshal onto the GUI thread before touching widgets: the
                    // event arrives on whichever thread the transport delivered
                    // it on, and a widget touched off the GUI thread is a crash
                    // that happens later and elsewhere.
                    QMetaObject::invokeMethod(
                        self, [self, name, requestJson, attempt, id, request]() {
                            if (!self) {
                                return;
                            }
                            self->log(QStringLiteral("<= event %1 (attempt %2): %3")
                                          .arg(name)
                                          .arg(attempt)
                                          .arg(compact(requestJson, 200)));
                            self->pendingRequestId_ = id;
                            self->approvalLabel_->setText(
                                QStringLiteral(
                                    "<b>The agent is asking you to approve a spend.</b><br>"
                                    "id: %1<br>recipient: %2<br>amount: %3<br>reason: %4<br>"
                                    "attempt %5")
                                    .arg(id.isEmpty() ? QStringLiteral("(none)") : id,
                                         request.value(QStringLiteral("recipient")).toString(),
                                         request.value(QStringLiteral("amount")).toString(),
                                         request.value(QStringLiteral("reason")).toString())
                                    .arg(attempt));
                            self->approveButton_->setEnabled(!id.isEmpty());
                            self->denyButton_->setEnabled(!id.isEmpty());
                        },
                        Qt::QueuedConnection);
                });
            subscribed_ = true;
            log(QStringLiteral("<- subscribed to ownerApprovalRequested — this window is now "
                               "the owner end of the channel"));
        }
    }

    stateLabel_->setText(QStringLiteral("connected to the loaded '%1' module")
                             .arg(QString::fromUtf8(kModule)));
    onRefreshStatus();
}

void AgentConsole::onConfigure()
{
    if (!client_) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    const QString owner = ownerEdit_->text().trimmed();
    const QString policy = policyEdit_->text().trimmed();
    logCall(QStringLiteral("configure"), owner + QStringLiteral(", ") + policy);

    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("configure"),
        QVariantList{QVariant(owner), QVariant(policy)},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            QString why;
            const bool ok = resultOk(reply, &why);
            self->log(ok ? QStringLiteral("<- configure accepted: the agent is bound to this "
                                          "owner and this policy anchor")
                         : QStringLiteral("<- configure refused: %1").arg(why));
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::onStart()
{
    if (!client_) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    logCall(QStringLiteral("start"));
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("start"), QVariantList{},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            QString why;
            const bool ok = resultOk(reply, &why);
            self->log(ok ? QStringLiteral("<- start accepted")
                         : QStringLiteral("<- start refused: %1").arg(why));
            if (ok) {
                self->onRefreshSkills();
                self->onRefreshStatus();
            }
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::onStop()
{
    if (!client_) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    logCall(QStringLiteral("stop"));
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("stop"), QVariantList{},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            QString why;
            const bool ok = resultOk(reply, &why);
            self->log(ok ? QStringLiteral("<- stop accepted")
                         : QStringLiteral("<- stop refused: %1").arg(why));
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::onRefreshStatus()
{
    if (!client_) {
        return;
    }
    logCall(QStringLiteral("status"));
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("status"), QVariantList{},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            const QString json = reply.toString();
            self->log(QStringLiteral("<- status: %1").arg(compact(json)));
            const QJsonObject status = QJsonDocument::fromJson(json.toUtf8()).object();
            if (status.isEmpty()) {
                self->stateLabel_->setText(
                    QStringLiteral("the module answered nothing readable to status()"));
                return;
            }
            // `started`, not `running`. The module's status() answers
            // {"configured":…,"owner":…,"policy":…,"started":…}, and an absent
            // key reads back as false — so a header that asked for "running"
            // said "no" against an agent that had just started and reported so.
            // Seen in the first run of this window inside Basecamp.
            self->stateLabel_->setText(
                QStringLiteral("started: %1 · configured: %2 · owner: %3")
                    .arg(status.value(QStringLiteral("started")).toBool()
                             ? QStringLiteral("yes")
                             : QStringLiteral("no"),
                         status.value(QStringLiteral("configured")).toBool()
                             ? QStringLiteral("yes")
                             : QStringLiteral("no"),
                         status.value(QStringLiteral("owner")).toString().isEmpty()
                             ? QStringLiteral("(none)")
                             : status.value(QStringLiteral("owner")).toString()));
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::onRefreshSkills()
{
    if (!client_) {
        return;
    }
    logCall(QStringLiteral("skills"));
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("skills"), QVariantList{},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            const QString json = reply.toString();
            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
            self->skillList_->clear();
            if (!doc.isArray()) {
                // An unstarted agent answers `{"error":"agent is not started"}`
                // here rather than `[]`, deliberately: an empty Agent Card is a
                // valid Agent Card, and the two must not look alike.
                self->log(QStringLiteral("<- skills is not a card: %1").arg(compact(json)));
                self->skillList_->addItem(compact(json, 200));
                return;
            }
            const QJsonArray entries = doc.array();
            self->schemas_.clear();
            for (const QJsonValue &entry : entries) {
                const QJsonObject skill = entry.toObject();
                const QString name = skill.value(QStringLiteral("name")).toString();
                // `parameters` is a JSON Schema object, so its own key count is
                // `type`/`properties`/`required` — 3, for every skill, which is
                // what this line printed before it read `properties`. The
                // parameter names are in there.
                const QJsonObject schema =
                    skill.value(QStringLiteral("parameters")).toObject();
                const QJsonObject properties =
                    schema.value(QStringLiteral("properties")).toObject();
                QStringList required;
                for (const QJsonValue &r : schema.value(QStringLiteral("required")).toArray()) {
                    required << r.toString();
                }
                QStringList rendered;
                for (auto it = properties.begin(); it != properties.end(); ++it) {
                    rendered << (required.contains(it.key())
                                     ? it.key() + QStringLiteral("*")
                                     : it.key());
                }
                self->schemas_.insert(name, schema);
                self->skillList_->addItem(
                    rendered.isEmpty()
                        ? QStringLiteral("%1()").arg(name)
                        : QStringLiteral("%1(%2)").arg(name, rendered.join(QStringLiteral(", "))));
            }
            // Two numbers, not one. `entries.size()` is what this window
            // parsed; the `"name":` count is what the module sent. They came
            // apart once — the card displayed 21 entries while the module's own
            // harness read 22 off the same binary — and one number could not
            // have told which side lost the skill.
            self->log(QStringLiteral("<- skills(): %1 entries parsed, %2 names in "
                                     "%3 bytes of reply (* = required)")
                          .arg(entries.size())
                          .arg(json.count(QStringLiteral("\"name\":")))
                          .arg(json.size()));
        },
        Timeout(kCallTimeoutMs));
}

QString AgentConsole::templateFor(const QString &skill) const
{
    const QJsonObject schema = schemas_.value(skill);
    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    QStringList required;
    for (const QJsonValue &r : schema.value(QStringLiteral("required")).toArray()) {
        required << r.toString();
    }
    // Only the required parameters go into the template. Filling in every
    // optional one with an empty string is not "the call with defaults" — it is
    // the call with each default explicitly overridden by nothing, and the
    // skills refuse a blank where they would have used their own value.
    if (required.isEmpty()) {
        return QStringLiteral("{}");
    }
    QJsonObject skeleton;
    for (const QString &name : required) {
        const QJsonObject property = properties.value(name).toObject();
        const QJsonValue type = property.value(QStringLiteral("type"));
        const QString typeName =
            type.isArray() ? type.toArray().first().toString() : type.toString();
        if (typeName == QLatin1String("integer") || typeName == QLatin1String("number")) {
            skeleton.insert(name, 0);
        } else if (typeName == QLatin1String("boolean")) {
            skeleton.insert(name, false);
        } else {
            skeleton.insert(name, QString());
        }
    }
    return QString::fromUtf8(QJsonDocument(skeleton).toJson(QJsonDocument::Compact));
}

void AgentConsole::onInvoke()
{
    if (!client_) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    const QString name = skillEdit_->text().trimmed();
    const QString params = paramsEdit_->text().trimmed();
    logCall(QStringLiteral("invoke"), name + QStringLiteral(", ") + params);

    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("invoke"),
        QVariantList{QVariant(name), QVariant(params.isEmpty() ? QStringLiteral("{}") : params)},
        [self, name](QVariant reply) {
            if (!self) {
                return;
            }
            self->log(QStringLiteral("<- invoke(%1): %2")
                          .arg(name, compact(reply.toString(), 600)));
        },
        Timeout(kCallTimeoutMs));
}

// ---------------------------------------------------------------------------
// The owner channel over Logos Messaging: the half that reaches another app
// ---------------------------------------------------------------------------

void AgentConsole::onJoinMessaging()
{
    if (!client_) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    const QString owner = ownerAccountEdit_->text().trimmed();
    const QString agent = agentAccountEdit_->text().trimmed();
    if (owner.isEmpty() || agent.isEmpty()) {
        log(QStringLiteral("!! both accounts are needed: the channel is named after the pair"));
        return;
    }

    // Three settings and no more. `owner_channel_account` and `agent_account`
    // are what the channel id is derived from — on BOTH sides, which is why
    // nothing is exchanged to agree on it — and `delivery on` is the two
    // strings that make a loaded module open a node of its own. Everything
    // after this crosses the plugin boundary as `invoke`, because that is all a
    // `ui` plugin can do.
    const QVector<QPair<QString, QString>> settings{
        {QStringLiteral("owner_channel_account"), owner},
        {QStringLiteral("agent_account"), agent},
        {QStringLiteral("delivery"), QStringLiteral("on")},
    };
    QPointer<AgentConsole> self(this);
    for (const auto &kv : settings) {
        const QString params = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("key"), kv.first},
                                      {QStringLiteral("value"), kv.second}})
                .toJson(QJsonDocument::Compact));
        logCall(QStringLiteral("invoke"), QStringLiteral("meta.configure, ") + params);
        client_->invokeRemoteMethodAsync(
            QString::fromUtf8(kModule), QStringLiteral("invoke"),
            QVariantList{QVariant(QStringLiteral("meta.configure")), QVariant(params)},
            [self, kv](QVariant reply) {
                if (!self) {
                    return;
                }
                self->log(QStringLiteral("<- meta.configure(%1): %2")
                              .arg(kv.first, compact(reply.toString(), 200)));
            },
            Timeout(kCallTimeoutMs));
    }
    messagingLabel_->setText(
        QStringLiteral("starting this app's own Logos Delivery node — joining the public "
                       "relays takes tens of seconds"));
    lastDeliveryState_.clear();
    deliveryStateTimer_->start();
    pollDeliveryState();
}

void AgentConsole::pollDeliveryState()
{
    if (!client_) {
        return;
    }
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("invoke"),
        QVariantList{QVariant(QStringLiteral("meta.status")), QVariant(QStringLiteral("{}"))},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            const QJsonObject status =
                QJsonDocument::fromJson(reply.toString().toUtf8()).object();
            const QJsonObject delivery =
                status.value(QStringLiteral("delivery")).toObject();
            const QString state = delivery.value(QStringLiteral("state")).toString();
            const QJsonObject frames = delivery.value(QStringLiteral("frames")).toObject();
            self->messagingLabel_->setText(
                QStringLiteral("node: %1 · relay frames %2 · channel frames %3 (decoded %4)%5")
                    .arg(state.isEmpty() ? QStringLiteral("unknown") : state)
                    .arg(frames.value(QStringLiteral("relay_seen")).toInt())
                    .arg(frames.value(QStringLiteral("channel_seen")).toInt())
                    .arg(frames.value(QStringLiteral("channel_decoded")).toInt())
                    .arg(delivery.value(QStringLiteral("error")).toString().isEmpty()
                             ? QString()
                             : QStringLiteral(" · %1")
                                   .arg(delivery.value(QStringLiteral("error")).toString())));
            // Logged on CHANGE only. This runs every two seconds and a
            // transcript that repeats itself is one nobody reads to the end.
            if (state != self->lastDeliveryState_) {
                self->lastDeliveryState_ = state;
                self->log(QStringLiteral("<- delivery node: %1").arg(state));
            }
            if (state == QLatin1String("ready") || state == QLatin1String("failed") ||
                state == QLatin1String("absent")) {
                self->deliveryStateTimer_->stop();
            }
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::onWatchAsOwner()
{
    if (!client_) {
        log(QStringLiteral("!! not connected"));
        return;
    }
    logCall(QStringLiteral("invoke"), QStringLiteral("owner.watch, {}"));
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("invoke"),
        QVariantList{QVariant(QStringLiteral("owner.watch")), QVariant(QStringLiteral("{}"))},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            const QString text = reply.toString();
            const QJsonObject answer = QJsonDocument::fromJson(text.toUtf8()).object();
            self->log(QStringLiteral("<- owner.watch: %1").arg(compact(text, 300)));
            if (!answer.value(QStringLiteral("ok")).toBool()) {
                self->deliveryApprovalLabel_->setText(
                    QStringLiteral("not watching: %1")
                        .arg(answer.value(QStringLiteral("error")).toString()));
                return;
            }
            self->watching_ = true;
            self->deliveryApprovalLabel_->setText(
                QStringLiteral("watching %1 — waiting for the agent in the other app to ask")
                    .arg(answer.value(QStringLiteral("channel")).toString()));
            self->log(QStringLiteral("<- this window is the OWNER end of a Logos Messaging "
                                     "channel; the agent is in another app instance"));
            self->ownerPollTimer_->start();
            self->pollOwnerChannel();
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::pollOwnerChannel()
{
    if (!client_ || !watching_) {
        return;
    }
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("invoke"),
        QVariantList{QVariant(QStringLiteral("owner.pending")), QVariant(QStringLiteral("{}"))},
        [self](QVariant reply) {
            if (!self) {
                return;
            }
            const QString text = reply.toString();
            const QJsonObject answer = QJsonDocument::fromJson(text.toUtf8()).object();
            if (!answer.value(QStringLiteral("ok")).toBool()) {
                self->log(QStringLiteral("<- owner.pending: %1").arg(compact(text, 200)));
                return;
            }
            const QJsonArray waiting = answer.value(QStringLiteral("pending")).toArray();
            if (waiting.isEmpty()) {
                self->deliveryRequestId_.clear();
                self->deliveryApproveButton_->setEnabled(false);
                self->deliveryDenyButton_->setEnabled(false);
                return;
            }
            const QJsonObject request = waiting.first().toObject();
            const QString id = request.value(QStringLiteral("id")).toString();
            self->deliveryRequestId_ = id;
            self->deliveryApproveButton_->setEnabled(true);
            self->deliveryDenyButton_->setEnabled(true);
            self->deliveryApprovalLabel_->setText(
                QStringLiteral(
                    "<b>An agent in another Logos app is asking you to approve a spend.</b><br>"
                    "id: %1<br>agent: %2<br>recipient: %3<br>amount: %4<br>"
                    "marker seed: %5<br>this app re-derived that seed from the terms: %6")
                    .arg(id, request.value(QStringLiteral("agent")).toString(),
                         request.value(QStringLiteral("recipient")).toString(),
                         request.value(QStringLiteral("amount")).toString(),
                         request.value(QStringLiteral("marker_seed")).toString(),
                         request.value(QStringLiteral("seed_verified")).toBool()
                             ? QStringLiteral("yes")
                             : QStringLiteral("NO")));
            // Once per request, not once per second.
            if (id != self->lastLoggedDeliveryId_) {
                self->lastLoggedDeliveryId_ = id;
                self->log(QStringLiteral("<= over Logos Messaging: %1").arg(compact(text, 400)));
            }
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::onDeliveryApprove()
{
    answerOverDelivery(QStringLiteral("approve"));
}

void AgentConsole::onDeliveryDeny()
{
    answerOverDelivery(QStringLiteral("deny"));
}

void AgentConsole::answerOverDelivery(const QString &decision)
{
    if (!client_ || deliveryRequestId_.isEmpty()) {
        log(QStringLiteral("!! nothing has arrived over Logos Messaging to answer"));
        return;
    }
    const QString id = deliveryRequestId_;
    const QString params = QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("id"), id},
                                  {QStringLiteral("decision"), decision}})
            .toJson(QJsonDocument::Compact));
    logCall(QStringLiteral("invoke"), QStringLiteral("owner.answer, ") + params);

    // The FIRST connection, deliberately, and this is where the two owner
    // channels differ. `approveSpend` has to reach a module that is blocked
    // inside the call it is answering, which is why that one needs a second
    // socket. Here the module being answered is in ANOTHER PROCESS in ANOTHER
    // APP: this call only hands a reply to this app's own node, and returns.
    QPointer<AgentConsole> self(this);
    client_->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("invoke"),
        QVariantList{QVariant(QStringLiteral("owner.answer")), QVariant(params)},
        [self, id, decision](QVariant reply) {
            if (!self) {
                return;
            }
            const QString text = reply.toString();
            const QJsonObject answer = QJsonDocument::fromJson(text.toUtf8()).object();
            self->log(QStringLiteral("<- owner.answer(%1, %2): %3")
                          .arg(id, decision, compact(text, 300)));
            if (!answer.value(QStringLiteral("ok")).toBool()) {
                return;
            }
            self->deliveryRequestId_.clear();
            self->deliveryApproveButton_->setEnabled(false);
            self->deliveryDenyButton_->setEnabled(false);
            self->deliveryApprovalLabel_->setText(
                QStringLiteral("answered '%1' for %2 over Logos Messaging — the agent is in "
                               "another app instance and there is no server between them")
                    .arg(decision, id));
        },
        Timeout(kCallTimeoutMs));
}

void AgentConsole::onApprove()
{
    answerApproval(QStringLiteral("approved"));
}

void AgentConsole::onDeny()
{
    answerApproval(QStringLiteral("denied"));
}

void AgentConsole::answerApproval(const QString &verdict)
{
    if (!client_ || pendingRequestId_.isEmpty()) {
        log(QStringLiteral("!! no spend is waiting for an answer"));
        return;
    }
    const QString id = pendingRequestId_;
    logCall(QStringLiteral("approveSpend"), id + QStringLiteral(", ") + verdict);

    // The second connection, when there is one. Falling back to the first is
    // not a silent downgrade: it still delivers the verdict, just not until the
    // spend has stopped waiting for it, and the transcript will show that.
    LogosAPIClient *answering = answerClient_ ? answerClient_ : client_;
    QPointer<AgentConsole> self(this);
    answering->invokeRemoteMethodAsync(
        QString::fromUtf8(kModule), QStringLiteral("approveSpend"),
        QVariantList{QVariant(id), QVariant(verdict)},
        [self, id, verdict](QVariant reply) {
            if (!self) {
                return;
            }
            QString why;
            const bool ok = resultOk(reply, &why);
            self->log(ok ? QStringLiteral("<- approveSpend(%1, %2) accepted").arg(id, verdict)
                         : QStringLiteral("<- approveSpend(%1, %2) refused: %3")
                               .arg(id, verdict, why));
            if (ok) {
                self->pendingRequestId_.clear();
                self->approveButton_->setEnabled(false);
                self->denyButton_->setEnabled(false);
                self->approvalLabel_->setText(
                    QStringLiteral("answered '%1' for %2").arg(verdict, id));
            }
        },
        Timeout(kCallTimeoutMs));
}
