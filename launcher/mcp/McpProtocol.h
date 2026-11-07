// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

#include "mcp/McpTokens.h"
#include "mcp/McpTool.h"

// JSON-RPC and MCP semantics, independent of HTTP. Serves two protocol eras from one endpoint:
//
//   modern  2026-07-28            stateless: version + capabilities in every request's _meta, server/discover,
//                                 Mcp-Method / Mcp-Name headers validated, resultType on every result
//   legacy  2025-03-26..2025-11-25  initialize handshake; no session is ever minted, so these clients run stateless
//
// A request is modern when its params._meta carries io.modelcontextprotocol/protocolVersion, legacy otherwise.
namespace Mcp {

constexpr auto MODERN_VERSION = "2026-07-28";
constexpr auto LATEST_LEGACY_VERSION = "2025-11-25";
// Spec-assigned JSON-RPC codes (2026-07-28 basic/index "Error Codes")
constexpr int ERROR_PARSE = -32700;
constexpr int ERROR_INVALID_REQUEST = -32600;
constexpr int ERROR_METHOD_NOT_FOUND = -32601;
constexpr int ERROR_INVALID_PARAMS = -32602;
constexpr int ERROR_INTERNAL = -32603;
constexpr int ERROR_HEADER_MISMATCH = -32020;
constexpr int ERROR_UNSUPPORTED_VERSION = -32022;

const QStringList& modernVersions();
const QStringList& legacyVersions();
QStringList supportedVersions();

// Decodes the "=?base64?...?=" sentinel the transport uses for non-ASCII header values; plain values pass through
std::optional<QString> decodeHeaderValue(const QByteArray& raw);

QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message, const QJsonValue& data = {});

struct Incoming {
    QJsonObject message;
    std::optional<QByteArray> versionHeader;
    std::optional<QByteArray> methodHeader;
    std::optional<QByteArray> nameHeader;
    TokenInfo token;
};

// One JSON-RPC exchange in flight. The transport decides how notifications travel (SSE) and what HTTP status means.
class Exchange {
   public:
    virtual ~Exchange() = default;
    // Request-scoped notifications such as notifications/progress
    virtual void notify(const QJsonObject& notification) = 0;
    virtual void finish(int httpStatus, const QJsonObject& response) = 0;
    // A notification was accepted (202, no body)
    virtual void accept() = 0;
    virtual void reject(int httpStatus, const QJsonObject& response) = 0;
    virtual std::shared_ptr<bool> cancelled() const = 0;
};
using ExchangePtr = std::shared_ptr<Exchange>;

struct ServerInfo {
    QString name;
    QString title;
    QString version;
    QString instructions;
};

struct CallRecord {
    QString tokenName;
    QString clientName;
    QString tool;
    QString summary;  // short, secret-free rendering of the arguments
    bool isError = false;
    QString error;
    qint64 durationMs = 0;
};

class Protocol {
   public:
    Protocol(const ToolRegistry* tools, ServerInfo info);

    void handle(const Incoming& incoming, const ExchangePtr& exchange);

    // Every tools/call, after it finished; feeds the activity log
    std::function<void(const CallRecord&)> onToolCall;
    // Every tools/call, as it starts (the activity bar shows what agents are doing)
    std::function<void(const CallRecord&)> onToolStart;
    // Fills in what only the host knows, such as how to ask the human for confirmation
    std::function<void(CallContext&)> prepareContext;
    // A reason to refuse every call (the user pressed Stop), or nothing
    std::function<std::optional<QString>()> gate;

   private:
    struct Request {
        QJsonValue id;
        QString method;
        QJsonObject params;
        bool modern = false;
        QString version;
        QString clientName;
    };

    void dispatch(const Request& request, const Incoming& incoming, const ExchangePtr& exchange);
    void callTool(const Request& request, const Incoming& incoming, const ExchangePtr& exchange);

    QJsonObject result(const Request& request, QJsonObject body) const;
    QJsonObject discoverResult(const Request& request) const;
    QJsonObject initializeResult(const Request& request) const;
    QJsonObject capabilities() const;
    // HTTP status for a JSON-RPC error: modern requests use the status the spec assigns, legacy ones 200
    static int errorStatus(const Request& request, int code);

    const ToolRegistry* m_tools;
    ServerInfo m_info;
};

}  // namespace Mcp
