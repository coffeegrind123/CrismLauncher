// SPDX-License-Identifier: GPL-3.0-only
#include "McpProtocol.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
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
constexpr auto META_CLIENT_CAPABILITIES = "io.modelcontextprotocol/clientCapabilities";
// How long a confirmation question stays answerable
constexpr qint64 CONFIRM_STATE_TTL_MS = 15 * 60 * 1000;
constexpr int STATE_KEY_BYTES = 32;
constexpr auto CONFIRM_FIELD = "allow";

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
    return method == "tools/call" || method == "resources/read" || method == "prompts/get" || method.startsWith("tasks/");
}

// The body value the Mcp-Name header mirrors
QString nameFor(const QString& method, const QJsonObject& params)
{
    if (method.startsWith("tasks/")) {
        return params.value("taskId").toString();
    }
    return params.contains("uri") ? params.value("uri").toString() : params.value("name").toString();
}

QByteArray digest(const QJsonObject& object)
{
    return QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
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

Protocol::Protocol(const ToolRegistry* tools, ServerInfo info) : m_tools(tools), m_info(std::move(info))
{
    m_stateKey.resize(STATE_KEY_BYTES);
    for (auto& byte : m_stateKey) {
        byte = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }
}

int Protocol::errorStatus(const Request& request, int code)
{
    switch (code) {
        case ERROR_PARSE:
        case ERROR_INVALID_REQUEST:
        case ERROR_HEADER_MISMATCH:
        case ERROR_UNSUPPORTED_VERSION:
        case ERROR_MISSING_CAPABILITY:
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
        request.clientCapabilities = meta.value(META_CLIENT_CAPABILITIES).toObject();

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
            const auto bodyName = nameFor(request.method, request.params);
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
    if (m_resources && method == "resources/list") {
        QJsonObject body{ { "resources", m_resources->listJson() } };
        if (request.modern) {
            body["ttlMs"] = LIST_TTL_MS;
            body["cacheScope"] = "private";
        }
        exchange->finish(200, result(request, body));
        return;
    }
    if (m_resources && method == "resources/templates/list") {
        QJsonObject body{ { "resourceTemplates", m_resources->templatesJson() } };
        if (request.modern) {
            body["ttlMs"] = LIST_TTL_MS;
            body["cacheScope"] = "private";
        }
        exchange->finish(200, result(request, body));
        return;
    }
    if (m_resources && method == "resources/read") {
        readResource(request, incoming, exchange);
        return;
    }
    if (m_prompts && method == "prompts/list") {
        QJsonObject body{ { "prompts", m_prompts->listJson() } };
        if (request.modern) {
            body["ttlMs"] = LIST_TTL_MS;
            body["cacheScope"] = "private";
        }
        exchange->finish(200, result(request, body));
        return;
    }
    if (m_prompts && method == "prompts/get") {
        getPrompt(request, incoming, exchange);
        return;
    }
    if (taskState && (method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel")) {
        taskMethod(request, incoming, exchange);
        return;
    }
    if (listen && method == "subscriptions/listen") {
        subscribe(request, incoming, exchange);
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

    const auto tokenId = incoming.token.id;
    auto reply = [this, request, exchange, timer, replied, record, tokenId](const ToolResult& toolResult) mutable {
        if (*replied) {
            qWarning() << "MCP: tool" << record.tool << "replied twice; the second reply is dropped";
            return;
        }
        *replied = true;

        // The user must answer through the agent's client first: an MRTR InputRequiredResult, not a tool result
        if (toolResult.inputRequired) {
            exchange->finish(200, result(request, *toolResult.inputRequired, QStringLiteral("input_required")));
            record.summary += " (asked the user)";
            record.durationMs = timer->elapsed();
            if (onToolCall) {
                onToolCall(record);
            }
            return;
        }

        // Tasks extension: a call that started a job becomes a task the client polls or subscribes to
        if (!toolResult.jobId.isEmpty() && supportsTasks(request) && taskState) {
            if (auto task = taskState(toolResult.jobId, tokenId)) {
                QJsonObject body;
                for (const auto* key : { "taskId", "status", "statusMessage", "createdAt", "lastUpdatedAt", "ttlMs", "pollIntervalMs" }) {
                    if (task->contains(key)) {
                        body[key] = task->value(key);
                    }
                }
                exchange->finish(200, result(request, body, QStringLiteral("task")));
                record.summary += " (task " + toolResult.jobId + ")";
                record.durationMs = timer->elapsed();
                if (onToolCall) {
                    onToolCall(record);
                }
                return;
            }
        }

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

    if (gate) {
        if (const auto reason = gate()) {
            reply(ToolResult::error(*reason, "Wait until the user resumes agents in the launcher."));
            return;
        }
    }
    if (prepareContext) {
        prepareContext(context);
    }
    // Clients that can show a form ask the user themselves (MRTR), unless the user keeps confirmations in the launcher
    if (supportsFormElicitation(request) && incoming.token.chatConfirmations) {
        context.confirm = [this, request, tokenId, name, arguments](const QStringList& actions, const ConfirmAnswer& answer) {
            answer(chatConfirm(request, tokenId, name, arguments, actions));
        };
    }
    if (onToolStart) {
        onToolStart(record);
    }
    invokeTool(*tool, context, arguments, reply);
}

void Protocol::readResource(const Request& request, const Incoming& incoming, const ExchangePtr& exchange)
{
    if (!incoming.token.scopes.testFlag(Scope::Read)) {
        exchange->reject(errorStatus(request, ERROR_INVALID_REQUEST),
                         errorResponse(request.id, ERROR_INVALID_REQUEST, "Reading resources needs the 'read' scope"));
        return;
    }
    const auto uri = request.params.value("uri").toString();
    // The reader may answer later; `this` outlives every exchange (the service owns both)
    m_resources->read(uri, [this, request, exchange](Result<ResourceContent> content) {
        if (!content) {
            // 2026-07-28 reports a missing resource as invalid params (-32602), no longer -32002
            exchange->reject(errorStatus(request, ERROR_INVALID_PARAMS), errorResponse(request.id, ERROR_INVALID_PARAMS, content.error(),
                                                                                       QJsonObject{ { "uri", request.params.value("uri") } }));
            return;
        }
        QJsonObject body{ { "contents", QJsonArray{ QJsonObject{ { "uri", content->uri }, { "mimeType", content->mimeType }, { "text", content->text } } } } };
        if (request.modern) {
            body["ttlMs"] = 0;
            body["cacheScope"] = "private";
        }
        exchange->finish(200, result(request, body));
    });
}

void Protocol::getPrompt(const Request& request, const Incoming& incoming, const ExchangePtr& exchange)
{
    if (!incoming.token.scopes.testFlag(Scope::Read)) {
        exchange->reject(errorStatus(request, ERROR_INVALID_REQUEST),
                         errorResponse(request.id, ERROR_INVALID_REQUEST, "Prompts need the 'read' scope"));
        return;
    }
    auto prompt = m_prompts->get(request.params.value("name").toString(), request.params.value("arguments").toObject());
    if (!prompt) {
        exchange->reject(errorStatus(request, ERROR_INVALID_PARAMS), errorResponse(request.id, ERROR_INVALID_PARAMS, prompt.error()));
        return;
    }
    exchange->finish(200, result(request, *prompt));
}

QJsonObject Protocol::result(const Request& request, QJsonObject body, const QString& resultType) const
{
    if (request.modern) {
        body["resultType"] = resultType;
        auto meta = body.value("_meta").toObject();
        meta[META_SERVER_INFO] = QJsonObject{ { "name", m_info.name }, { "version", m_info.version } };
        body["_meta"] = meta;
    }
    return QJsonObject{ { "jsonrpc", "2.0" }, { "id", request.id }, { "result", body } };
}

bool Protocol::supportsFormElicitation(const Request& request)
{
    // `elicitation: {}` means form mode; a client that lists modes must list "form"
    const auto elicitation = request.clientCapabilities.value("elicitation");
    if (!request.modern || !elicitation.isObject()) {
        return false;
    }
    const auto modes = elicitation.toObject();
    return modes.isEmpty() || modes.contains("form");
}

bool Protocol::supportsTasks(const Request& request)
{
    return request.modern && request.clientCapabilities.value("extensions").toObject().value(TASKS_EXTENSION).isObject();
}

QString Protocol::signState(const QJsonObject& payload) const
{
    const auto data = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const auto mac = QMessageAuthenticationCode::hash(data, m_stateKey, QCryptographicHash::Sha256);
    const auto encoding = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    return QString::fromLatin1(data.toBase64(encoding) + '.' + mac.toBase64(encoding));
}

std::optional<QJsonObject> Protocol::verifyState(const QString& state) const
{
    const auto parts = state.toLatin1().split('.');
    if (parts.size() != 2) {
        return std::nullopt;
    }
    const auto encoding = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    const auto data = QByteArray::fromBase64Encoding(parts[0], encoding | QByteArray::AbortOnBase64DecodingErrors);
    const auto mac = QByteArray::fromBase64Encoding(parts[1], encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!data || !mac) {
        return std::nullopt;
    }
    const auto expected = QMessageAuthenticationCode::hash(*data, m_stateKey, QCryptographicHash::Sha256);
    // Constant time: the comparison must not reveal how much of a forged code was right
    if (expected.size() != mac->size()) {
        return std::nullopt;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(expected[i] ^ (*mac)[i]);
    }
    if (diff != 0) {
        return std::nullopt;
    }
    return QJsonDocument::fromJson(*data).object();
}

ConfirmVerdict Protocol::chatConfirm(const Request& request, const QString& tokenId, const QString& tool, const QJsonObject& arguments,
                                     const QStringList& actions) const
{
    // One question per distinct set of actions; the same call retried asks the same question under the same key
    const auto key = QString("confirm-%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(actions.join('\n').toUtf8(), QCryptographicHash::Sha256).toHex().left(16)));
    const auto argumentsDigest = QString::fromLatin1(digest(arguments));
    const auto now = QDateTime::currentMSecsSinceEpoch();

    if (const auto stateText = request.params.value("requestState").toString(); !stateText.isEmpty()) {
        const auto state = verifyState(stateText);
        if (!state) {
            // A forged or mangled state is refused outright (spec: reject state that fails verification)
            return ConfirmVerdict::denied("the confirmation could not be verified");
        }
        // Bound to this token, this tool, these exact arguments and this question, and only for a while
        const bool matches = state->value("t").toString() == tokenId && state->value("n").toString() == tool &&
                             state->value("d").toString() == argumentsDigest && state->value("k").toString() == key &&
                             state->value("x").toInteger() >= now;
        const auto response = request.params.value("inputResponses").toObject().value(key).toObject();
        if (matches && !response.isEmpty()) {
            const bool allowed = response.value("action").toString() == "accept" && response.value("content").toObject().value(CONFIRM_FIELD).toBool(false);
            return allowed ? ConfirmVerdict::allowed()
                           : ConfirmVerdict::denied(QString("the user declined in %1").arg(request.clientName.isEmpty() ? QString("the client") : request.clientName));
        }
        // Expired, for other arguments, or unanswered: ask again
    }

    QString message = actions.size() == 1 ? QString("Allow this change in the Minecraft launcher?\n\n%1").arg(actions.first())
                                          : QString("Allow these changes in the Minecraft launcher?\n\n- %1").arg(actions.join("\n- "));
    const QJsonObject elicitation{
        { "method", "elicitation/create" },
        { "params", QJsonObject{ { "mode", "form" },
                                 { "message", message },
                                 { "requestedSchema",
                                   QJsonObject{ { "type", "object" },
                                                { "properties", QJsonObject{ { CONFIRM_FIELD, QJsonObject{ { "type", "boolean" },
                                                                                                          { "title", "Allow" },
                                                                                                          { "description", "Allow the change(s) above" },
                                                                                                          { "default", false } } } } },
                                                { "required", QJsonArray{ CONFIRM_FIELD } } } } } },
    };
    const auto state = signState(QJsonObject{ { "v", 1 },
                                              { "t", tokenId },
                                              { "n", tool },
                                              { "d", argumentsDigest },
                                              { "k", key },
                                              { "x", now + CONFIRM_STATE_TTL_MS } });
    return ConfirmVerdict::needsInput(QJsonObject{ { "inputRequests", QJsonObject{ { key, elicitation } } }, { "requestState", state } });
}

void Protocol::taskMethod(const Request& request, const Incoming& incoming, const ExchangePtr& exchange)
{
    const auto taskId = request.params.value("taskId").toString();
    const auto task = taskState(taskId, incoming.token.id);
    if (!task) {
        exchange->reject(errorStatus(request, ERROR_INVALID_PARAMS),
                         errorResponse(request.id, ERROR_INVALID_PARAMS, QString("Unknown task: %1").arg(taskId)));
        return;
    }
    if (request.method == "tasks/get") {
        exchange->finish(200, result(request, *task));
        return;
    }
    if (request.method == "tasks/cancel" && taskCancel) {
        taskCancel(taskId, incoming.token.id);
    }
    // tasks/update: this server never puts a task in input_required (confirmations finish before the task starts), so
    // every response is for a key that is not outstanding and is ignored, as the extension specifies
    exchange->finish(200, result(request, {}));
}

void Protocol::subscribe(const Request& request, const Incoming& incoming, const ExchangePtr& exchange)
{
    if (!incoming.token.scopes.testFlag(Scope::Read)) {
        exchange->reject(errorStatus(request, ERROR_INVALID_REQUEST),
                         errorResponse(request.id, ERROR_INVALID_REQUEST, "Subscriptions need the 'read' scope"));
        return;
    }
    const auto filter = request.params.value("notifications").toObject();
    if (filter.contains("taskIds") && !supportsTasks(request)) {
        exchange->reject(errorStatus(request, ERROR_MISSING_CAPABILITY),
                         errorResponse(request.id, ERROR_MISSING_CAPABILITY, "Missing required client capability",
                                       QJsonObject{ { "requiredCapabilities",
                                                      QJsonObject{ { "extensions", QJsonObject{ { TASKS_EXTENSION, QJsonObject{} } } } } } }));
        return;
    }
    if (!exchange->canStream()) {
        exchange->reject(errorStatus(request, ERROR_INVALID_REQUEST),
                         errorResponse(request.id, ERROR_INVALID_REQUEST, "subscriptions/listen needs an SSE response (Accept: text/event-stream)"));
        return;
    }
    listen(request.id, filter, incoming.token, exchange);
}

QJsonObject Protocol::capabilities() const
{
    QJsonObject out{ { "tools", QJsonObject{ { "listChanged", false } } } };
    if (m_resources) {
        out["resources"] = QJsonObject{ { "listChanged", false }, { "subscribe", static_cast<bool>(listen) } };
    }
    if (taskState) {
        out["extensions"] = QJsonObject{ { TASKS_EXTENSION, QJsonObject{} } };
    }
    if (m_prompts) {
        out["prompts"] = QJsonObject{ { "listChanged", false } };
    }
    return out;
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
