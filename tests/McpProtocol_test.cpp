#include <QJsonArray>
#include <QTest>

#include <stdexcept>

#include <mcp/McpProtocol.h>
#include <mcp/McpSchema.h>

using namespace Mcp;

namespace {
struct Outcome {
    enum Kind { None, Finished, Accepted, Rejected } kind = None;
    int status = 0;
    QJsonObject response;
    QList<QJsonObject> notifications;
};

class FakeExchange : public Exchange {
   public:
    explicit FakeExchange(Outcome* outcome) : m_outcome(outcome) {}
    void notify(const QJsonObject& notification) override { m_outcome->notifications.append(notification); }
    void finish(int httpStatus, const QJsonObject& response) override
    {
        m_outcome->kind = Outcome::Finished;
        m_outcome->status = httpStatus;
        m_outcome->response = response;
    }
    void accept() override
    {
        m_outcome->kind = Outcome::Accepted;
        m_outcome->status = 202;
    }
    void reject(int httpStatus, const QJsonObject& response) override
    {
        m_outcome->kind = Outcome::Rejected;
        m_outcome->status = httpStatus;
        m_outcome->response = response;
    }
    std::shared_ptr<bool> cancelled() const override { return m_cancelled; }
    bool canStream() const override { return m_canStream; }

    bool m_canStream = true;

   private:
    Outcome* m_outcome;
    std::shared_ptr<bool> m_cancelled = std::make_shared<bool>(false);
};

QJsonObject modernMeta()
{
    return QJsonObject{ { "io.modelcontextprotocol/protocolVersion", MODERN_VERSION },
                        { "io.modelcontextprotocol/clientInfo", QJsonObject{ { "name", "test-client" }, { "version", "1" } } },
                        { "io.modelcontextprotocol/clientCapabilities", QJsonObject{} } };
}

QJsonObject request(const QString& method, QJsonObject params = {}, const QJsonValue& id = 1)
{
    QJsonObject message{ { "jsonrpc", "2.0" }, { "id", id }, { "method", method } };
    message["params"] = params;
    return message;
}
}  // namespace

class McpProtocolTest : public QObject {
    Q_OBJECT

    ToolRegistry m_tools;
    std::unique_ptr<Protocol> m_protocol;
    QList<CallRecord> m_records;

    Outcome run(const Incoming& incoming)
    {
        Outcome outcome;
        m_protocol->handle(incoming, std::make_shared<FakeExchange>(&outcome));
        return outcome;
    }

    Incoming modern(const QString& method, QJsonObject params = {}, const QByteArray& name = {}) const
    {
        params["_meta"] = modernMeta();
        Incoming in;
        in.message = request(method, params);
        in.versionHeader = QByteArray(MODERN_VERSION);
        in.methodHeader = method.toLatin1();
        if (!name.isEmpty()) {
            in.nameHeader = name;
        }
        in.token.name = "test";
        in.token.scopes = Scope::Read;
        return in;
    }

    Incoming legacy(const QString& method, const QJsonObject& params = {}, const QByteArray& version = "2025-11-25") const
    {
        Incoming in;
        in.message = request(method, params);
        if (!version.isEmpty()) {
            in.versionHeader = version;
        }
        in.token.name = "test";
        in.token.scopes = Scope::Read;
        return in;
    }

   private slots:
    void initTestCase()
    {
        Tool echo;
        echo.name = "echo";
        echo.title = "Echo";
        echo.description = "Echoes its text";
        echo.inputSchema = Schema::object({ { "text", Schema::string("text") } }, { "text" });
        echo.scope = Scope::Read;
        echo.handler = [](const CallContext& context, const QJsonObject& arguments, const ToolReply& reply) {
            context.progress(1, 2, "half");
            reply(ToolResult::ok(QJsonObject{ { "text", arguments["text"] }, { "client", context.clientName } }));
        };
        m_tools.add(echo);

        Tool files;
        files.name = "delete_files";
        files.description = "Needs the files scope";
        files.inputSchema = Schema::object({});
        files.annotations.readOnly = false;
        files.annotations.destructive = true;
        files.scope = Scope::Files;
        files.handler = [](const CallContext&, const QJsonObject&, const ToolReply& reply) { reply(ToolResult::ok(QJsonObject{})); };
        m_tools.add(files);

        Tool starter;
        starter.name = "start_job";
        starter.description = "Starts a job";
        starter.inputSchema = Schema::object({});
        starter.handler = [](const CallContext&, const QJsonObject&, const ToolReply& reply) {
            auto result = ToolResult::ok(QJsonObject{ { "job", "j1" } });
            result.jobId = "j1";
            reply(result);
        };
        m_tools.add(starter);

        Tool thrower;
        thrower.name = "throws";
        thrower.description = "Throws";
        thrower.inputSchema = Schema::object({});
        thrower.handler = [](const CallContext&, const QJsonObject&, const ToolReply&) { throw std::runtime_error("boom"); };
        m_tools.add(thrower);

        m_protocol = std::make_unique<Protocol>(&m_tools, ServerInfo{ "crism", "Crism Launcher", "1.0", "Use it well" });
        m_protocol->onToolCall = [this](const CallRecord& record) { m_records.append(record); };
    }

    void legacyInitializeNegotiates()
    {
        auto in = legacy("initialize", QJsonObject{ { "protocolVersion", "2025-06-18" }, { "clientInfo", QJsonObject{ { "name", "old" } } } }, {});
        auto out = run(in);
        QCOMPARE(out.kind, Outcome::Finished);
        QCOMPARE(out.status, 200);
        const auto result = out.response["result"].toObject();
        QCOMPARE(result["protocolVersion"].toString(), QString("2025-06-18"));
        QCOMPARE(result["serverInfo"].toObject()["name"].toString(), QString("crism"));
        QCOMPARE(result["instructions"].toString(), QString("Use it well"));
        QVERIFY(result["capabilities"].toObject().contains("tools"));
        QVERIFY(!result.contains("resultType"));

        in = legacy("initialize", QJsonObject{ { "protocolVersion", "2099-01-01" } }, {});
        QCOMPARE(run(in).response["result"].toObject()["protocolVersion"].toString(), QString(LATEST_LEGACY_VERSION));
    }

    void legacyWithoutHeaderIsOldest()
    {
        const auto out = run(legacy("tools/list", {}, {}));
        QCOMPARE(out.status, 200);
        QCOMPARE(out.response["result"].toObject()["tools"].toArray().size(), 4);
    }

    void modernDiscover()
    {
        const auto out = run(modern("server/discover"));
        QCOMPARE(out.status, 200);
        const auto result = out.response["result"].toObject();
        QCOMPARE(result["resultType"].toString(), QString("complete"));
        QVERIFY(result["supportedVersions"].toArray().contains(QJsonValue(MODERN_VERSION)));
        QVERIFY(result["supportedVersions"].toArray().contains(QJsonValue("2025-11-25")));
        QCOMPARE(result["_meta"].toObject()["io.modelcontextprotocol/serverInfo"].toObject()["name"].toString(), QString("crism"));
        QVERIFY(result.contains("ttlMs"));
        QCOMPARE(result["cacheScope"].toString(), QString("private"));
    }

    void modernToolsListIsSortedAndCacheable()
    {
        const auto out = run(modern("tools/list"));
        const auto result = out.response["result"].toObject();
        const auto tools = result["tools"].toArray();
        QCOMPARE(tools.size(), 4);
        QCOMPARE(tools[0].toObject()["name"].toString(), QString("delete_files"));
        QCOMPARE(tools[1].toObject()["name"].toString(), QString("echo"));
        QCOMPARE(tools[2].toObject()["name"].toString(), QString("start_job"));
        QCOMPARE(tools[3].toObject()["name"].toString(), QString("throws"));
        QVERIFY(tools[0].toObject()["annotations"].toObject()["destructiveHint"].toBool());
        QVERIFY(!tools[1].toObject()["annotations"].toObject().contains("destructiveHint"));
        QVERIFY(result.contains("ttlMs"));
        QCOMPARE(result["cacheScope"].toString(), QString("private"));
    }

    void headerValidation_data()
    {
        QTest::addColumn<QString>("which");
        QTest::addColumn<int>("code");
        QTest::addColumn<int>("status");

        QTest::newRow("missing version header") << "noVersion" << ERROR_HEADER_MISMATCH << 400;
        QTest::newRow("version mismatch") << "badVersion" << ERROR_HEADER_MISMATCH << 400;
        QTest::newRow("missing method header") << "noMethod" << ERROR_HEADER_MISMATCH << 400;
        QTest::newRow("method mismatch") << "badMethod" << ERROR_HEADER_MISMATCH << 400;
        QTest::newRow("missing name header") << "noName" << ERROR_HEADER_MISMATCH << 400;
        QTest::newRow("name mismatch") << "badName" << ERROR_HEADER_MISMATCH << 400;
        QTest::newRow("unsupported version") << "future" << ERROR_UNSUPPORTED_VERSION << 400;
        QTest::newRow("modern header, legacy body") << "headerOnly" << ERROR_HEADER_MISMATCH << 400;
        QTest::newRow("unknown legacy version") << "legacyUnknown" << ERROR_UNSUPPORTED_VERSION << 400;
    }

    void headerValidation()
    {
        QFETCH(QString, which);
        QFETCH(int, code);
        QFETCH(int, status);

        auto in = modern("tools/call", QJsonObject{ { "name", "echo" }, { "arguments", QJsonObject{ { "text", "x" } } } }, "echo");
        if (which == "noVersion") {
            in.versionHeader.reset();
        } else if (which == "badVersion") {
            in.versionHeader = QByteArray("2025-11-25");
        } else if (which == "noMethod") {
            in.methodHeader.reset();
        } else if (which == "badMethod") {
            in.methodHeader = QByteArray("tools/list");
        } else if (which == "noName") {
            in.nameHeader.reset();
        } else if (which == "badName") {
            in.nameHeader = QByteArray("other");
        } else if (which == "future") {
            auto params = in.message["params"].toObject();
            auto meta = params["_meta"].toObject();
            meta["io.modelcontextprotocol/protocolVersion"] = "2099-01-01";
            params["_meta"] = meta;
            in.message["params"] = params;
            in.versionHeader = QByteArray("2099-01-01");
        } else if (which == "headerOnly") {
            in = legacy("tools/list", {}, MODERN_VERSION);
        } else if (which == "legacyUnknown") {
            in = legacy("tools/list", {}, "2024-01-01");
        }

        const auto out = run(in);
        QCOMPARE(out.kind, Outcome::Rejected);
        QCOMPARE(out.status, status);
        QCOMPARE(out.response["error"].toObject()["code"].toInt(), code);
        if (code == ERROR_UNSUPPORTED_VERSION) {
            QVERIFY(out.response["error"].toObject()["data"].toObject()["supported"].toArray().contains(QJsonValue(MODERN_VERSION)));
        }
    }

    void base64NameHeader()
    {
        auto in = modern("tools/call", QJsonObject{ { "name", "echo" }, { "arguments", QJsonObject{ { "text", "x" } } } },
                         "=?base64?" + QByteArray("echo").toBase64() + "?=");
        QCOMPARE(run(in).kind, Outcome::Finished);
        QVERIFY(!decodeHeaderValue("=?base64?!!!?=").has_value());
        QCOMPARE(decodeHeaderValue("plain").value_or(QString()), QString("plain"));
    }

    void callToolWithProgress()
    {
        m_records.clear();
        auto in = modern("tools/call", QJsonObject{ { "name", "echo" }, { "arguments", QJsonObject{ { "text", "hi" } } } }, "echo");
        auto params = in.message["params"].toObject();
        auto meta = params["_meta"].toObject();
        meta["progressToken"] = "p1";
        params["_meta"] = meta;
        in.message["params"] = params;

        const auto out = run(in);
        QCOMPARE(out.kind, Outcome::Finished);
        QCOMPARE(out.notifications.size(), 1);
        const auto progress = out.notifications.first()["params"].toObject();
        QCOMPARE(progress["progressToken"].toString(), QString("p1"));
        QCOMPARE(progress["progress"].toDouble(), 1.0);
        QCOMPARE(progress["total"].toDouble(), 2.0);
        QCOMPARE(progress["message"].toString(), QString("half"));

        const auto result = out.response["result"].toObject();
        QCOMPARE(result["resultType"].toString(), QString("complete"));
        QVERIFY(!result["isError"].toBool());
        QCOMPARE(result["structuredContent"].toObject()["text"].toString(), QString("hi"));
        QCOMPARE(result["structuredContent"].toObject()["client"].toString(), QString("test-client"));
        QCOMPARE(result["content"].toArray().first().toObject()["type"].toString(), QString("text"));

        QCOMPARE(m_records.size(), 1);
        QCOMPARE(m_records.first().tool, QString("echo"));
        QCOMPARE(m_records.first().clientName, QString("test-client"));
        QVERIFY(!m_records.first().isError);
    }

    void noProgressWithoutToken()
    {
        const auto out = run(modern("tools/call", QJsonObject{ { "name", "echo" }, { "arguments", QJsonObject{ { "text", "hi" } } } }, "echo"));
        QVERIFY(out.notifications.isEmpty());
    }

    void toolErrors_data()
    {
        QTest::addColumn<QString>("tool");
        QTest::addColumn<QJsonObject>("arguments");
        QTest::addColumn<QString>("contains");

        QTest::newRow("missing scope") << "delete_files" << QJsonObject{} << "'files' scope";
        QTest::newRow("invalid arguments") << "echo" << QJsonObject{ { "text", 1 } } << "/text: expected string";
        QTest::newRow("unknown argument") << "echo" << QJsonObject{ { "text", "a" }, { "extra", 1 } } << "unknown property 'extra'";
        QTest::newRow("exception") << "throws" << QJsonObject{} << "boom";
    }

    void toolErrors()
    {
        QFETCH(QString, tool);
        QFETCH(QJsonObject, arguments);
        QFETCH(QString, contains);

        const auto out = run(modern("tools/call", QJsonObject{ { "name", tool }, { "arguments", arguments } }, tool.toLatin1()));
        QCOMPARE(out.kind, Outcome::Finished);
        QCOMPARE(out.status, 200);
        const auto result = out.response["result"].toObject();
        QVERIFY(result["isError"].toBool());
        const auto text = result["content"].toArray().first().toObject()["text"].toString();
        QVERIFY2(text.contains(contains), qPrintable(text));
    }

    void unknownToolSuggests()
    {
        const auto out = run(modern("tools/call", QJsonObject{ { "name", "ecco" } }, "ecco"));
        QCOMPARE(out.kind, Outcome::Rejected);
        QCOMPARE(out.response["error"].toObject()["code"].toInt(), ERROR_INVALID_PARAMS);
        QVERIFY(out.response["error"].toObject()["message"].toString().contains("did you mean echo"));
    }

    void unknownMethod()
    {
        auto out = run(modern("resources/list"));
        QCOMPARE(out.status, 404);
        QCOMPARE(out.response["error"].toObject()["code"].toInt(), ERROR_METHOD_NOT_FOUND);

        out = run(legacy("resources/list"));
        QCOMPARE(out.status, 200);
        QCOMPARE(out.response["error"].toObject()["code"].toInt(), ERROR_METHOD_NOT_FOUND);
    }

    void notificationsAreAccepted()
    {
        Incoming in;
        in.message = QJsonObject{ { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } };
        QCOMPARE(run(in).kind, Outcome::Accepted);
    }

    void malformedMessages_data()
    {
        QTest::addColumn<QJsonObject>("message");

        QTest::newRow("no jsonrpc") << QJsonObject{ { "id", 1 }, { "method", "ping" } };
        QTest::newRow("response") << QJsonObject{ { "jsonrpc", "2.0" }, { "id", 1 }, { "result", QJsonObject{} } };
        QTest::newRow("method not string") << QJsonObject{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", 3 } };
        QTest::newRow("params not object") << QJsonObject{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "ping" }, { "params", 3 } };
        QTest::newRow("null id") << QJsonObject{ { "jsonrpc", "2.0" }, { "id", QJsonValue::Null }, { "method", "ping" } };
    }

    void malformedMessages()
    {
        QFETCH(QJsonObject, message);
        Incoming in;
        in.message = message;
        const auto out = run(in);
        QCOMPARE(out.kind, Outcome::Rejected);
        QCOMPARE(out.status, 400);
    }

   private:
    Incoming withCapabilities(Incoming in, const QJsonObject& capabilities) const
    {
        auto params = in.message["params"].toObject();
        auto meta = params["_meta"].toObject();
        meta["io.modelcontextprotocol/clientCapabilities"] = capabilities;
        params["_meta"] = meta;
        in.message["params"] = params;
        return in;
    }

    Incoming deleteCall(const QJsonObject& arguments, const QJsonObject& retry = {}) const
    {
        QJsonObject params{ { "name", "delete_files" }, { "arguments", arguments } };
        for (auto it = retry.begin(); it != retry.end(); ++it) {
            params[it.key()] = it.value();
        }
        auto in = withCapabilities(modern("tools/call", params, "delete_files"), QJsonObject{ { "elicitation", QJsonObject{} } });
        in.token.id = "token-1";
        in.token.scopes = Scope::Read | Scope::Files;
        in.token.policy = ConfirmPolicy::AskDestructive;
        in.token.chatConfirmations = true;
        return in;
    }

   private slots:
    void chatConfirmationRoundTrip()
    {
        // 1. A destructive call from a client that can show forms comes back as a question
        auto out = run(deleteCall(QJsonObject{}));
        QCOMPARE(out.kind, Outcome::Finished);
        auto result = out.response["result"].toObject();
        QCOMPARE(result["resultType"].toString(), QString("input_required"));
        const auto requests = result["inputRequests"].toObject();
        QCOMPARE(requests.size(), 1);
        const auto key = requests.keys().first();
        const auto elicitation = requests[key].toObject();
        QCOMPARE(elicitation["method"].toString(), QString("elicitation/create"));
        QCOMPARE(elicitation["params"].toObject()["mode"].toString(), QString("form"));
        const auto state = result["requestState"].toString();
        QVERIFY(!state.isEmpty());

        // 2. The retry carries the user's yes: the tool runs
        auto accept = QJsonObject{ { key, QJsonObject{ { "action", "accept" }, { "content", QJsonObject{ { "allow", true } } } } } };
        out = run(deleteCall(QJsonObject{}, QJsonObject{ { "inputResponses", accept }, { "requestState", state } }));
        result = out.response["result"].toObject();
        QCOMPARE(result["resultType"].toString(), QString("complete"));
        QVERIFY(!result["isError"].toBool());

        // 3. A no is refused
        auto decline = QJsonObject{ { key, QJsonObject{ { "action", "decline" } } } };
        out = run(deleteCall(QJsonObject{}, QJsonObject{ { "inputResponses", decline }, { "requestState", state } }));
        result = out.response["result"].toObject();
        QVERIFY(result["isError"].toBool());
        QVERIFY(result["content"].toArray()[0].toObject()["text"].toString().contains("declined"));

        // 4. A forged state is refused
        auto forged = state;
        forged[forged.size() - 2] = forged[forged.size() - 2] == 'A' ? 'B' : 'A';
        out = run(deleteCall(QJsonObject{}, QJsonObject{ { "inputResponses", accept }, { "requestState", forged } }));
        QVERIFY(out.response["result"].toObject()["isError"].toBool());

        // 5. The answer does not carry over to other arguments: asked again
        out = run(deleteCall(QJsonObject{ { "extra", 1 } }, QJsonObject{ { "inputResponses", accept }, { "requestState", state } }));
        QVERIFY(out.response["result"].toObject()["isError"].toBool() ||
                out.response["result"].toObject()["resultType"].toString() == "input_required");
    }

    void noChatWithoutElicitationOrWhenTurnedOff()
    {
        auto in = deleteCall(QJsonObject{});
        in = withCapabilities(in, QJsonObject{});
        auto out = run(in);
        // No form support and no launcher prompter in this test: refused, never silently allowed
        QVERIFY(out.response["result"].toObject()["isError"].toBool());

        in = deleteCall(QJsonObject{});
        in.token.chatConfirmations = false;
        out = run(in);
        QVERIFY(out.response["result"].toObject()["isError"].toBool());
    }

    void tasks()
    {
        m_protocol->taskState = [](const QString& id, const QString& token) -> std::optional<QJsonObject> {
            if (id != "j1" || token != "token-1") {
                return std::nullopt;
            }
            return QJsonObject{ { "taskId", "j1" }, { "status", "working" }, { "createdAt", "2026-11-08T00:00:00Z" },
                                { "lastUpdatedAt", "2026-11-08T00:00:00Z" }, { "ttlMs", 1000 }, { "pollIntervalMs", 500 } };
        };
        bool cancelled = false;
        m_protocol->taskCancel = [&cancelled](const QString&, const QString&) { return cancelled = true; };

        auto call = modern("tools/call", QJsonObject{ { "name", "start_job" }, { "arguments", QJsonObject{} } }, "start_job");
        call.token.id = "token-1";

        // Without the extension: the ordinary result
        auto out = run(call);
        QCOMPARE(out.response["result"].toObject()["resultType"].toString(), QString("complete"));

        // With it: a task
        out = run(withCapabilities(call, QJsonObject{ { "extensions", QJsonObject{ { TASKS_EXTENSION, QJsonObject{} } } } }));
        auto result = out.response["result"].toObject();
        QCOMPARE(result["resultType"].toString(), QString("task"));
        QCOMPARE(result["taskId"].toString(), QString("j1"));
        QCOMPARE(result["status"].toString(), QString("working"));

        auto get = modern("tasks/get", QJsonObject{ { "taskId", "j1" } }, "j1");
        get.token.id = "token-1";
        out = run(get);
        QCOMPARE(out.response["result"].toObject()["status"].toString(), QString("working"));
        QCOMPARE(out.response["result"].toObject()["resultType"].toString(), QString("complete"));

        get.token.id = "someone-else";
        QCOMPARE(run(get).kind, Outcome::Rejected);

        auto cancel = modern("tasks/cancel", QJsonObject{ { "taskId", "j1" } }, "j1");
        cancel.token.id = "token-1";
        QCOMPARE(run(cancel).kind, Outcome::Finished);
        QVERIFY(cancelled);

        // Mcp-Name mirrors the task id
        auto wrongName = modern("tasks/get", QJsonObject{ { "taskId", "j1" } }, "j2");
        QCOMPARE(run(wrongName).response["error"].toObject()["code"].toInt(), ERROR_HEADER_MISMATCH);

        m_protocol->taskState = nullptr;
        m_protocol->taskCancel = nullptr;
    }

    void subscriptions()
    {
        QJsonObject seenFilter;
        m_protocol->listen = [&seenFilter](const QJsonValue&, const QJsonObject& filter, const TokenInfo&, const ExchangePtr&) { seenFilter = filter; };

        auto listen = modern("subscriptions/listen", QJsonObject{ { "notifications", QJsonObject{ { "taskIds", QJsonArray{ "j1" } } } } });
        auto out = run(listen);
        QCOMPARE(out.response["error"].toObject()["code"].toInt(), ERROR_MISSING_CAPABILITY);

        out = run(withCapabilities(listen, QJsonObject{ { "extensions", QJsonObject{ { TASKS_EXTENSION, QJsonObject{} } } } }));
        QCOMPARE(out.kind, Outcome::None);
        QCOMPARE(seenFilter["taskIds"].toArray().size(), 1);

        Outcome noStream;
        auto exchange = std::make_shared<FakeExchange>(&noStream);
        exchange->m_canStream = false;
        m_protocol->handle(modern("subscriptions/listen", QJsonObject{ { "notifications", QJsonObject{} } }), exchange);
        QCOMPARE(noStream.kind, Outcome::Rejected);

        m_protocol->listen = nullptr;
    }
};

QTEST_GUILESS_MAIN(McpProtocolTest)

#include "McpProtocol_test.moc"
