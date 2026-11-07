#include <QJsonArray>
#include <QTest>

#include <mcp/McpOps.h>
#include <mcp/McpSchema.h>
#include <mcp/McpTool.h>

using namespace Mcp;

class McpOpsTest : public QObject {
    Q_OBJECT

    ToolRegistry m_ops;
    ToolRegistry m_tools;
    QStringList m_ran;

    QStringList m_asked;

    ToolResult run(const QString& toolName,
                   const QJsonObject& arguments,
                   Scopes scopes = Scope::Read,
                   ConfirmPolicy policy = ConfirmPolicy::NeverAsk,
                   std::optional<bool> answer = std::nullopt)
    {
        CallContext context;
        context.token.id = "t";
        context.token.scopes = scopes;
        context.token.policy = policy;
        context.progress = [](double, std::optional<double>, const QString&) {};
        if (answer) {
            context.confirm = [this, allowed = *answer](const QString& action, std::function<void(bool, const QString&)> done) {
                m_asked.append(action);
                done(allowed, allowed ? QString() : QString("declined"));
            };
        }
        ToolResult out;
        bool replied = false;
        invokeTool(*m_tools.find(toolName), context, arguments, [&out, &replied](const ToolResult& result) {
            out = result;
            replied = true;
        });
        // Every op here replies synchronously
        Q_ASSERT(replied);
        return out;
    }

    static QJsonObject step(const QString& op, const QJsonObject& arguments = {})
    {
        return QJsonObject{ { "op", op }, { "arguments", arguments } };
    }

   private slots:
    void initTestCase()
    {
        Tool make;
        make.name = "items.make";
        make.description = "Makes two items. Used by tests.";
        make.inputSchema = Schema::object({});
        make.handler = [this](const CallContext&, const QJsonObject&, const ToolReply& reply) {
            m_ran.append("items.make");
            reply(ToolResult::ok(QJsonObject{ { "items", QJsonArray{ QJsonObject{ { "id", "x" } }, QJsonObject{ { "id", "y" } } } } }));
        };
        m_ops.add(make);

        Tool echo;
        echo.name = "items.echo";
        echo.description = "Echoes a value.";
        echo.inputSchema = Schema::object({ { "value", QJsonObject{} } }, { "value" });
        echo.handler = [this](const CallContext&, const QJsonObject& arguments, const ToolReply& reply) {
            m_ran.append("items.echo");
            reply(ToolResult::ok(QJsonObject{ { "value", arguments["value"] } }));
        };
        m_ops.add(echo);

        Tool fail;
        fail.name = "items.fail";
        fail.description = "Always fails.";
        fail.inputSchema = Schema::object({});
        fail.handler = [this](const CallContext&, const QJsonObject&, const ToolReply& reply) {
            m_ran.append("items.fail");
            reply(ToolResult::error("nope"));
        };
        m_ops.add(fail);

        Tool write;
        write.name = "files.write";
        write.description = "Needs the files scope.";
        write.inputSchema = Schema::object({});
        write.scope = Scope::Files;
        write.annotations.readOnly = false;
        write.annotations.destructive = true;
        write.handler = [this](const CallContext&, const QJsonObject&, const ToolReply& reply) {
            m_ran.append("files.write");
            reply(ToolResult::ok(QJsonObject{}));
        };
        m_ops.add(write);

        registerOpTools(m_tools, &m_ops);
    }

    void init()
    {
        m_ran.clear();
        m_asked.clear();
    }

    void references_data()
    {
        QTest::addColumn<QJsonValue>("input");
        QTest::addColumn<QJsonValue>("expected");

        QTest::newRow("whole result") << QJsonValue("$0") << QJsonValue(QJsonObject{ { "list", QJsonArray{ 1, 2 } }, { "name", "n" } });
        QTest::newRow("field") << QJsonValue("$0.name") << QJsonValue("n");
        QTest::newRow("array index") << QJsonValue("$0.list.1") << QJsonValue(2);
        QTest::newRow("escaped") << QJsonValue("$$0.name") << QJsonValue("$0.name");
        QTest::newRow("plain string") << QJsonValue("hello $0") << QJsonValue("hello $0");
        QTest::newRow("nested") << QJsonValue(QJsonObject{ { "a", QJsonArray{ "$0.name" } } })
                                << QJsonValue(QJsonObject{ { "a", QJsonArray{ "n" } } });
    }

    void references()
    {
        QFETCH(QJsonValue, input);
        QFETCH(QJsonValue, expected);
        const QJsonArray results{ QJsonObject{ { "op", "x" }, { "ok", true }, { "result", QJsonObject{ { "list", QJsonArray{ 1, 2 } }, { "name", "n" } } } } };
        const auto resolved = resolveReferences(input, results, 1);
        QVERIFY2(resolved.has_value(), qPrintable(resolved ? QString() : resolved.error()));
        QCOMPARE(*resolved, expected);
    }

    void badReferences_data()
    {
        QTest::addColumn<QString>("reference");
        QTest::addColumn<QString>("contains");

        QTest::newRow("future step") << "$1" << "has not run yet";
        QTest::newRow("failed step") << "$0" << "failed";
        QTest::newRow("missing field") << "$2.nope" << "no field 'nope'";
        QTest::newRow("index out of range") << "$2.list.9" << "out of range";
    }

    void badReferences()
    {
        QFETCH(QString, reference);
        QFETCH(QString, contains);
        const QJsonArray results{ QJsonObject{ { "ok", false } }, QJsonObject{}, QJsonObject{ { "ok", true }, { "result", QJsonObject{ { "list", QJsonArray{ 1 } } } } } };
        const int current = reference == "$1" ? 1 : 3;
        const auto resolved = resolveReferences(QJsonValue(reference), results, current);
        QVERIFY(!resolved.has_value());
        QVERIFY2(resolved.error().contains(contains), qPrintable(resolved.error()));
    }

    void batchPassesResultsAlong()
    {
        const auto result = run("batch", QJsonObject{ { "ops", QJsonArray{ step("items.make"), step("items.echo", { { "value", "$0.items.1.id" } }) } } });
        QVERIFY(!result.isError);
        const auto results = result.structured.toObject()["results"].toArray();
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[1].toObject()["result"].toObject()["value"].toString(), QString("y"));
        QCOMPARE(result.structured.toObject()["completed"].toInt(), 2);
    }

    void batchStopsAtFirstFailure()
    {
        const auto result = run("batch", QJsonObject{ { "ops", QJsonArray{ step("items.make"), step("items.fail"), step("items.make") } } });
        QVERIFY(result.isError);
        QCOMPARE(m_ran, QStringList({ "items.make", "items.fail" }));
        QCOMPARE(result.structured.toObject()["stoppedAt"].toInt(), 1);
    }

    void batchContinuesOnErrorWhenAsked()
    {
        const auto result = run("batch", QJsonObject{ { "ops", QJsonArray{ step("items.fail"), step("items.make") } }, { "continueOnError", true } });
        QVERIFY(!result.isError);
        QCOMPARE(m_ran, QStringList({ "items.fail", "items.make" }));
        QCOMPARE(result.structured.toObject()["failures"].toInt(), 1);
    }

    void batchChecksEverythingFirst()
    {
        auto result = run("batch", QJsonObject{ { "ops", QJsonArray{ step("items.make"), step("items.mak") } } });
        QVERIFY(result.isError);
        QVERIFY(result.text.contains("items.make"));
        QVERIFY(m_ran.isEmpty());

        result = run("batch", QJsonObject{ { "ops", QJsonArray{ step("items.make"), step("files.write") } } });
        QVERIFY(result.isError);
        QVERIFY(result.text.contains("'files' scope"));
        QVERIFY(m_ran.isEmpty());
    }

    void batchValidatesResolvedArguments()
    {
        const auto result = run("batch", QJsonObject{ { "ops", QJsonArray{ step("items.make"), step("items.echo") } } });
        QVERIFY(result.isError);
        const auto second = result.structured.toObject()["results"].toArray()[1].toObject();
        QVERIFY(second["error"].toString().contains("missing required property 'value'"));
    }

    void callRunsOneOp()
    {
        auto result = run("call", QJsonObject{ { "op", "items.echo" }, { "arguments", QJsonObject{ { "value", 5 } } } });
        QVERIFY(!result.isError);
        QCOMPARE(result.structured.toObject()["value"].toInt(), 5);

        result = run("call", QJsonObject{ { "op", "items.eco" } });
        QVERIFY(result.isError);
        QVERIFY(result.text.contains("items.echo"));

        result = run("call", QJsonObject{ { "op", "files.write" } });
        QVERIFY(result.isError);
        result = run("call", QJsonObject{ { "op", "files.write" } }, Scope::Read | Scope::Files);
        QVERIFY(!result.isError);
    }

    void confirmationPolicy()
    {
        const auto write = QJsonObject{ { "op", "files.write" } };
        const auto scopes = Scope::Read | Scope::Files;

        // Destructive op, default policy, nobody to ask: refused, nothing ran
        auto result = run("call", write, scopes, ConfirmPolicy::AskDestructive);
        QVERIFY(result.isError);
        QVERIFY(m_ran.isEmpty());

        // The user says no
        result = run("call", write, scopes, ConfirmPolicy::AskDestructive, false);
        QVERIFY(result.isError);
        QVERIFY(result.text.contains("did not allow"));
        QVERIFY(m_ran.isEmpty());
        QCOMPARE(m_asked.size(), 1);

        // The user says yes; `call` itself never asks, so there is exactly one question
        m_asked.clear();
        result = run("call", write, scopes, ConfirmPolicy::AskDestructive, true);
        QVERIFY(!result.isError);
        QCOMPARE(m_ran, QStringList{ "files.write" });
        QCOMPARE(m_asked.size(), 1);

        // Reads never ask, even under ask_all_writes
        m_asked.clear();
        result = run("call", QJsonObject{ { "op", "items.make" } }, scopes, ConfirmPolicy::AskAllWrites, false);
        QVERIFY(!result.isError);
        QVERIFY(m_asked.isEmpty());

        // never_ask runs without a question
        result = run("call", write, scopes, ConfirmPolicy::NeverAsk);
        QVERIFY(!result.isError);
        QVERIFY(m_asked.isEmpty());
    }

    void needsConfirmationTable()
    {
        QVERIFY(!needsConfirmation(Effect::Read, ConfirmPolicy::AskAllWrites));
        QVERIFY(needsConfirmation(Effect::Write, ConfirmPolicy::AskAllWrites));
        QVERIFY(!needsConfirmation(Effect::Write, ConfirmPolicy::AskDestructive));
        QVERIFY(needsConfirmation(Effect::Destructive, ConfirmPolicy::AskDestructive));
        QVERIFY(!needsConfirmation(Effect::Destructive, ConfirmPolicy::NeverAsk));
    }

    void searchOps()
    {
        auto result = run("search_ops", QJsonObject{ { "query", "echoes" } });
        auto ops = result.structured.toObject()["ops"].toArray();
        QCOMPARE(ops.size(), 1);
        QCOMPARE(ops[0].toObject()["name"].toString(), QString("items.echo"));
        QCOMPARE(ops[0].toObject()["arguments"].toString(), QString("value: "));

        result = run("search_ops", QJsonObject{ { "area", "items" }, { "detail", "names" } });
        ops = result.structured.toObject()["ops"].toArray();
        QCOMPARE(ops.size(), 3);
        QVERIFY(!ops[0].toObject().contains("summary"));
        QCOMPARE(result.structured.toObject()["areas"].toArray().size(), 2);

        result = run("search_ops", QJsonObject{ { "query", "files.write" }, { "detail", "full" } });
        ops = result.structured.toObject()["ops"].toArray();
        QVERIFY(ops[0].toObject().contains("inputSchema"));
        QCOMPARE(ops[0].toObject()["scope"].toString(), QString("files"));
    }
};

QTEST_GUILESS_MAIN(McpOpsTest)

#include "McpOps_test.moc"
