// SPDX-License-Identifier: GPL-3.0-only
#include "McpPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "Application.h"
#include "mcp/McpService.h"
#include "settings/SettingsObject.h"
#include "ui/GuiUtil.h"

namespace {
constexpr int MIN_PORT = 1024;
constexpr int MAX_PORT = 65535;
constexpr auto SERVER_KEY = "crism-launcher";

enum TokenColumn { NameColumn, ScopesColumn, PolicyColumn, CreatedColumn, LastUsedColumn };

QString scopeLabel(Mcp::Scope scope)
{
    switch (scope) {
        case Mcp::Scope::Read:
            return McpPage::tr("Read: see instances, mods, settings and logs (never secrets)");
        case Mcp::Scope::Instances:
            return McpPage::tr("Instances: create, edit, copy and delete instances");
        case Mcp::Scope::Content:
            return McpPage::tr("Content: install, update and remove mods, packs, shaders and worlds");
        case Mcp::Scope::Launch:
            return McpPage::tr("Launch: start and stop the game");
        case Mcp::Scope::Settings:
            return McpPage::tr("Settings: change launcher and instance settings");
        case Mcp::Scope::Accounts:
            return McpPage::tr("Accounts: manage accounts (never reveals their tokens)");
        case Mcp::Scope::Files:
            return McpPage::tr("Files: read and write any file inside an instance folder");
        case Mcp::Scope::Commands:
            return McpPage::tr("Commands: run any program on this computer (also needs \"Allow agents to run commands\")");
    }
    return {};
}

QString policyLabel(Mcp::ConfirmPolicy policy)
{
    switch (policy) {
        case Mcp::ConfirmPolicy::AskDestructive:
            return McpPage::tr("Ask me before destructive changes");
        case Mcp::ConfirmPolicy::AskAllWrites:
            return McpPage::tr("Ask me before every change");
        case Mcp::ConfirmPolicy::NeverAsk:
            return McpPage::tr("Never ask");
    }
    return {};
}

QString formatTime(const QDateTime& time)
{
    if (!time.isValid()) {
        return McpPage::tr("Never");
    }
    return QLocale().toString(time.toLocalTime(), QLocale::ShortFormat);
}

// The token editor: name, scopes, confirmation policy. Returns false when cancelled.
bool editTokenDialog(QWidget* parent, const QString& title, QString& name, Mcp::Scopes& scopes, Mcp::ConfirmPolicy& policy, bool& chat)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);

    auto* form = new QFormLayout();
    auto* nameEdit = new QLineEdit(name, &dialog);
    nameEdit->setPlaceholderText(McpPage::tr("e.g. Claude Code"));
    form->addRow(McpPage::tr("Name:"), nameEdit);
    layout->addLayout(form);

    auto* scopeGroup = new QGroupBox(McpPage::tr("What may agents using this token do?"), &dialog);
    auto* scopeLayout = new QVBoxLayout(scopeGroup);
    QList<std::pair<Mcp::Scope, QCheckBox*>> boxes;
    for (auto scope : Mcp::allScopes()) {
        auto* box = new QCheckBox(scopeLabel(scope), scopeGroup);
        box->setChecked(scopes.testFlag(scope));
        scopeLayout->addWidget(box);
        boxes.emplace_back(scope, box);
    }
    layout->addWidget(scopeGroup);

    auto* policyForm = new QFormLayout();
    auto* policyBox = new QComboBox(&dialog);
    for (auto option : { Mcp::ConfirmPolicy::AskDestructive, Mcp::ConfirmPolicy::AskAllWrites, Mcp::ConfirmPolicy::NeverAsk }) {
        policyBox->addItem(policyLabel(option), static_cast<int>(option));
    }
    policyBox->setCurrentIndex(std::max(0, policyBox->findData(static_cast<int>(policy))));
    policyForm->addRow(McpPage::tr("Confirmation:"), policyBox);
    auto* chatBox = new QCheckBox(McpPage::tr("Ask in the agent's chat when its app supports it (otherwise in this window)"), &dialog);
    chatBox->setToolTip(McpPage::tr("The agent's app shows the question and passes on your answer. Turn this off to always answer here."));
    chatBox->setChecked(chat);
    policyForm->addRow(chatBox);
    layout->addLayout(policyForm);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    name = nameEdit->text().trimmed();
    scopes = {};
    for (const auto& [scope, box] : boxes) {
        if (box->isChecked()) {
            scopes |= scope;
        }
    }
    policy = static_cast<Mcp::ConfirmPolicy>(policyBox->currentData().toInt());
    chat = chatBox->isChecked();
    return true;
}
}  // namespace

McpPage::McpPage(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);

    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    layout->addWidget(m_intro);

    auto* serverForm = new QFormLayout();
    m_enabled = new QCheckBox(this);
    serverForm->addRow(m_enabled);
    m_portLabel = new QLabel(this);
    m_port = new QSpinBox(this);
    m_port->setRange(MIN_PORT - 1, MAX_PORT);
    m_port->setSpecialValueText(tr("Automatic"));
    m_portLabel->setBuddy(m_port);
    serverForm->addRow(m_portLabel, m_port);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    serverForm->addRow(m_status);
    m_stdio = new QCheckBox(this);
    m_copyStdioConfig = new QPushButton(QIcon::fromTheme("copy"), QString(), this);
    auto* stdioRow = new QHBoxLayout();
    stdioRow->addWidget(m_stdio, 1);
    stdioRow->addWidget(m_copyStdioConfig);
    serverForm->addRow(stdioRow);
    m_allowCommands = new QCheckBox(this);
    serverForm->addRow(m_allowCommands);
    layout->addLayout(serverForm);

    m_tokensLabel = new QLabel(this);
    layout->addWidget(m_tokensLabel);
    m_tokenList = new QTreeWidget(this);
    m_tokenList->setRootIsDecorated(false);
    m_tokenList->setColumnCount(5);
    m_tokenList->header()->setStretchLastSection(false);
    m_tokenList->header()->setSectionResizeMode(ScopesColumn, QHeaderView::Stretch);
    layout->addWidget(m_tokenList, 1);

    auto* tokenButtons = new QHBoxLayout();
    m_create = new QPushButton(this);
    m_edit = new QPushButton(this);
    m_revoke = new QPushButton(this);
    tokenButtons->addWidget(m_create);
    tokenButtons->addWidget(m_edit);
    tokenButtons->addWidget(m_revoke);
    tokenButtons->addStretch();
    layout->addLayout(tokenButtons);

    m_activityLabel = new QLabel(this);
    layout->addWidget(m_activityLabel);
    m_activity = new QTreeWidget(this);
    m_activity->setRootIsDecorated(false);
    m_activity->setColumnCount(5);
    layout->addWidget(m_activity, 1);

    connect(m_copyStdioConfig, &QPushButton::clicked, this, [] {
        // Claude Desktop (and other clients that start a command) run the launcher binary itself as a relay
        const QJsonObject server{ { "command", QCoreApplication::applicationFilePath() }, { "args", QJsonArray{ "--mcp-stdio" } } };
        GuiUtil::setClipboardText(QString::fromUtf8(
            QJsonDocument(QJsonObject{ { "mcpServers", QJsonObject{ { SERVER_KEY, server } } } }).toJson(QJsonDocument::Indented)));
    });
    // Ticking it is a deliberate act: say plainly what it allows
    connect(m_allowCommands, &QCheckBox::clicked, this, [this](bool checked) {
        if (!checked) {
            return;
        }
        const auto answer = QMessageBox::warning(
            this, tr("Allow agents to run commands?"),
            tr("Agents whose token has the \"commands\" scope will be able to run any program on this computer as you: install "
               "or delete software, read your files, change your system. Only allow this for agents you trust.\n\n"
               "With the default confirmation policy you are asked before every command."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            m_allowCommands->setChecked(false);
        }
    });
    connect(m_create, &QPushButton::clicked, this, &McpPage::createToken);
    connect(m_edit, &QPushButton::clicked, this, &McpPage::editToken);
    connect(m_revoke, &QPushButton::clicked, this, &McpPage::revokeToken);
    connect(m_tokenList, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool selected = !selectedTokenId().isEmpty();
        m_edit->setEnabled(selected);
        m_revoke->setEnabled(selected);
    });
    connect(m_tokenList, &QTreeWidget::itemDoubleClicked, this, &McpPage::editToken);

    if (auto* service = APPLICATION->mcp()) {
        connect(service, &McpService::stateChanged, this, &McpPage::refreshStatus);
        connect(service, &McpService::callRecorded, this, &McpPage::addActivityRow);
    }

    retranslate();
    loadSettings();
    refreshTokens();
    refreshActivity();
    refreshStatus();
    m_edit->setEnabled(false);
    m_revoke->setEnabled(false);
}

void McpPage::retranslate()
{
    m_intro->setText(
        tr("Let AI agents (Claude Code, Cursor, VS Code and other MCP clients) manage the launcher: instances, mods, settings, "
           "launching. The endpoint only listens on this computer (127.0.0.1), and every agent needs a token created here."));
    m_enabled->setText(tr("Enable the MCP endpoint"));
    m_portLabel->setText(tr("Port:"));
    m_stdio->setText(tr("Allow clients that start the launcher as a command (Claude Desktop): they use the \"%1\" token")
                         .arg(QString("Local stdio clients")));
    m_copyStdioConfig->setText(tr("Copy Claude Desktop config"));
    m_allowCommands->setText(tr("Allow agents to run commands on this computer (dangerous; also needs the token's \"commands\" scope)"));
    m_tokensLabel->setText(tr("Tokens"));
    m_tokenList->setHeaderLabels({ tr("Name"), tr("Scopes"), tr("Confirmation"), tr("Created"), tr("Last used") });
    m_create->setText(tr("Create token..."));
    m_edit->setText(tr("Edit..."));
    m_revoke->setText(tr("Revoke"));
    m_activityLabel->setText(tr("Recent agent activity"));
    m_activity->setHeaderLabels({ tr("Token"), tr("Client"), tr("Tool"), tr("Result"), tr("Time") });
    refreshStatus();
}

void McpPage::loadSettings()
{
    auto* settings = APPLICATION->settings();
    m_enabled->setChecked(settings->get("McpEnabled").toBool());
    m_stdio->setChecked(settings->get("McpStdioEnabled").toBool());
    m_allowCommands->setChecked(settings->get("McpAllowCommands").toBool());
    const int port = settings->get("McpPort").toInt();
    m_port->setValue(port < MIN_PORT ? MIN_PORT - 1 : port);
}

bool McpPage::apply()
{
    auto* settings = APPLICATION->settings();
    settings->set("McpEnabled", m_enabled->isChecked());
    settings->set("McpStdioEnabled", m_stdio->isChecked());
    settings->set("McpAllowCommands", m_allowCommands->isChecked());
    settings->set("McpPort", m_port->value() < MIN_PORT ? 0 : m_port->value());
    if (auto* service = APPLICATION->mcp()) {
        service->applySettings();
        // An automatic port was just chosen; show it so it can be copied into client configs
        const int port = settings->get("McpPort").toInt();
        m_port->setValue(port < MIN_PORT ? MIN_PORT - 1 : port);
        // Enabling stdio clients creates their token
        refreshTokens();
    }
    return true;
}

void McpPage::refreshStatus()
{
    auto* service = APPLICATION->mcp();
    if (!service) {
        m_status->setText(tr("Not available yet."));
        return;
    }
    if (!service->lastError().isEmpty()) {
        m_status->setText(tr("Error: %1").arg(service->lastError()));
    } else if (service->isListening()) {
        m_status->setText(tr("Listening on %1").arg(service->endpointUrl()));
    } else {
        m_status->setText(tr("Off"));
    }
}

void McpPage::refreshTokens()
{
    m_tokenList->clear();
    auto* service = APPLICATION->mcp();
    if (!service) {
        return;
    }
    for (const auto& token : service->tokens().tokens()) {
        auto* item = new QTreeWidgetItem(m_tokenList);
        item->setText(NameColumn, token.name);
        item->setText(ScopesColumn, Mcp::scopeNames(token.scopes).join(", "));
        item->setText(PolicyColumn, token.chatConfirmations ? tr("%1 (in the agent's chat if possible)").arg(policyLabel(token.policy))
                                                            : policyLabel(token.policy));
        item->setText(CreatedColumn, formatTime(token.created));
        item->setText(LastUsedColumn, formatTime(token.lastUsed));
        item->setData(NameColumn, Qt::UserRole, token.id);
    }
    for (int column = 0; column < m_tokenList->columnCount(); ++column) {
        if (column != ScopesColumn) {
            m_tokenList->resizeColumnToContents(column);
        }
    }
}

void McpPage::refreshActivity()
{
    m_activity->clear();
    if (auto* service = APPLICATION->mcp()) {
        for (const auto& record : service->recentCalls()) {
            addActivityRow(record);
        }
    }
}

void McpPage::addActivityRow(const Mcp::CallRecord& record)
{
    auto* item = new QTreeWidgetItem();
    item->setText(0, record.tokenName);
    item->setText(1, record.clientName);
    item->setText(2, record.tool);
    item->setText(3, record.isError ? tr("Error") : tr("OK"));
    item->setText(4, tr("%1 ms").arg(record.durationMs));
    item->setToolTip(2, record.summary);
    item->setToolTip(3, record.error);
    m_activity->insertTopLevelItem(0, item);
    while (m_activity->topLevelItemCount() > McpService::MAX_RECENT_CALLS) {
        delete m_activity->takeTopLevelItem(m_activity->topLevelItemCount() - 1);
    }
}

QString McpPage::selectedTokenId() const
{
    const auto selected = m_tokenList->selectedItems();
    return selected.isEmpty() ? QString() : selected.first()->data(NameColumn, Qt::UserRole).toString();
}

void McpPage::createToken()
{
    auto* service = APPLICATION->mcp();
    if (!service) {
        return;
    }
    QString name;
    Mcp::Scopes scopes = Mcp::Scope::Read | Mcp::Scope::Instances | Mcp::Scope::Content | Mcp::Scope::Launch;
    auto policy = Mcp::ConfirmPolicy::AskDestructive;
    bool chat = true;
    if (!editTokenDialog(this, tr("Create token"), name, scopes, policy, chat)) {
        return;
    }
    const auto [token, secret] = service->tokens().create(name, scopes, policy);
    service->tokens().setChatConfirmations(token.id, chat);
    if (auto res = service->saveTokens(); !res) {
        service->tokens().revoke(token.id);
        QMessageBox::critical(this, tr("Could not save the token"), res.error());
        return;
    }
    refreshTokens();
    showSecret(token.name, secret);
}

void McpPage::editToken()
{
    auto* service = APPLICATION->mcp();
    const auto id = selectedTokenId();
    if (!service || id.isEmpty()) {
        return;
    }
    auto token = service->tokens().find(id);
    if (!token) {
        return;
    }
    QString name = token->name;
    Mcp::Scopes scopes = token->scopes;
    auto policy = token->policy;
    bool chat = token->chatConfirmations;
    if (!editTokenDialog(this, tr("Edit token"), name, scopes, policy, chat)) {
        return;
    }
    service->tokens().update(id, name, scopes, policy);
    service->tokens().setChatConfirmations(id, chat);
    if (auto res = service->saveTokens(); !res) {
        QMessageBox::critical(this, tr("Could not save the token"), res.error());
    }
    refreshTokens();
}

void McpPage::revokeToken()
{
    auto* service = APPLICATION->mcp();
    const auto id = selectedTokenId();
    if (!service || id.isEmpty()) {
        return;
    }
    const auto token = service->tokens().find(id);
    const auto answer = QMessageBox::question(
        this, tr("Revoke token"),
        tr("Revoke \"%1\"? Agents using it lose access immediately.").arg(token ? token->name : id));
    if (answer != QMessageBox::Yes) {
        return;
    }
    service->tokens().revoke(id);
    if (auto res = service->saveTokens(); !res) {
        QMessageBox::critical(this, tr("Could not save the change"), res.error());
    }
    refreshTokens();
}

void McpPage::showSecret(const QString& name, const QString& secret)
{
    auto* service = APPLICATION->mcp();
    // The port is chosen the first time the endpoint starts; until then there is no address to hand out
    const auto url = service->port() == 0 ? QString("http://127.0.0.1:PORT/mcp") : service->endpointUrl();
    const auto header = QString("Bearer %1").arg(secret);

    const auto claudeCode =
        QString("claude mcp add --transport http %1 %2 --header \"Authorization: %3\"").arg(SERVER_KEY, url, header);

    const QJsonObject server{ { "type", "http" }, { "url", url }, { "headers", QJsonObject{ { "Authorization", header } } } };
    const auto clientJson = QString::fromUtf8(
        QJsonDocument(QJsonObject{ { "mcpServers", QJsonObject{ { SERVER_KEY, server } } } }).toJson(QJsonDocument::Indented));

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Token \"%1\" created").arg(name));
    dialog.resize(720, 480);
    auto* layout = new QVBoxLayout(&dialog);

    auto* warning = new QLabel(
        tr("Copy the token now: it is shown only this once. Anyone with it can do what its scopes allow while the endpoint is on."),
        &dialog);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    auto addCopyRow = [&dialog, layout](const QString& label, const QString& text, bool multiline) {
        layout->addWidget(new QLabel(label, &dialog));
        auto* row = new QHBoxLayout();
        if (multiline) {
            auto* edit = new QPlainTextEdit(text, &dialog);
            edit->setReadOnly(true);
            edit->setLineWrapMode(QPlainTextEdit::NoWrap);
            row->addWidget(edit, 1);
        } else {
            auto* edit = new QLineEdit(text, &dialog);
            edit->setReadOnly(true);
            edit->setCursorPosition(0);
            row->addWidget(edit, 1);
        }
        auto* copy = new QPushButton(QIcon::fromTheme("copy"), McpPage::tr("Copy"), &dialog);
        QObject::connect(copy, &QPushButton::clicked, &dialog, [text] { GuiUtil::setClipboardText(text); });
        row->addWidget(copy, 0, Qt::AlignTop);
        layout->addLayout(row);
    };

    addCopyRow(tr("Token:"), secret, false);
    addCopyRow(tr("Claude Code (run in a terminal):"), claudeCode, false);
    addCopyRow(tr("Cursor and other clients that read an \"mcpServers\" JSON config (VS Code names the key \"servers\"):"), clientJson, true);

    if (!service->isListening()) {
        auto* note = new QLabel(service->port() == 0 ? tr("The endpoint is off, so it has no port yet: enable it on this page and apply, "
                                                         "then replace PORT with the port shown there.")
                                                      : tr("The endpoint is off. Enable it on this page and apply before connecting."),
                                &dialog);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
