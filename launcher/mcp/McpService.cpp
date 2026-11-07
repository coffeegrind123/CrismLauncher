// SPDX-License-Identifier: GPL-3.0-only
#include "McpService.h"

#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QUrl>

#include <algorithm>

#include "BuildConfig.h"
#include "FileSystem.h"
#include "mcp/McpFileOps.h"
#include "mcp/McpGameTools.h"
#include "mcp/McpInstanceTools.h"
#include "mcp/McpLauncherTools.h"
#include "mcp/McpOps.h"
#include "mcp/McpWriteOps.h"
#include "settings/SettingsObject.h"

namespace {
constexpr auto SETTING_ENABLED = "McpEnabled";
constexpr auto SETTING_PORT = "McpPort";
constexpr auto TOKENS_FILE = "mcp/tokens.json";
constexpr auto AUDIT_LOG_FILE = "logs/mcp.log";
constexpr auto AUTH_REALM = "Bearer realm=\"Crism Launcher MCP\"";
constexpr qint64 RATE_WINDOW_MS = 60 * 1000;

// Headers a browser-based client (MCP Inspector) sends; listed for CORS preflight
constexpr auto ALLOWED_REQUEST_HEADERS =
    "Authorization, Content-Type, Accept, MCP-Protocol-Version, Mcp-Method, Mcp-Name, Mcp-Session-Id, Last-Event-ID";

using Mcp::Http::Headers;

bool isLoopbackHostName(const QString& host)
{
    if (host.compare("localhost", Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

// Host header: "127.0.0.1:47211", "localhost", "[::1]:47211". DNS rebinding sends the attacker's name here.
bool isLoopbackHost(const QByteArray& hostHeader)
{
    if (hostHeader.isEmpty()) {
        return false;
    }
    QUrl url;
    url.setAuthority(QString::fromLatin1(hostHeader), QUrl::StrictMode);
    return url.isValid() && url.userInfo().isEmpty() && isLoopbackHostName(url.host());
}

// Origin is sent by browsers. Only pages served from this machine's loopback may call; "null" (sandboxed frames,
// local files) and every remote origin are refused.
bool isAllowedOrigin(const QByteArray& origin)
{
    const QUrl url(QString::fromLatin1(origin), QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || !url.path().isEmpty()) {
        return false;
    }
    return isLoopbackHostName(url.host());
}

bool acceptsType(const QByteArray& accept, QByteArrayView type)
{
    if (accept.isEmpty()) {
        return true;
    }
    for (const auto& part : accept.split(',')) {
        const auto mediaType = part.split(';').first().trimmed();
        if (mediaType == type.toByteArray() || mediaType == "*/*") {
            return true;
        }
    }
    return false;
}

QByteArray jsonBytes(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

class HttpExchange : public Mcp::Exchange {
   public:
    HttpExchange(Mcp::Http::ResponderPtr responder, bool sseAllowed, Headers extraHeaders)
        : m_responder(std::move(responder)), m_sseAllowed(sseAllowed), m_headers(std::move(extraHeaders))
    {
        auto cancelled = m_cancelled;
        m_responder->onClientGone([cancelled] { *cancelled = true; });
    }

    void notify(const QJsonObject& notification) override
    {
        // Without SSE there is nowhere to put a notification before the response; progress is best effort
        if (!m_sseAllowed || !m_responder->isOpen()) {
            return;
        }
        if (!m_responder->isStreaming()) {
            m_responder->beginStream(m_headers);
        }
        m_responder->streamEvent(jsonBytes(notification));
    }

    void finish(int httpStatus, const QJsonObject& response) override
    {
        if (m_responder->isStreaming()) {
            m_responder->streamEvent(jsonBytes(response));
            m_responder->endStream();
            return;
        }
        m_responder->send(httpStatus, "application/json", jsonBytes(response), m_headers);
    }

    void accept() override { m_responder->sendEmpty(202, m_headers); }

    void reject(int httpStatus, const QJsonObject& response) override { finish(httpStatus, response); }

    std::shared_ptr<bool> cancelled() const override { return m_cancelled; }

   private:
    Mcp::Http::ResponderPtr m_responder;
    bool m_sseAllowed;
    Headers m_headers;
    std::shared_ptr<bool> m_cancelled = std::make_shared<bool>(false);
};
}  // namespace

McpService::McpService(SettingsObject* settings, QString dataRoot, QObject* parent)
    : QObject(parent), m_settings(settings), m_dataRoot(std::move(dataRoot)), m_tokens(FS::PathCombine(m_dataRoot, TOKENS_FILE))
{
    if (auto res = m_tokens.load(); !res) {
        qWarning() << "MCP: could not load tokens:" << res.error();
    }

    Mcp::registerLauncherTools(m_tools);
    Mcp::registerInstanceOps(m_ops);
    Mcp::registerWriteOps(m_ops);
    Mcp::registerGameOps(m_ops);
    Mcp::registerFileOps(m_ops);
    Mcp::registerOpTools(m_tools, &m_ops);

    Mcp::ServerInfo info;
    info.name = BuildConfig.LAUNCHER_APP_BINARY_NAME;
    info.title = BuildConfig.LAUNCHER_DISPLAYNAME;
    info.version = BuildConfig.printableVersionString();
    info.instructions = instructions();
    m_protocol = std::make_unique<Mcp::Protocol>(&m_tools, info);
    m_protocol->onToolCall = [this](const Mcp::CallRecord& record) {
        m_activeCalls = std::max(0, m_activeCalls - 1);
        recordCall(record);
        emit activityChanged();
    };
    m_protocol->onToolStart = [this](const Mcp::CallRecord& record) {
        ++m_activeCalls;
        m_lastStarted = record;
        emit activityChanged();
    };
    m_protocol->gate = [this]() -> std::optional<QString> {
        if (m_paused) {
            return tr("The user stopped AI agents in the launcher.");
        }
        return std::nullopt;
    };
    m_protocol->prepareContext = [this](Mcp::CallContext& context) {
        const auto tokenName = context.token.name;
        const auto clientName = context.clientName;
        context.confirm = [this, tokenName, clientName](const QString& action, std::function<void(bool, const QString&)> done) {
            if (!m_prompter) {
                done(false, tr("the launcher window is not available to ask the user"));
                return;
            }
            m_prompter(McpConfirmRequest{ tokenName, clientName, action },
                       [done](bool allowed) { done(allowed, allowed ? QString() : tr("the user declined")); });
        };
    };
    connect(&m_jobs, &Mcp::JobRegistry::jobChanged, this, &McpService::activityChanged);

    m_server = new Mcp::Http::Server([this](const auto& request, const auto& responder) { handleHttp(request, responder); }, this);

    applySettings();
}

McpService::~McpService() = default;

void McpService::registerSettings(SettingsObject* settings)
{
    settings->registerSetting(SETTING_ENABLED, false);
    // 0 means "pick a free port on first start"; the chosen port is then saved so client configs stay valid
    settings->registerSetting(SETTING_PORT, 0);
}

void McpService::applySettings()
{
    const bool enabled = m_settings->get(SETTING_ENABLED).toBool();
    const int configuredPort = m_settings->get(SETTING_PORT).toInt();

    if (!enabled) {
        if (m_server->isListening()) {
            m_server->close();
            qInfo() << "MCP: endpoint stopped";
        }
        m_lastError.clear();
        emit stateChanged();
        return;
    }

    if (m_server->isListening() && (configuredPort == 0 || static_cast<int>(m_server->port()) == configuredPort)) {
        return;
    }

    const auto port = static_cast<quint16>(std::clamp(configuredPort, 0, 65535));
    QString error;
    if (!m_server->listen(port, &error)) {
        m_lastError = tr("Could not listen on 127.0.0.1:%1: %2").arg(port).arg(error);
        qWarning() << "MCP:" << m_lastError;
        emit stateChanged();
        return;
    }
    m_lastError.clear();
    if (configuredPort == 0) {
        m_settings->set(SETTING_PORT, m_server->port());
    }
    qInfo() << "MCP: endpoint listening on" << endpointUrl();
    emit stateChanged();
}

bool McpService::isListening() const
{
    return m_server->isListening();
}

quint16 McpService::port() const
{
    return m_server->isListening() ? m_server->port() : static_cast<quint16>(m_settings->get(SETTING_PORT).toInt());
}

QString McpService::endpointUrl() const
{
    return QString("http://127.0.0.1:%1%2").arg(port()).arg(QLatin1String(ENDPOINT_PATH));
}

Result<> McpService::saveTokens()
{
    return m_tokens.save();
}

void McpService::stopAgents()
{
    m_paused = true;
    m_jobs.cancelAll();
    qInfo() << "MCP: the user stopped agents";
    emit activityChanged();
}

void McpService::resumeAgents()
{
    m_paused = false;
    qInfo() << "MCP: the user resumed agents";
    emit activityChanged();
}

bool McpService::allowRequest(const QString& tokenId)
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto& [windowStart, count] = m_rate[tokenId];
    if (now - windowStart >= RATE_WINDOW_MS) {
        windowStart = now;
        count = 0;
    }
    return ++count <= RATE_LIMIT_PER_MINUTE;
}

void McpService::handleHttp(const Mcp::Http::Request& request, const Mcp::Http::ResponderPtr& responder)
{
    auto plain = [&responder](int status, const QByteArray& message, const Headers& headers = {}) {
        responder->send(status, "application/json", jsonBytes(QJsonObject{ { "error", QString::fromUtf8(message) } }), headers);
    };

    if (!isLoopbackHost(request.header("host"))) {
        plain(403, "Host must be a loopback address");
        return;
    }

    Headers cors;
    if (request.hasHeader("origin")) {
        const auto origin = request.header("origin");
        if (!isAllowedOrigin(origin)) {
            plain(403, "Origin not allowed");
            return;
        }
        cors = {
            { "Access-Control-Allow-Origin", origin },
            { "Vary", "Origin" },
            { "Access-Control-Expose-Headers", "WWW-Authenticate" },
        };
    }

    const auto path = request.path();
    if (path != ENDPOINT_PATH && path != QByteArray(ENDPOINT_PATH) + "/") {
        plain(404, "Not found; the MCP endpoint is " + QByteArray(ENDPOINT_PATH), cors);
        return;
    }

    if (request.method == "OPTIONS") {
        auto headers = cors;
        headers.emplace_back("Access-Control-Allow-Methods", "POST, OPTIONS");
        headers.emplace_back("Access-Control-Allow-Headers", ALLOWED_REQUEST_HEADERS);
        headers.emplace_back("Access-Control-Max-Age", "600");
        responder->sendEmpty(204, headers);
        return;
    }
    if (request.method != "POST") {
        // GET (standalone SSE stream) and DELETE (session end) belong to the 2025 transport; this server has neither
        auto headers = cors;
        headers.emplace_back("Allow", "POST, OPTIONS");
        plain(405, "Use POST", headers);
        return;
    }

    const auto authorization = request.header("authorization");
    std::optional<Mcp::TokenInfo> token;
    if (authorization.size() > 7 && authorization.left(7).compare("Bearer ", Qt::CaseInsensitive) == 0) {
        token = m_tokens.verify(QString::fromLatin1(authorization.mid(7).trimmed()));
    }
    if (!token) {
        auto headers = cors;
        headers.emplace_back("WWW-Authenticate", AUTH_REALM);
        plain(401, "Missing or invalid bearer token. Create one in the launcher: Settings > AI agents (MCP).", headers);
        return;
    }

    if (!allowRequest(token->id)) {
        auto headers = cors;
        headers.emplace_back("Retry-After", "60");
        plain(429, "Rate limit exceeded", headers);
        return;
    }

    const auto contentType = request.header("content-type").split(';').first().trimmed().toLower();
    if (contentType != "application/json") {
        plain(415, "Content-Type must be application/json", cors);
        return;
    }

    const auto accept = request.joinedHeader("accept");
    if (!acceptsType(accept, "application/json") && !acceptsType(accept, "text/event-stream")) {
        plain(406, "Accept must allow application/json or text/event-stream", cors);
        return;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(request.body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        responder->send(400, "application/json",
                        jsonBytes(Mcp::errorResponse({}, Mcp::ERROR_PARSE, "Parse error: " + parseError.errorString())), cors);
        return;
    }
    if (!document.isObject()) {
        responder->send(400, "application/json",
                        jsonBytes(Mcp::errorResponse({}, Mcp::ERROR_INVALID_REQUEST, "The body must be a single JSON-RPC message (no batches)")),
                        cors);
        return;
    }

    Mcp::Incoming incoming;
    incoming.message = document.object();
    incoming.token = *token;
    if (request.hasHeader("mcp-protocol-version")) {
        incoming.versionHeader = request.header("mcp-protocol-version");
    }
    if (request.hasHeader("mcp-method")) {
        incoming.methodHeader = request.header("mcp-method");
    }
    if (request.hasHeader("mcp-name")) {
        incoming.nameHeader = request.header("mcp-name");
    }

    const bool sseAllowed = !accept.isEmpty() && acceptsType(accept, "text/event-stream");
    m_protocol->handle(incoming, std::make_shared<HttpExchange>(responder, sseAllowed, cors));
}

void McpService::recordCall(const Mcp::CallRecord& record)
{
    m_recentCalls.append(record);
    while (m_recentCalls.size() > MAX_RECENT_CALLS) {
        m_recentCalls.removeFirst();
    }

    const auto line = QString("%1\t%2\t%3\t%4\t%5\t%6ms\t%7\n")
                          .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate), record.tokenName,
                               record.clientName.isEmpty() ? QStringLiteral("-") : record.clientName, record.tool,
                               record.isError ? QStringLiteral("error") : QStringLiteral("ok"))
                          .arg(record.durationMs)
                          .arg(record.isError ? record.error.left(300).simplified() : record.summary);
    QFile log(FS::PathCombine(m_dataRoot, AUDIT_LOG_FILE));
    if (log.open(QIODevice::Append | QIODevice::Text)) {
        log.write(line.toUtf8());
    }

    emit callRecorded(record);
}

QString McpService::instructions() const
{
    // Kept under 4,096 characters: Claude Code truncates server instructions there
    return QStringLiteral(
               "This server controls %1, a Minecraft launcher running on the user's computer. The user may be using its window "
               "at the same time as you.\n"
               "Start with launcher_info (what exists, what this token may do), then instances (ids for everything else).\n"
               "Direct tools cover the common work: instance_get, search_content / content_versions (Modrinth, CurseForge), "
               "settings, jobs. Everything else is an op in a catalog: find ops with search_ops, run one with call, run several "
               "with batch (\"$N.path\" passes an earlier step's result on).\n"
               "Ids: instances are addressed by the id from `instances`, never by name. Mod-platform projects are addressed by "
               "platform plus projectId.\n"
               "Slow work (installs, downloads) returns a job id at once; follow it with jobs action=wait.\n"
               "Permissions: each token has scopes (read, instances, content, launch, settings, accounts, files). A tool "
               "outside the token's scopes returns an error naming the scope; ask the user to grant it rather than retrying.\n"
               "Secrets (account tokens, API keys, proxy passwords) are never returned, and settings that can run programs or "
               "redirect downloads can only be changed by the user.\n"
               "Errors with isError=true carry a hint on how to recover; follow it.")
        .arg(BuildConfig.LAUNCHER_DISPLAYNAME);
}
