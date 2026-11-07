// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

#include "mcp/McpHttp.h"
#include "mcp/McpJobs.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpTokens.h"
#include "mcp/McpTool.h"

class SettingsObject;

// What the human is asked when an agent wants to make a change the token's policy reserves for them
struct McpConfirmRequest {
    QString tokenName;
    QString clientName;
    QString action;
};

// The launcher's MCP endpoint: http://127.0.0.1:<port>/mcp. Off until the user enables it; while off no socket is open.
//
//   request ─► loopback peer? ─► Host is loopback? ─► Origin absent or loopback? ─► POST /mcp?
//           ─► bearer token valid? ─► rate limit ─► JSON body ─► Mcp::Protocol ─► tool
//
// HTTP concerns stop here; Mcp::Protocol never sees a socket and tools never see JSON-RPC.
class McpService : public QObject {
    Q_OBJECT

   public:
    static constexpr auto ENDPOINT_PATH = "/mcp";
    static constexpr int MAX_RECENT_CALLS = 200;
    static constexpr int RATE_LIMIT_PER_MINUTE = 600;

    McpService(SettingsObject* settings, QString dataRoot, QObject* parent = nullptr);
    ~McpService() override;

    static void registerSettings(SettingsObject* settings);

    // Starts, stops or rebinds to match the McpEnabled / McpPort settings
    void applySettings();

    bool isListening() const;
    quint16 port() const;
    QString endpointUrl() const;
    QString lastError() const { return m_lastError; }

    Mcp::TokenStore& tokens() { return m_tokens; }

    // The token the --mcp-stdio relay uses; its secret lives in <data>/mcp/stdio.json while stdio clients are enabled
    QString stdioTokenId() const;
    Result<> saveTokens();

    const QList<Mcp::CallRecord>& recentCalls() const { return m_recentCalls; }

    // The UI installs this to put confirmation questions in front of the user; `answer` must be called exactly once
    using Prompter = std::function<void(const McpConfirmRequest& request, std::function<void(bool allowed)> answer)>;
    void setPrompter(Prompter prompter) { m_prompter = std::move(prompter); }

    // The Stop button: cancels agents' running jobs and refuses their calls until resumed
    void stopAgents();
    void resumeAgents();
    bool isPaused() const { return m_paused; }
    int activeCalls() const { return m_activeCalls; }
    // The latest call that started, for the activity bar
    const Mcp::CallRecord& lastStarted() const { return m_lastStarted; }
    const Mcp::ToolRegistry& tools() const { return m_tools; }
    const Mcp::ToolRegistry& ops() const { return m_ops; }
    Mcp::JobRegistry* jobs() { return &m_jobs; }

   signals:
    void stateChanged();
    void callRecorded(const Mcp::CallRecord& record);
    void activityChanged();

   private:
    void handleHttp(const Mcp::Http::Request& request, const Mcp::Http::ResponderPtr& responder);
    void recordCall(const Mcp::CallRecord& record);
    void updateStdioConfig();
    bool allowRequest(const QString& tokenId);
    QString instructions() const;

    SettingsObject* m_settings;
    QString m_dataRoot;
    Mcp::TokenStore m_tokens;
    Mcp::ToolRegistry m_tools;
    // The op catalog behind search_ops / call / batch
    Mcp::ToolRegistry m_ops;
    Mcp::JobRegistry m_jobs;
    Mcp::ResourceCatalog m_resources;
    Mcp::PromptCatalog m_prompts;
    std::unique_ptr<Mcp::Protocol> m_protocol;
    Mcp::Http::Server* m_server = nullptr;
    QString m_lastError;
    QList<Mcp::CallRecord> m_recentCalls;
    Prompter m_prompter;
    bool m_paused = false;
    int m_activeCalls = 0;
    Mcp::CallRecord m_lastStarted;
    // Per token: start of the current one-minute window and requests seen in it
    QHash<QString, std::pair<qint64, int>> m_rate;
};
