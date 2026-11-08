// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QIcon>
#include <QWidget>

#include "ui/pages/BasePage.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace Mcp {
struct CallRecord;
}

// Settings > AI agents (MCP): turns the endpoint on, manages the bearer tokens agents use, shows recent agent calls
class McpPage : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit McpPage(QWidget* parent = nullptr);

    QString displayName() const override { return tr("AI agents (MCP)"); }
    QIcon icon() const override { return QIcon::fromTheme("custom-commands"); }
    QString id() const override { return "mcp-settings"; }
    bool apply() override;
    void retranslate() override;

   private:
    void loadSettings();
    void refreshStatus();
    void refreshTokens();
    void refreshActivity();
    void addActivityRow(const Mcp::CallRecord& record);
    void createToken();
    void editToken();
    void revokeToken();
    void showSecret(const QString& name, const QString& secret);
    QString selectedTokenId() const;

    QLabel* m_intro;
    QCheckBox* m_enabled;
    QLabel* m_portLabel;
    QSpinBox* m_port;
    QLabel* m_status;
    QCheckBox* m_stdio;
    QCheckBox* m_allowCommands;
    QPushButton* m_copyStdioConfig;
    QLabel* m_tokensLabel;
    QTreeWidget* m_tokenList;
    QPushButton* m_create;
    QPushButton* m_edit;
    QPushButton* m_revoke;
    QLabel* m_activityLabel;
    QTreeWidget* m_activity;
};
