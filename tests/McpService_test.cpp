#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <mcp/McpJobs.h>
#include <mcp/McpService.h>
#include <settings/INISettingsObject.h>

namespace {
// A task the test finishes by hand
class ManualTask : public Task {
    Q_OBJECT
   public:
    void succeed() { emitSucceeded(); }

   protected:
    void executeTask() override {}
};

// Pumps the event loop until `buffer` holds `needle`
bool waitFor(QTcpSocket& socket, QByteArray& buffer, const QByteArray& needle)
{
    QElapsedTimer timer;
    timer.start();
    while (!buffer.contains(needle) && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        buffer += socket.readAll();
    }
    return buffer.contains(needle);
}

struct Response {
    int status = 0;
    QByteArray head;
    QByteArray body;

    QByteArray header(const QByteArray& name) const
    {
        for (const auto& line : head.split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon > 0 && line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0) {
                return line.mid(colon + 1).trimmed();
            }
        }
        return {};
    }
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

// Pumps the event loop (the server runs on this thread) until one response is complete
Response readResponse(QTcpSocket& socket, QByteArray& buffer)
{
    QElapsedTimer timer;
    timer.start();
    Response response;
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        buffer += socket.readAll();
        const auto end = buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            continue;
        }
        response.head = buffer.left(end);
        response.status = response.head.split(' ').value(1).toInt();
        const auto lengthText = response.header("content-length");
        if (lengthText.isEmpty()) {
            // Streams end by closing the connection
            if (socket.state() != QAbstractSocket::UnconnectedState) {
                continue;
            }
            response.body = buffer.mid(end + 4);
            buffer.clear();
            return response;
        }
        const auto length = lengthText.toInt();
        if (buffer.size() < end + 4 + length) {
            continue;
        }
        response.body = buffer.mid(end + 4, length);
        buffer.remove(0, end + 4 + length);
        return response;
    }
    return response;
}
}  // namespace

class McpServiceTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<INISettingsObject> m_settings;
    std::unique_ptr<McpService> m_service;
    QString m_secret;
    QString m_tokenId;

    QByteArray post(const QByteArray& body, const QList<QByteArray>& extraHeaders, bool withToken = true, const QByteArray& host = {}) const
    {
        QByteArray raw = "POST /mcp HTTP/1.1\r\n";
        raw += "Host: " + (host.isEmpty() ? "127.0.0.1:" + QByteArray::number(m_service->port()) : host) + "\r\n";
        raw += "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n";
        if (withToken) {
            raw += "Authorization: Bearer " + m_secret.toLatin1() + "\r\n";
        }
        for (const auto& header : extraHeaders) {
            raw += header + "\r\n";
        }
        raw += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        return raw;
    }

    Response send(const QByteArray& raw)
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, m_service->port());
        QElapsedTimer timer;
        timer.start();
        while (socket.state() != QAbstractSocket::ConnectedState && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        socket.write(raw);
        QByteArray buffer;
        return readResponse(socket, buffer);
    }

    static QByteArray legacyInitialize()
    {
        return QJsonDocument(QJsonObject{ { "jsonrpc", "2.0" },
                                          { "id", 1 },
                                          { "method", "initialize" },
                                          { "params", QJsonObject{ { "protocolVersion", "2025-11-25" },
                                                                   { "capabilities", QJsonObject{} },
                                                                   { "clientInfo", QJsonObject{ { "name", "t" }, { "version", "1" } } } } } })
            .toJson(QJsonDocument::Compact);
    }

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_settings = std::make_unique<INISettingsObject>(m_dir.filePath("launcher.cfg"));
        McpService::registerSettings(m_settings.get());
        m_settings->set("McpEnabled", true);
        m_service = std::make_unique<McpService>(m_settings.get(), m_dir.path());
        QVERIFY2(m_service->isListening(), qPrintable(m_service->lastError()));
        QVERIFY(m_service->port() != 0);
        QCOMPARE(m_settings->get("McpPort").toInt(), static_cast<int>(m_service->port()));

        const auto [token, secret] = m_service->tokens().create("test", Mcp::Scope::Read, Mcp::ConfirmPolicy::NeverAsk);
        m_secret = secret;
        m_tokenId = token.id;
        QVERIFY(m_service->saveTokens().has_value());
    }

    void rejectsMissingToken()
    {
        const auto response = send(post(legacyInitialize(), {}, false));
        QCOMPARE(response.status, 401);
        QVERIFY(response.header("www-authenticate").startsWith("Bearer"));
    }

    void rejectsWrongToken()
    {
        auto raw = post(legacyInitialize(), {}, false);
        raw.replace("Content-Type:", "Authorization: Bearer crism_mcp_wrong\r\nContent-Type:");
        QCOMPARE(send(raw).status, 401);
    }

    void rejectsForeignHost()
    {
        QCOMPARE(send(post(legacyInitialize(), {}, true, "evil.example:80")).status, 403);
    }

    void origins()
    {
        QCOMPARE(send(post(legacyInitialize(), { "Origin: https://evil.example" })).status, 403);
        QCOMPARE(send(post(legacyInitialize(), { "Origin: null" })).status, 403);
        const auto allowed = send(post(legacyInitialize(), { "Origin: http://localhost:6274" }));
        QCOMPARE(allowed.status, 200);
        QCOMPARE(allowed.header("access-control-allow-origin"), QByteArray("http://localhost:6274"));
    }

    void preflight()
    {
        const auto response = send("OPTIONS /mcp HTTP/1.1\r\nHost: localhost\r\nOrigin: http://127.0.0.1:6274\r\n\r\n");
        QCOMPARE(response.status, 204);
        QVERIFY(response.header("access-control-allow-headers").contains("Mcp-Method"));
    }

    void methodsAndPaths()
    {
        QCOMPARE(send("GET /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n").status, 405);
        QCOMPARE(send("DELETE /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n").status, 405);
        QCOMPARE(send("POST /other HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n").status, 404);
    }

    void rejectsWrongContentType()
    {
        auto raw = post(legacyInitialize(), {});
        raw.replace("Content-Type: application/json", "Content-Type: text/plain");
        QCOMPARE(send(raw).status, 415);
    }

    void parseError()
    {
        const auto response = send(post("{not json", {}));
        QCOMPARE(response.status, 400);
        QCOMPARE(response.json()["error"].toObject()["code"].toInt(), -32700);
    }

    void legacyInitialize_()
    {
        const auto response = send(post(legacyInitialize(), {}));
        QCOMPARE(response.status, 200);
        QCOMPARE(response.header("content-type"), QByteArray("application/json"));
        QVERIFY(response.header("mcp-session-id").isEmpty());
        QCOMPARE(response.json()["result"].toObject()["protocolVersion"].toString(), QString("2025-11-25"));
    }

    void notificationIsAccepted()
    {
        const auto body = QJsonDocument(QJsonObject{ { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }).toJson();
        QCOMPARE(send(post(body, { "MCP-Protocol-Version: 2025-11-25" })).status, 202);
    }

    void modernToolsListOnKeepAlive()
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, m_service->port());
        QVERIFY(socket.waitForConnected(5000));

        const QJsonObject meta{ { "io.modelcontextprotocol/protocolVersion", "2026-07-28" },
                                { "io.modelcontextprotocol/clientCapabilities", QJsonObject{} } };
        const auto discover = QJsonDocument(QJsonObject{
                                                { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "server/discover" }, { "params", QJsonObject{ { "_meta", meta } } } })
                                  .toJson(QJsonDocument::Compact);
        const auto list = QJsonDocument(QJsonObject{
                                            { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/list" }, { "params", QJsonObject{ { "_meta", meta } } } })
                              .toJson(QJsonDocument::Compact);

        // Both requests written at once: the second must wait for the first and reuse the connection
        socket.write(post(discover, { "MCP-Protocol-Version: 2026-07-28", "Mcp-Method: server/discover" }) +
                     post(list, { "MCP-Protocol-Version: 2026-07-28", "Mcp-Method: tools/list" }));

        QByteArray buffer;
        const auto first = readResponse(socket, buffer);
        QCOMPARE(first.status, 200);
        QCOMPARE(first.json()["id"].toInt(), 1);
        QCOMPARE(first.json()["result"].toObject()["resultType"].toString(), QString("complete"));

        const auto second = readResponse(socket, buffer);
        QCOMPARE(second.status, 200);
        QCOMPARE(second.json()["id"].toInt(), 2);
        QStringList names;
        for (const auto& tool : second.json()["result"].toObject()["tools"].toArray()) {
            names.append(tool.toObject()["name"].toString());
        }
        QVERIFY(names.contains("launcher_info"));
        QVERIFY(names.contains("instances"));
        QCOMPARE(names, [&names] {
            auto sorted = names;
            sorted.sort();
            return sorted;
        }());
    }

    void taskSubscriptionStreams()
    {
        auto task = makeShared<ManualTask>();
        const auto jobId = m_service->jobs()->start(m_tokenId, "test", "A job", task);

        const QJsonObject meta{ { "io.modelcontextprotocol/protocolVersion", "2026-07-28" },
                                { "io.modelcontextprotocol/clientCapabilities",
                                  QJsonObject{ { "extensions", QJsonObject{ { "io.modelcontextprotocol/tasks", QJsonObject{} } } } } } };
        const auto listen = QJsonDocument(QJsonObject{ { "jsonrpc", "2.0" },
                                                       { "id", 7 },
                                                       { "method", "subscriptions/listen" },
                                                       { "params", QJsonObject{ { "_meta", meta },
                                                                                { "notifications", QJsonObject{ { "taskIds", QJsonArray{ jobId, "not-mine" } } } } } } })
                                .toJson(QJsonDocument::Compact);

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, m_service->port());
        QVERIFY(socket.waitForConnected(5000));
        socket.write(post(listen, { "MCP-Protocol-Version: 2026-07-28", "Mcp-Method: subscriptions/listen" }));

        QByteArray buffer;
        QVERIFY(waitFor(socket, buffer, "notifications/subscriptions/acknowledged"));
        QVERIFY(buffer.contains("text/event-stream"));
        QVERIFY(buffer.contains(jobId.toLatin1()));
        QVERIFY(!buffer.contains("not-mine"));

        task->succeed();
        QVERIFY(waitFor(socket, buffer, "notifications/tasks"));
        QVERIFY(waitFor(socket, buffer, "\"completed\""));
        QVERIFY(buffer.contains("subscriptionId\":7"));
    }

    void auditLogHasNoSecrets()
    {
        // tools/list is not a tool call; nothing in the log may ever contain the bearer secret
        QFile log(m_dir.filePath("logs/mcp.log"));
        if (log.open(QIODevice::ReadOnly)) {
            QVERIFY(!log.readAll().contains(m_secret.toLatin1()));
        }
    }

    void disableClosesSocket()
    {
        const auto port = m_service->port();
        m_settings->set("McpEnabled", false);
        m_service->applySettings();
        QVERIFY(!m_service->isListening());

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(!socket.waitForConnected(1000));

        m_settings->set("McpEnabled", true);
        m_service->applySettings();
        QVERIFY(m_service->isListening());
        QCOMPARE(m_service->port(), port);
    }
};

QTEST_GUILESS_MAIN(McpServiceTest)

#include "McpService_test.moc"
