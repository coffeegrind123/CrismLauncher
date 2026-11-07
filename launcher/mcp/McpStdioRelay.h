// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// `<launcher> --mcp-stdio`: lets stdio-only MCP clients (Claude Desktop and others that spawn a command) reach the
// running launcher's HTTP endpoint.
//
//   client ── stdin (one JSON-RPC message per line) ──► relay ── POST http://127.0.0.1:<port>/mcp ──► launcher
//   client ◄─ stdout (one message per line)        ◄── relay ◄── JSON or SSE response ─────────────
//
// The port and a token reserved for the relay come from <data>/mcp/stdio.json, which the launcher writes only while the
// user has enabled stdio clients. If the launcher is not running, the relay starts it and waits for the endpoint.
// Nothing but protocol messages is ever written to stdout.
class McpStdioRelay : public QObject {
    Q_OBJECT

   public:
    static constexpr int STARTUP_TIMEOUT_MS = 60 * 1000;
    static constexpr auto CONFIG_FILE = "mcp/stdio.json";

    McpStdioRelay(QString dataPath, QStringList launcherArguments, QObject* parent = nullptr);
    ~McpStdioRelay() override;

    void start();

    // Builds the HTTP headers for one message; exposed for tests
    static QList<std::pair<QByteArray, QByteArray>> headersFor(const QJsonObject& message, const QString& legacyVersion);

   private:
    enum class State { Connecting, Ready, Failed };

    void onLine(const QByteArray& line);
    void onInputClosed();
    void forward(const QByteArray& line);
    void failRequest(const QByteArray& line, const QString& reason);
    void write(const QByteArray& message);
    void tryConnect();
    bool readConfig();
    void finishIfDone();

    QString m_dataPath;
    QStringList m_launcherArguments;
    QNetworkAccessManager* m_network;
    State m_state = State::Connecting;
    QString m_failure;
    quint16 m_port = 0;
    QString m_token;
    QString m_legacyVersion;  // negotiated by initialize, for clients of the 2025 protocol versions
    QList<QByteArray> m_pending;
    int m_inFlight = 0;
    bool m_inputClosed = false;
    bool m_launched = false;
    qint64 m_connectStarted = 0;
};
