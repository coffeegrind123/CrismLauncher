// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

#include "mcp/McpHttp.h"
#include "mcp/McpJobs.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpTokens.h"
#include "mcp/McpTool.h"

class SettingsObject;

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
    Result<> saveTokens();

    const QList<Mcp::CallRecord>& recentCalls() const { return m_recentCalls; }
    const Mcp::ToolRegistry& tools() const { return m_tools; }
    const Mcp::ToolRegistry& ops() const { return m_ops; }
    Mcp::JobRegistry* jobs() { return &m_jobs; }

   signals:
    void stateChanged();
    void callRecorded(const Mcp::CallRecord& record);

   private:
    void handleHttp(const Mcp::Http::Request& request, const Mcp::Http::ResponderPtr& responder);
    void recordCall(const Mcp::CallRecord& record);
    bool allowRequest(const QString& tokenId);
    QString instructions() const;

    SettingsObject* m_settings;
    QString m_dataRoot;
    Mcp::TokenStore m_tokens;
    Mcp::ToolRegistry m_tools;
    // The op catalog behind search_ops / call / batch
    Mcp::ToolRegistry m_ops;
    Mcp::JobRegistry m_jobs;
    std::unique_ptr<Mcp::Protocol> m_protocol;
    Mcp::Http::Server* m_server = nullptr;
    QString m_lastError;
    QList<Mcp::CallRecord> m_recentCalls;
    // Per token: start of the current one-minute window and requests seen in it
    QHash<QString, std::pair<qint64, int>> m_rate;
};
