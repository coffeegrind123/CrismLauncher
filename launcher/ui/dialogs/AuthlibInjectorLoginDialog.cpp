// SPDX-License-Identifier: GPL-3.0-only

#include "AuthlibInjectorLoginDialog.h"

#include <QDragEnterEvent>
#include <algorithm>
#include <array>
#include <QDropEvent>
#include <QInputDialog>
#include <QMimeData>
#include <QPushButton>

#include "minecraft/auth/Yggdrasil.h"
#include "ui_AuthlibInjectorLoginDialog.h"

namespace {
struct ServerPreset {
    const char* name;
    QString url;
};

// Empty URL = custom server typed by the user
const std::array s_presets{
    ServerPreset{ QT_TRANSLATE_NOOP("AuthlibInjectorLoginDialog", "Ely.by"), QStringLiteral("https://authserver.ely.by") },
    ServerPreset{ QT_TRANSLATE_NOOP("AuthlibInjectorLoginDialog", "LittleSkin"), QStringLiteral("https://littleskin.cn") },
    ServerPreset{ QT_TRANSLATE_NOOP("AuthlibInjectorLoginDialog", "Custom server"), QString() },
};
constexpr int CUSTOM_PRESET = std::tuple_size_v<decltype(s_presets)> - 1;

bool isPresetUrl(const QString& url)
{
    return std::ranges::any_of(s_presets, [&url](const ServerPreset& preset) { return !preset.url.isEmpty() && preset.url == url; });
}

QUrl serverFromMimeData(const QMimeData* mimeData)
{
    if (!mimeData->hasText() || !mimeData->text().trimmed().startsWith(Yggdrasil::DRAG_LINK_PREFIX)) {
        return {};
    }
    return Yggdrasil::normalizeServerInput(mimeData->text());
}
}  // namespace

AuthlibInjectorLoginDialog::AuthlibInjectorLoginDialog(QWidget* parent) : QDialog(parent), ui(new Ui::AuthlibInjectorLoginDialog)
{
    ui->setupUi(this);
    setAcceptDrops(true);

    for (const auto& preset : s_presets) {
        ui->presetComboBox->addItem(tr(preset.name));
    }

    ui->totpLabel->setVisible(false);
    ui->totpTextBox->setVisible(false);
    ui->statusLabel->setVisible(false);
    ui->buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Log In"));

    connect(ui->presetComboBox, &QComboBox::currentIndexChanged, this, &AuthlibInjectorLoginDialog::onPresetChanged);
    connect(ui->serverTextBox, &QLineEdit::textChanged, this, &AuthlibInjectorLoginDialog::updateAcceptAllowed);
    connect(ui->usernameTextBox, &QLineEdit::textChanged, this, &AuthlibInjectorLoginDialog::updateAcceptAllowed);
    connect(ui->passwordTextBox, &QLineEdit::textChanged, this, &AuthlibInjectorLoginDialog::updateAcceptAllowed);
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &AuthlibInjectorLoginDialog::accept);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &AuthlibInjectorLoginDialog::reject);

    onPresetChanged(0);
    ui->usernameTextBox->setFocus();
}

AuthlibInjectorLoginDialog::~AuthlibInjectorLoginDialog()
{
    delete ui;
}

void AuthlibInjectorLoginDialog::onPresetChanged(int index)
{
    const bool custom = index == CUSTOM_PRESET;
    ui->serverTextBox->setEnabled(custom);
    if (!custom) {
        ui->serverTextBox->setText(s_presets.at(index).url);
    } else if (isPresetUrl(ui->serverTextBox->text())) {
        ui->serverTextBox->clear();
    }
    ui->linksLabel->clear();
    updateAcceptAllowed();
}

void AuthlibInjectorLoginDialog::updateAcceptAllowed()
{
    const bool complete = Yggdrasil::normalizeServerInput(ui->serverTextBox->text()).isValid() && !ui->usernameTextBox->text().isEmpty() &&
                          !ui->passwordTextBox->text().isEmpty();
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(complete);
}

void AuthlibInjectorLoginDialog::dragEnterEvent(QDragEnterEvent* event)
{
    if (serverFromMimeData(event->mimeData()).isValid()) {
        event->acceptProposedAction();
    }
}

void AuthlibInjectorLoginDialog::dropEvent(QDropEvent* event)
{
    const auto url = serverFromMimeData(event->mimeData());
    if (!url.isValid()) {
        return;
    }
    ui->presetComboBox->setCurrentIndex(CUSTOM_PRESET);
    ui->serverTextBox->setText(url.toString());
    event->acceptProposedAction();
}

void AuthlibInjectorLoginDialog::setBusy(bool busy)
{
    ui->presetComboBox->setEnabled(!busy);
    ui->serverTextBox->setEnabled(!busy && ui->presetComboBox->currentIndex() == CUSTOM_PRESET);
    ui->usernameTextBox->setEnabled(!busy);
    ui->passwordTextBox->setEnabled(!busy);
    ui->totpTextBox->setEnabled(!busy);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(!busy);
}

void AuthlibInjectorLoginDialog::showStatus(const QString& text, bool isError)
{
    ui->statusLabel->setText(isError ? QString("<font color='red'>%1</font>").arg(text.toHtmlEscaped()) : text.toHtmlEscaped());
    ui->statusLabel->setVisible(!text.isEmpty());
}

// Stage 1: find the server's API root and metadata
void AuthlibInjectorLoginDialog::accept()
{
    const auto serverUrl = Yggdrasil::normalizeServerInput(ui->serverTextBox->text());
    if (!serverUrl.isValid()) {
        showStatus(tr("The server URL is not valid."), true);
        return;
    }

    setBusy(true);
    showStatus(tr("Contacting the server..."), false);

    m_resolveTask.reset(new YggdrasilServerResolveTask(serverUrl));
    connect(m_resolveTask.get(), &Task::succeeded, this, &AuthlibInjectorLoginDialog::onServerResolved);
    connect(m_resolveTask.get(), &Task::failed, this, &AuthlibInjectorLoginDialog::onFailed);
    m_resolveTask->start();
}

// Stage 2: log in against the resolved API root
void AuthlibInjectorLoginDialog::onServerResolved()
{
    const auto apiRoot = m_resolveTask->apiRoot();
    const auto metadata = m_resolveTask->metadata();
    const auto base64 = QString::fromLatin1(metadata.toBase64());

    QStringList links;
    if (const auto registerLink = Yggdrasil::linkFromMetadata(base64, "register"); !registerLink.isEmpty()) {
        links << QString("<a href=\"%1\">%2</a>").arg(registerLink.toHtmlEscaped(), tr("Register"));
    }
    if (const auto homepage = Yggdrasil::linkFromMetadata(base64, "homepage"); !homepage.isEmpty()) {
        links << QString("<a href=\"%1\">%2</a>").arg(homepage.toHtmlEscaped(), tr("Website"));
    }
    ui->linksLabel->setText(links.join(" · "));

    auto password = ui->passwordTextBox->text();
    if (ui->totpTextBox->isVisible() && !ui->totpTextBox->text().trimmed().isEmpty()) {
        password += ":" + ui->totpTextBox->text().trimmed();
    }

    m_account = MinecraftAccount::createAuthlibInjector(ui->usernameTextBox->text().trimmed(), apiRoot, metadata);
    m_loginTask = m_account->login(false, password);
    connect(m_loginTask.get(), &AuthFlow::selectProfile, this, &AuthlibInjectorLoginDialog::onSelectProfile, Qt::DirectConnection);
    connect(m_loginTask.get(), &AuthFlow::twoFactorRequired, this, &AuthlibInjectorLoginDialog::onTwoFactorRequired);
    connect(m_loginTask.get(), &Task::succeeded, this, &AuthlibInjectorLoginDialog::onLoginSucceeded);
    connect(m_loginTask.get(), &Task::failed, this, &AuthlibInjectorLoginDialog::onFailed);
    connect(m_loginTask.get(), &Task::status, this, [this](const QString& status) { showStatus(status, false); });

    showStatus(tr("Logging in to %1...").arg(m_account->accountData()->serverName()), false);
    m_loginTask->start();
}

void AuthlibInjectorLoginDialog::onLoginSucceeded()
{
    QDialog::accept();
}

void AuthlibInjectorLoginDialog::onFailed(const QString& reason)
{
    m_account.reset();
    setBusy(false);
    showStatus(reason, true);
}

void AuthlibInjectorLoginDialog::onSelectProfile(const QStringList& names, int* chosenIndex)
{
    bool ok = false;
    const auto name = QInputDialog::getItem(this, tr("Choose a character"), tr("This account has several characters. Which one do you want to use?"),
                                            names, 0, false, &ok);
    *chosenIndex = ok ? names.indexOf(name) : -1;
}

void AuthlibInjectorLoginDialog::onTwoFactorRequired()
{
    ui->totpLabel->setVisible(true);
    ui->totpTextBox->setVisible(true);
    ui->totpTextBox->setFocus();
}

void AuthlibInjectorLoginDialog::reject()
{
    if (m_resolveTask && m_resolveTask->isRunning()) {
        m_resolveTask->abort();
    }
    if (m_loginTask && m_loginTask->isRunning()) {
        m_loginTask->abort();
    }
    QDialog::reject();
}

MinecraftAccountPtr AuthlibInjectorLoginDialog::newAccount(QWidget* parent, const QString& message, const MinecraftAccountPtr& reauth)
{
    AuthlibInjectorLoginDialog dialog(parent);
    dialog.ui->messageLabel->setText(message);

    if (reauth) {
        auto* data = reauth->accountData();
        dialog.ui->presetComboBox->setCurrentIndex(CUSTOM_PRESET);
        dialog.ui->serverTextBox->setText(data->authlibInjectorUrl);
        dialog.ui->usernameTextBox->setText(data->userName());
        dialog.ui->passwordTextBox->setFocus();
    }

    if (dialog.exec() == QDialog::Accepted) {
        return dialog.m_account;
    }
    return nullptr;
}
