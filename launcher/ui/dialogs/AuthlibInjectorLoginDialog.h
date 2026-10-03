// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>

#include "minecraft/auth/AuthFlow.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "minecraft/auth/YggdrasilServerResolveTask.h"

namespace Ui {
class AuthlibInjectorLoginDialog;
}

//! Logs in to a Yggdrasil-compatible server (Ely.by, LittleSkin, Drasl, ...) used through authlib-injector
class AuthlibInjectorLoginDialog : public QDialog {
    Q_OBJECT

   public:
    ~AuthlibInjectorLoginDialog() override;

    /** Shows the dialog and returns the logged in account, or nullptr if cancelled. With `reauth`,
     *  the server and username of that account are filled in. */
    static MinecraftAccountPtr newAccount(QWidget* parent, const QString& message, const MinecraftAccountPtr& reauth = nullptr);

   protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

   private slots:
    void accept() override;
    void reject() override;

    void onPresetChanged(int index);
    void updateAcceptAllowed();

    void onServerResolved();
    void onLoginSucceeded();
    void onFailed(const QString& reason);
    void onSelectProfile(const QStringList& names, int* chosenIndex);
    void onTwoFactorRequired();

   private:
    explicit AuthlibInjectorLoginDialog(QWidget* parent = nullptr);

    void setBusy(bool busy);
    void showStatus(const QString& text, bool isError);

   private:
    Ui::AuthlibInjectorLoginDialog* ui;

    shared_qobject_ptr<YggdrasilServerResolveTask> m_resolveTask;
    shared_qobject_ptr<AuthFlow> m_loginTask;
    MinecraftAccountPtr m_account;
};
