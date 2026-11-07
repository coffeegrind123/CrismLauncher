// SPDX-License-Identifier: GPL-3.0-only
#include "McpStdioRelay.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <thread>
#include <utility>

#include "FileSystem.h"
#include "mcp/McpProtocol.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
constexpr int RETRY_INTERVAL_MS = 500;
constexpr int READ_CHUNK = 64 * 1024;

// Native handles: a GUI-subsystem binary on Windows may have no C runtime streams bound to the pipes it was given
void writeStdout(const QByteArray& data)
{
    qsizetype written = 0;
    while (written < data.size()) {
#ifdef Q_OS_WIN
        DWORD n = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data.constData() + written, static_cast<DWORD>(data.size() - written), &n, nullptr) || n == 0) {
            return;
        }
#else
        const auto n = ::write(STDOUT_FILENO, data.constData() + written, static_cast<size_t>(data.size() - written));
        if (n <= 0) {
            return;
        }
#endif
        written += static_cast<qsizetype>(n);
    }
}

// Reads stdin on its own thread and hands complete lines to the relay on the main thread
void startReader(McpStdioRelay* relay, std::function<void(QByteArray)> onLine, std::function<void()> onClosed)
{
    std::thread([relay, onLine, onClosed] {
        QByteArray buffer;
        char chunk[READ_CHUNK];
        while (true) {
#ifdef Q_OS_WIN
            DWORD n = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), chunk, sizeof(chunk), &n, nullptr) || n == 0) {
                break;
            }
#else
            const auto n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n <= 0) {
                break;
            }
#endif
            buffer.append(chunk, static_cast<qsizetype>(n));
            qsizetype newline;
            while ((newline = buffer.indexOf('\n')) >= 0) {
                auto line = buffer.left(newline);
                buffer.remove(0, newline + 1);
                if (line.endsWith('\r')) {
                    line.chop(1);
                }
                if (!line.trimmed().isEmpty()) {
                    QMetaObject::invokeMethod(relay, [onLine, line] { onLine(line); }, Qt::QueuedConnection);
                }
            }
        }
        QMetaObject::invokeMethod(relay, [onClosed] { onClosed(); }, Qt::QueuedConnection);
    }).detach();
}

bool isHeaderSafe(const QString& value)
{
    if (value.isEmpty() || value.trimmed() != value) {
        return false;
    }
    for (const auto c : value) {
        if (c.unicode() < 0x20 || c.unicode() > 0x7E) {
            return false;
        }
    }
    return true;
}

QJsonObject errorFor(const QJsonValue& id, const QString& message)
{
    return Mcp::errorResponse(id, Mcp::ERROR_INTERNAL, message);
}
}  // namespace

McpStdioRelay::McpStdioRelay(QString dataPath, QStringList launcherArguments, QObject* parent)
    : QObject(parent), m_dataPath(std::move(dataPath)), m_launcherArguments(std::move(launcherArguments)), m_network(new QNetworkAccessManager(this))
{
    // Loopback only: never route the relay through a system proxy
    m_network->setProxy(QNetworkProxy::NoProxy);
}

McpStdioRelay::~McpStdioRelay() = default;

QList<std::pair<QByteArray, QByteArray>> McpStdioRelay::headersFor(const QJsonObject& message, const QString& legacyVersion)
{
    QList<std::pair<QByteArray, QByteArray>> headers;
    const auto method = message.value("method").toString();
    const auto params = message.value("params").toObject();
    const auto metaVersion = params.value("_meta").toObject().value("io.modelcontextprotocol/protocolVersion").toString();

    if (!metaVersion.isEmpty()) {
        headers.emplace_back("MCP-Protocol-Version", metaVersion.toLatin1());
    } else if (method != "initialize" && !legacyVersion.isEmpty()) {
        headers.emplace_back("MCP-Protocol-Version", legacyVersion.toLatin1());
    }
    if (!method.isEmpty()) {
        headers.emplace_back("Mcp-Method", method.toUtf8());
    }
    if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
        const auto name = params.contains("uri") ? params.value("uri").toString() : params.value("name").toString();
        headers.emplace_back("Mcp-Name", isHeaderSafe(name) ? name.toLatin1() : "=?base64?" + name.toUtf8().toBase64() + "?=");
    }
    return headers;
}

void McpStdioRelay::start()
{
    startReader(
        this, [this](const QByteArray& line) { onLine(line); }, [this] { onInputClosed(); });
    m_connectStarted = QDateTime::currentMSecsSinceEpoch();
    tryConnect();
}

bool McpStdioRelay::readConfig()
{
    QFile file(FS::PathCombine(m_dataPath, CONFIG_FILE));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const auto config = QJsonDocument::fromJson(file.readAll()).object();
    m_port = static_cast<quint16>(config.value("port").toInt());
    m_token = config.value("token").toString();
    return m_port != 0 && !m_token.isEmpty();
}

void McpStdioRelay::tryConnect()
{
    if (readConfig()) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, m_port);
        if (probe.waitForConnected(RETRY_INTERVAL_MS)) {
            m_state = State::Ready;
            const auto pending = std::exchange(m_pending, {});
            for (const auto& line : pending) {
                forward(line);
            }
            return;
        }
    }

    // Not reachable: start the launcher once (it opens the endpoint if the user enabled it), then keep trying
    if (!m_launched) {
        m_launched = true;
        QProcess::startDetached(QCoreApplication::applicationFilePath(), m_launcherArguments);
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_connectStarted > STARTUP_TIMEOUT_MS) {
        m_state = State::Failed;
        m_failure = tr("The launcher's MCP endpoint is not available. Enable it and \"stdio clients\" in the launcher: "
                       "Settings > AI agents (MCP).");
        const auto pending = std::exchange(m_pending, {});
        for (const auto& line : pending) {
            failRequest(line, m_failure);
        }
        finishIfDone();
        return;
    }
    QTimer::singleShot(RETRY_INTERVAL_MS, this, &McpStdioRelay::tryConnect);
}

void McpStdioRelay::onLine(const QByteArray& line)
{
    switch (m_state) {
        case State::Connecting:
            m_pending.append(line);
            return;
        case State::Ready:
            forward(line);
            return;
        case State::Failed:
            failRequest(line, m_failure);
            return;
    }
}

void McpStdioRelay::onInputClosed()
{
    m_inputClosed = true;
    finishIfDone();
}

void McpStdioRelay::finishIfDone()
{
    if (m_inputClosed && m_inFlight == 0 && (m_state != State::Connecting || m_pending.isEmpty())) {
        QCoreApplication::exit(0);
    }
}

void McpStdioRelay::failRequest(const QByteArray& line, const QString& reason)
{
    const auto message = QJsonDocument::fromJson(line).object();
    // Notifications get no answer
    if (message.contains("id")) {
        write(QJsonDocument(errorFor(message.value("id"), reason)).toJson(QJsonDocument::Compact));
    }
}

void McpStdioRelay::write(const QByteArray& message)
{
    writeStdout(message + '\n');
}

void McpStdioRelay::forward(const QByteArray& line)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        write(QJsonDocument(Mcp::errorResponse({}, Mcp::ERROR_PARSE, "Parse error")).toJson(QJsonDocument::Compact));
        return;
    }
    const auto message = document.object();
    const auto id = message.value("id");
    const bool isInitialize = message.value("method").toString() == "initialize";

    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/mcp").arg(m_port)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json, text/event-stream");
    request.setRawHeader("Authorization", "Bearer " + m_token.toLatin1());
    for (const auto& [name, value] : headersFor(message, m_legacyVersion)) {
        request.setRawHeader(name, value);
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(0);

    ++m_inFlight;
    auto* reply = m_network->post(request, line);
    auto buffer = std::make_shared<QByteArray>();

    // SSE responses carry progress before the result; pass each event on as soon as it is complete
    auto drainEvents = [this, buffer](bool final) {
        while (true) {
            auto end = buffer->indexOf("\n\n");
            if (end < 0 && final && !buffer->trimmed().isEmpty()) {
                end = buffer->size();
            }
            if (end < 0) {
                return;
            }
            QByteArrayList data;
            for (const auto& field : buffer->left(end).split('\n')) {
                if (field.startsWith("data:")) {
                    data.append(field.mid(5).trimmed());
                }
            }
            buffer->remove(0, std::min(end + 2, buffer->size()));
            if (!data.isEmpty()) {
                write(data.join('\n'));
            }
        }
    };

    connect(reply, &QNetworkReply::readyRead, this, [reply, buffer, drainEvents] {
        buffer->append(reply->readAll());
        if (reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("text/event-stream")) {
            drainEvents(false);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, buffer, drainEvents, id, isInitialize] {
        reply->deleteLater();
        buffer->append(reply->readAll());
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool hasId = !id.isUndefined();

        if (reply->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("text/event-stream")) {
            drainEvents(true);
        } else if (status == 202 || (buffer->isEmpty() && !hasId)) {
            // An accepted notification: nothing to answer
        } else {
            const auto body = QJsonDocument::fromJson(*buffer).object();
            if (body.contains("jsonrpc")) {
                if (isInitialize) {
                    m_legacyVersion = body.value("result").toObject().value("protocolVersion").toString();
                }
                write(QJsonDocument(body).toJson(QJsonDocument::Compact));
            } else if (hasId) {
                const auto reason = body.value("error").toString(reply->errorString());
                write(QJsonDocument(errorFor(id, QString("Launcher endpoint answered %1: %2").arg(status).arg(reason))).toJson(QJsonDocument::Compact));
            }
        }
        --m_inFlight;
        finishIfDone();
    });
}
