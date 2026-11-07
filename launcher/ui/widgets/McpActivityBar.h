// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class McpService;
struct McpConfirmRequest;

// Status-bar strip shown while AI agents act: what they're doing, and a Stop button that cancels their jobs and
// refuses further calls until resumed. It also asks the user to allow changes the agent's token policy reserves.
class McpActivityBar : public QWidget {
    Q_OBJECT

   public:
    static constexpr int CONFIRM_TIMEOUT_MS = 2 * 60 * 1000;
    static constexpr int LINGER_MS = 10 * 1000;

    McpActivityBar(McpService* service, QWidget* parent);
    ~McpActivityBar() override;

   private:
    void refresh();
    void prompt(const McpConfirmRequest& request, std::function<void(bool)> answer);

    QPointer<McpService> m_service;
    QLabel* m_label;
    QPushButton* m_button;
    QTimer m_linger;
};
