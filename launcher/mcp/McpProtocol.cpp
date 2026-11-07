// SPDX-License-Identifier: GPL-3.0-only
#include "McpProtocol.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>


namespace Mcp {

namespace {
constexpr auto META_VERSION = "io.modelcontextprotocol/protocolVersion";
constexpr auto META_CLIENT_INFO = "io.modelcontextprotocol/clientInfo";
constexpr auto META_SERVER_INFO = "io.modelcontextprotocol/serverInfo";
constexpr QByteArrayView BASE64_PREFIX = "=?base64?";
constexpr QByteArrayView BASE64_SUFFIX = "?=";
// Versions before 2025-06-18 sent no MCP-Protocol-Version header (spec: such requests MAY be treated as this one)
constexpr auto HEADERLESS_VERSION = "2025-03-26";
// The tool list is fixed for the life of the process
constexpr qint64 LIST_TTL_MS = 60 * 60 * 1000;
constexpr int SUMMARY_MAX_CHARS = 200;

QString renderJson(const QJsonValue& value)
{
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return value.toVariant().toString();
}

QString summarize(const QJsonObject& arguments)
{
    auto text = renderJson(arguments);
    if (text.size() > SUMMARY_MAX_CHARS) {
        text = text.left(SUMMARY_MAX_CHARS - 1) + QChar(0x2026);
    }
    return text;
}

bool requiresName(const QString& method)
{
    return method == "tools/call" || method == "resources/read" || method == "prompts/get";
}
}  // namespace

const QStringList& modernVersions()
{
    static const QStringList s_versions{ MODERN_VERSION };
    return s_versions;
}

const QStringList& legacyVersions()
{
    static const QStringList s_versions{ "2025-11-25", "2025-06-18", "2025-03-26" };
    return s_versions;
}

QStringList supportedVersions()
{
    return modernVersions() + legacyVersions();
}

std::optional<QString> decodeHeaderValue(const QByteArray& raw)
{
    const auto markers = BASE64_PREFIX.size() + BASE64_SUFFIX.size();
    if (raw.size() < markers || !raw.startsWith(BASE64_PREFIX) || !raw.endsWith(BASE64_SUFFIX)) {
        return QString::fromLatin1(raw);
    }
    const auto encoded = raw.sliced(BASE64_PREFIX.size(), raw.size() - markers);
    auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return std::nullopt;
    }
    return QString::fromUtf8(*decoded);
}

QJsonObject errorResponse(const QJsonValue& id, int code, const QString& message, const QJsonValue& data)
{
    QJsonObject error{ { "code", code }, { "message", message } };
    if (!data.isUndefined() && !data.isNull()) {
        error["data"] = data;
    }
    return QJsonObject{ { "jsonrpc", "2.0" }, { "id", id.isUndefined() ? QJsonValue() : id }, { "error", error } };
}

Protocol::Protocol(const ToolRegistry* tools, ServerInfo info) : m_tools(tools), m_info(std::move(info)) {}

int Protocol::errorStatus(const Request& request, int code)
{
    switch (code) {
        case ERROR_PARSE:
        case ERROR_INVALID_REQUEST:
        case ERROR_HEADER_MISMATCH:
        case ERROR_UNSUPPORTED_VERSION:
            return 400;
        case ERROR_METHOD_NOT_FOUND:
            return request.modern ? 404 : 200;
        default:
            return 200;
    }
}

void Protocol::handle(const Incoming& incoming, const ExchangePtr& exchange)
{
    const auto& message = incoming.message;

    if (message.value("jsonrpc").toString() != "2.0") {
        exchange->reject(400, errorResponse({}, ERROR_INVALID_REQUEST, "\"jsonrpc\" must be \"2.0\""));
        return;
    }
    if (!message.contains("method")) {
        exchange->reject(400, errorResponse(message.value("id"), ERROR_INVALID_REQUEST, "Clients must not send JSON-RPC responses"));
        return;
    }
    if (!message.value("method").isString()) {
        exchange->reject(400, errorResponse(message.value("id"), ERROR_INVALID_REQUEST, "\"method\" must be a string"));
        return;
    }
    const auto paramsValue = message.value("params");
    if (!paramsValue.isUndefined() && !paramsValue.isObject()) {
        exchange->reject(400, errorResponse(message.value("id"), ERROR_INVALID_PARAMS, "\"params\" must be an object"));
        return;
    }

    // Notifications (no id) carry nothing this server acts on: cancellation on HTTP is closing the stream
    if (!message.contains("id")) {
        exchange->accept();
        return;
    }
    const auto id = message.value("id");
    if (!id.isString() && !id.isDouble()) {
        exchange->reject(400, errorResponse({}, ERROR_INVALID_REQUEST, "\"id\" must be a string or a number"));
        return;
    }

    Request request;
    request.id = id;
    request.method = message.value("method").toString();
    request.params = paramsValue.toObject();
    const auto meta = request.params.value("_meta").toObject();

    auto fail = [&](int code, const QString& text, const QJsonValue& data = {}) {
        exchange->reject(errorStatus(request, code), errorResponse(id, code, text, data));
    };

    if (const auto metaVersion = meta.value(META_VERSION); metaVersion.isString()) {
        request.modern = true;
        request.version = metaVersion.toString();
        request.clientName = meta.value(META_CLIENT_INFO).toObject().value("name").toString();

        if (!incoming.versionHeader) {
            fail(ERROR_HEADER_MISMATCH, "Header mismatch: MCP-Protocol-Version header is required");
            return;
        }
        if (QString::fromLatin1(*incoming.versionHeader) != request.version) {
            fail(ERROR_HEADER_MISMATCH, QString("Header mismatch: MCP-Protocol-Version header value '%1' does not match body value '%2'")
                                            .arg(QString::fromLatin1(*incoming.versionHeader), request.version));
            return;
        }
        if (!modernVersions().contains(request.version)) {
            fail(ERROR_UNSUPPORTED_VERSION, "Unsupported protocol version",
                 QJsonObject{ { "supported", QJsonArray::fromStringList(supportedVersions()) }, { "requested", request.version } });
            return;
        }
        if (!incoming.methodHeader) {
            fail(ERROR_HEADER_MISMATCH, "Header mismatch: Mcp-Method header is required");
            return;
        }
        if (QString::fromLatin1(*incoming.methodHeader) != request.method) {
            fail(ERROR_HEADER_MISMATCH, QString("Header mismatch: Mcp-Method header value '%1' does not match body value '%2'")
                                            .arg(QString::fromLatin1(*incoming.methodHeader), request.method));
            return;
        }
        if (requiresName(request.method)) {
            const auto bodyName = request.params.contains("uri") ? request.params.value("uri").toString()
                                                                  : request.params.value("name").toString();
            if (!incoming.nameHeader) {
                fail(ERROR_HEADER_MISMATCH, "Header mismatch: Mcp-Name header is required");
                return;
            }
            const auto headerName = decodeHeaderValue(*incoming.nameHeader);
            if (!headerName || *headerName != bodyName) {
                fail(ERROR_HEADER_MISMATCH, QString("Header mismatch: Mcp-Name header value '%1' does not match body value '%2'")
                                                .arg(QString::fromLatin1(*incoming.nameHeader), bodyName));
                return;
            }
        }
    } else if (request.method == "initialize") {
        const auto requested = request.params.value("protocolVersion").toString();
        request.version = legacyVersions().contains(requested) ? requested : QString(LATEST_LEGACY_VERSION);
        request.clientName = request.params.value("clientInfo").toObject().value("name").toString();
    } else {
        const auto headerVersion = incoming.versionHeader ? QString::fromLatin1(*incoming.versionHeader) : QString(HEADERLESS_VERSION);
        if (modernVersions().contains(headerVersion)) {
            fail(ERROR_HEADER_MISMATCH, QString("Header mismatch: MCP-Protocol-Version is '%1' but the body has no _meta[\"%2\"]")
                                            .arg(headerVersion, META_VERSION));
            return;
        }
        if (!legacyVersions().contains(headerVersion)) {
            fail(ERROR_UNSUPPORTED_VERSION, "Unsupported protocol version",
                 QJsonObject{ { "supported", QJsonArray::fromStringList(supportedVersions()) }, { "requested", headerVersion } });
            return;
        }
        request.version = headerVersion;
    }

    dispatch(request, incoming, exchange);
}

void Protocol::dispatch(const Request& request, const Incoming& incoming, const ExchangePtr& exchange)
{
    const auto& method = request.method;

    if (method == "server/discover") {
        exchange->finish(200, result(request, discoverResult(request)));
        return;
    }
    if (method == "initialize") {
        exchange->finish(200, result(request, initializeResult(request)));
        return;
    }
    if (method == "ping") {
        exchange->finish(200, result(request, {}));
        return;
    }
    if (method == "tools/list") {
        QJsonArray tools;
        for (const auto& tool : m_tools->tools()) {
            tools.append(tool.describe());
        }
        QJsonObject body{ { "tools", tools } };
        if (request.modern) {
            body["ttlMs"] = LIST_TTL_MS;
            body["cacheScope"] = "private";
        }
        exchange->finish(200, result(request, body));
        return;
    }
    if (method == "tools/call") {
        callTool(request, incoming, exchange);
        return;
    }

    exchange->reject(errorStatus(request, ERROR_METHOD_NOT_FOUND),
                     errorResponse(request.id, ERROR_METHOD_NOT_FOUND, QString("Method not found: %1").arg(method)));
}

void Protocol::callTool(const Request& request, const Incoming& incoming, const ExchangePtr& exchange)
{
    const auto nameValue = request.params.value("name");
    if (!nameValue.isString()) {
        exchange->reject(errorStatus(request, ERROR_INVALID_PARAMS),
                         errorResponse(request.id, ERROR_INVALID_PARAMS, "tools/call needs a string \"name\""));
        return;
    }
    const auto name = nameValue.toString();
    const auto* tool = m_tools->find(name);
    if (!tool) {
        const auto suggestions = m_tools->suggest(name);
        auto text = QString("Unknown tool: %1").arg(name);
        if (!suggestions.isEmpty()) {
            text += QString(" (did you mean %1?)").arg(suggestions.join(", "));
        }
        exchange->reject(errorStatus(request, ERROR_INVALID_PARAMS), errorResponse(request.id, ERROR_INVALID_PARAMS, text));
        return;
    }

    const auto argumentsValue = request.params.value("arguments");
    if (!argumentsValue.isUndefined() && !argumentsValue.isNull() && !argumentsValue.isObject()) {
        exchange->reject(errorStatus(request, ERROR_INVALID_PARAMS),
                         errorResponse(request.id, ERROR_INVALID_PARAMS, "tools/call \"arguments\" must be an object"));
        return;
    }
    const auto arguments = argumentsValue.toObject();

    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    auto replied = std::make_shared<bool>(false);

    CallRecord record;
    record.tokenName = incoming.token.name;
    record.clientName = request.clientName;
    record.tool = name;
    record.summary = summarize(arguments);

    auto reply = [this, request, exchange, timer, replied, record](const ToolResult& toolResult) mutable {
        if (*replied) {
            qWarning() << "MCP: tool" << record.tool << "replied twice; the second reply is dropped";
            return;
        }
        *replied = true;

        QJsonArray content;
        const auto text = toolResult.text.isEmpty() ? renderJson(toolResult.structured) : toolResult.text;
        content.append(QJsonObject{ { "type", "text" }, { "text", text } });
        for (const auto& extra : toolResult.extraContent) {
            content.append(extra);
        }
        QJsonObject body{ { "content", content }, { "isError", toolResult.isError } };
        if (!toolResult.structured.isUndefined()) {
            body["structuredContent"] = toolResult.structured;
        }
        exchange->finish(200, result(request, body));

        record.isError = toolResult.isError;
        record.error = toolResult.isError ? text : QString();
        record.durationMs = timer->elapsed();
        if (onToolCall) {
            onToolCall(record);
        }
    };

    CallContext context;
    context.token = incoming.token;
    context.clientName = request.clientName;
    context.protocolVersion = request.version;
    context.cancelled = exchange->cancelled();
    const auto progressToken = request.params.value("_meta").toObject().value("progressToken");
    if (progressToken.isString() || progressToken.isDouble()) {
        context.progress = [exchange, progressToken](double progress, std::optional<double> total, const QString& message) {
            QJsonObject params{ { "progressToken", progressToken }, { "progress", progress } };
            if (total) {
                params["total"] = *total;
            }
            if (!message.isEmpty()) {
                params["message"] = message;
            }
            exchange->notify(QJsonObject{ { "jsonrpc", "2.0" }, { "method", "notifications/progress" }, { "params", params } });
        };
    } else {
        context.progress = [](double, std::optional<double>, const QString&) {};
    }

    invokeTool(*tool, context, arguments, reply);
}

QJsonObject Protocol::result(const Request& request, QJsonObject body) const
{
    if (request.modern) {
        body["resultType"] = "complete";
        auto meta = body.value("_meta").toObject();
        meta[META_SERVER_INFO] = QJsonObject{ { "name", m_info.name }, { "version", m_info.version } };
        body["_meta"] = meta;
    }
    return QJsonObject{ { "jsonrpc", "2.0" }, { "id", request.id }, { "result", body } };
}

QJsonObject Protocol::capabilities() const
{
    return QJsonObject{ { "tools", QJsonObject{ { "listChanged", false } } } };
}

QJsonObject Protocol::discoverResult(const Request& request) const
{
    QJsonObject body{
        { "supportedVersions", QJsonArray::fromStringList(supportedVersions()) },
        { "capabilities", capabilities() },
        { "ttlMs", LIST_TTL_MS },
        { "cacheScope", "private" },
    };
    if (!m_info.instructions.isEmpty()) {
        body["instructions"] = m_info.instructions;
    }
    if (!request.modern) {
        // Legacy callers get no _meta from result(), so identify the server here as well
        body["_meta"] = QJsonObject{ { META_SERVER_INFO, QJsonObject{ { "name", m_info.name }, { "version", m_info.version } } } };
    }
    return body;
}

QJsonObject Protocol::initializeResult(const Request& request) const
{
    QJsonObject serverInfo{ { "name", m_info.name }, { "version", m_info.version } };
    if (!m_info.title.isEmpty()) {
        serverInfo["title"] = m_info.title;
    }
    QJsonObject body{
        { "protocolVersion", request.version },
        { "capabilities", capabilities() },
        { "serverInfo", serverInfo },
    };
    if (!m_info.instructions.isEmpty()) {
        body["instructions"] = m_info.instructions;
    }
    return body;
}

}  // namespace Mcp
