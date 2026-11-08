// SPDX-License-Identifier: GPL-3.0-only
#include "McpActivityBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>

#include <memory>

#include "mcp/McpService.h"

McpActivityBar::McpActivityBar(McpService* service, QWidget* parent) : QWidget(parent), m_service(service)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new QLabel(this);
    m_button = new QPushButton(this);
    m_button->setFlat(true);
    layout->addWidget(m_label);
    layout->addWidget(m_button);

    m_linger.setSingleShot(true);
    m_linger.setInterval(LINGER_MS);
    connect(&m_linger, &QTimer::timeout, this, &McpActivityBar::refresh);

    connect(m_button, &QPushButton::clicked, this, [this] {
        if (!m_service) {
            return;
        }
        if (m_service->isPaused()) {
            m_service->resumeAgents();
        } else {
            m_service->stopAgents();
        }
    });
    connect(service, &McpService::activityChanged, this, [this] {
        m_linger.start();
        refresh();
    });
    connect(service, &McpService::stateChanged, this, &McpActivityBar::refresh);

    service->setPrompter([this](const McpConfirmRequest& request, std::function<void(bool)> answer) { prompt(request, std::move(answer)); });
    refresh();
}

McpActivityBar::~McpActivityBar()
{
    if (m_service) {
        m_service->setPrompter(nullptr);
    }
}

void McpActivityBar::refresh()
{
    if (!m_service || !m_service->isListening()) {
        hide();
        return;
    }
    const int jobs = static_cast<int>(m_service->jobs()->running().size());
    const bool busy = m_service->activeCalls() > 0 || jobs > 0;

    if (m_service->isPaused()) {
        m_label->setText(tr("AI agents stopped"));
        m_button->setText(tr("Resume"));
        show();
        return;
    }
    if (!busy && !m_linger.isActive()) {
        hide();
        return;
    }

    const auto& last = m_service->lastStarted();
    const auto who = last.clientName.isEmpty() ? last.tokenName : QString("%1 (%2)").arg(last.tokenName, last.clientName);
    auto text = tr("AI agent %1: %2").arg(who, last.tool);
    if (jobs > 0) {
        text += tr(", %n job(s) running", "", jobs);
    }
    m_label->setText(text);
    m_label->setToolTip(last.summary);
    m_button->setText(tr("Stop"));
    show();
}

void McpActivityBar::prompt(const McpConfirmRequest& request, std::function<void(bool)> answer)
{
    const auto who = request.clientName.isEmpty() ? request.tokenName : QString("%1 (%2)").arg(request.tokenName, request.clientName);

    const QString bullet = QString(QChar(0x2022)) + ' ';
    const auto list = request.actions.size() == 1 ? request.actions.first() : bullet + request.actions.join("\n" + bullet);
    auto* box = new QMessageBox(QMessageBox::Question, tr("Allow AI agent action?"),
                                tr("The AI agent %1 wants to:\n\n%2\n\nAllow it?").arg(who, list), QMessageBox::NoButton, window());
    auto* allow = box->addButton(tr("Allow"), QMessageBox::AcceptRole);
    box->addButton(tr("Deny"), QMessageBox::RejectRole);
    box->setAttribute(Qt::WA_DeleteOnClose);
    // Not modal: the launcher stays usable, and an unanswered question must not block the event loop
    box->setWindowModality(Qt::NonModal);

    // Each question is answered once: by a button, by closing the box, or by the timeout (which denies)
    auto answered = std::make_shared<bool>(false);
    auto respond = [answered, answer = std::move(answer)](bool allowed) {
        if (*answered) {
            return;
        }
        *answered = true;
        answer(allowed);
    };
    connect(box, &QMessageBox::finished, box, [box, allow, respond] { respond(box->clickedButton() == allow); });
    connect(box, &QObject::destroyed, this, [respond] { respond(false); });
    QTimer::singleShot(CONFIRM_TIMEOUT_MS, box, [box] { box->close(); });

    box->show();
    box->raise();
    box->activateWindow();
}
