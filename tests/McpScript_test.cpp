#include <QJsonArray>
#include <QTest>

#include <mcp/McpOps.h>
#include <mcp/McpReplay.h>
#include <mcp/McpSchema.h>
#include <mcp/McpScript.h>
#include <mcp/McpTool.h>

using namespace Mcp;

class McpScriptTest : public QObject {
    Q_OBJECT

    ToolRegistry m_ops;
    ToolRegistry m_tools;
    QStringList m_ran;
    QStringList m_askedIn;

    // Runs execute and spins the event loop until it replies: ops are dispatched from the event loop
    ToolResult execute(const QString& code,
                       QJsonObject arguments = {},
                       Scopes scopes = Scope::Read,
                       ConfirmPolicy policy = ConfirmPolicy::NeverAsk,
                       bool allow = true)
    {
        arguments["code"] = code;
        CallContext context;
        context.token.id = "t";
        context.token.scopes = scopes;
        context.token.policy = policy;
        context.progress = [](double, std::optional<double>, const QString&) {};
        // In-chat confirmation must never be used by a script: it would re-run the whole call
        context.confirm = [this](const QStringList&, const ConfirmAnswer& answer) {
            m_askedIn.append("chat");
            answer(ConfirmVerdict::needsInput(QJsonObject{ { "requestState", "x" } }));
        };
        context.confirmInLauncher = [this, allow](const QStringList&, const ConfirmAnswer& answer) {
            m_askedIn.append("launcher");
            answer(allow ? ConfirmVerdict::allowed() : ConfirmVerdict::denied("declined"));
        };
        auto out = std::make_shared<ToolResult>();
        auto replied = std::make_shared<bool>(false);
        invokeTool(*m_tools.find("execute"), context, arguments, [out, replied](const ToolResult& result) {
            *out = result;
            *replied = true;
        });
        [&replied] { QTRY_VERIFY_WITH_TIMEOUT(*replied, 20000); }();
        return *out;
    }

    static QJsonObject structured(const ToolResult& result) { return result.structured.toObject(); }

   private slots:
    void initTestCase()
    {
        Tool make;
        make.name = "items.make";
        make.description = "Makes two items. Used by tests.";
        make.inputSchema = Schema::object({});
        make.outputSchema = Schema::object({ { "items", Schema::array(Schema::object({ { "id", Schema::string("Item id") } })) } });
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
            reply(ToolResult::error(ErrorCode::NotFound, "No item 'z'.", "items.make makes some."));
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

        Tool direct;
        direct.name = "double_it";
        direct.description = "A direct tool: doubles a number.";
        direct.inputSchema = Schema::object({ { "n", Schema::integer("A number") } }, { "n" });
        direct.handler = [](const CallContext&, const QJsonObject& arguments, const ToolReply& reply) {
            reply(ToolResult::ok(QJsonObject{ { "n", arguments["n"].toInt() * 2 } }));
        };
        m_tools.add(direct);

        registerOpTools(m_tools, &m_ops);
        registerScriptTool(m_tools, &m_ops);
    }

    void init()
    {
        m_ran.clear();
        m_askedIn.clear();
        replayCache().clear();
    }

    void returnsValueAndLogs()
    {
        const auto result = execute("console.log('hi', {a: 1}); console.warn(2); return 1 + 2;");
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(structured(result)["value"].toInt(), 3);
        QCOMPARE(structured(result)["logs"].toArray(), QJsonArray({ "hi {\"a\":1}", "[warn] 2" }));
        QCOMPARE(structured(result)["opCount"].toInt(), 0);
    }

    void returnsNullWithoutReturn()
    {
        const auto result = execute("const x = 1;");
        QVERIFY2(!result.isError, qPrintable(result.text));
        QVERIFY(structured(result)["value"].isNull());
    }

    void callsOpsAndTools()
    {
        const auto result = execute(
            "const made = await launcher.items.make();\n"
            "const echoed = await launcher.items.echo({value: made.items[1].id});\n"
            "const doubled = await launcher.double_it({n: 21});\n"
            "const viaCall = await launcher.call('items.echo', {value: 7});\n"
            "return [echoed.value, doubled.n, viaCall.value];");
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(structured(result)["value"].toArray(), QJsonArray({ "y", 42, 7 }));
        QCOMPARE(m_ran, QStringList({ "items.make", "items.echo", "items.echo" }));
        const auto ops = structured(result)["ops"].toArray();
        QCOMPARE(ops.size(), 4);
        QCOMPARE(ops[0].toObject()["op"].toString(), QString("items.make"));
        QVERIFY(ops[0].toObject()["ok"].toBool());
    }

    void runsOpsConcurrently()
    {
        const auto result = execute(
            "const [a, b] = await Promise.all([launcher.items.echo({value: 1}), launcher.items.echo({value: 2})]);"
            "await launcher.sleep(5);"
            "return a.value + b.value;");
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(structured(result)["value"].toInt(), 3);
    }

    void metaToolsAreNotCallable()
    {
        const auto result = execute("return typeof launcher.batch + ' ' + typeof launcher.execute + ' ' + typeof launcher.search_ops;");
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(structured(result)["value"].toString(), QString("undefined undefined undefined"));
    }

    void scriptCatchesLauncherError()
    {
        const auto result = execute(
            "try { await launcher.items.fail(); } catch (e) { return [e instanceof LauncherError, e.name, e.code, e.op, e.message, e.hint]; }");
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(structured(result)["value"].toArray(),
                 QJsonArray({ true, "LauncherError", "not_found", "items.fail", "No item 'z'.", "items.make makes some." }));
    }

    void uncaughtLauncherErrorKeepsItsCode()
    {
        const auto result = execute("await launcher.items.make();\nawait launcher.items.fail();\nreturn 1;");
        QVERIFY(result.isError);
        QCOMPARE(result.code, ErrorCode::NotFound);
        QCOMPARE(structured(result)["code"].toString(), QString("not_found"));
        QCOMPARE(structured(result)["op"].toString(), QString("items.fail"));
        QCOMPARE(structured(result)["opCount"].toInt(), 2);
        QVERIFY(result.text.contains("not undone"));
    }

    void unknownOpSuggests()
    {
        const auto result = execute("return await launcher.call('items.mak');");
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("unknown_op"));
        QVERIFY2(structured(result)["hint"].toString().contains("items.make"), qPrintable(result.text));
    }

    void argumentsAreValidated()
    {
        const auto result = execute("return await launcher.items.echo({});");
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("invalid_argument"));
        QVERIFY(m_ran.isEmpty());
    }

    void syntaxErrorIsScriptError()
    {
        const auto result = execute("return (;");
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("script_error"));
        QVERIFY2(structured(result)["error"].toString().contains("SyntaxError"), qPrintable(result.text));
    }

    void thrownErrorHasStack()
    {
        const auto result = execute("const x = null;\nreturn x.y;");
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("script_error"));
        QVERIFY2(structured(result)["stack"].toString().contains("execute.js:2"), qPrintable(structured(result)["stack"].toString()));
    }

    void readOnlyRefusesWrites()
    {
        const auto scopes = Scope::Read | Scope::Files;
        auto result = execute("await launcher.items.make(); return await launcher.files.write();", { { "read_only", true } }, scopes);
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("denied"));
        QCOMPARE(m_ran, QStringList{ "items.make" });

        init();
        result = execute("return await launcher.files.write();", {}, scopes);
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(m_ran, QStringList{ "files.write" });
    }

    void scopesStillApply()
    {
        const auto result = execute("return await launcher.files.write();");
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("scope_missing"));
        QVERIFY(m_ran.isEmpty());
    }

    void confirmsInTheLauncherWindow()
    {
        const auto scopes = Scope::Read | Scope::Files;
        auto result = execute("return await launcher.files.write();", {}, scopes, ConfirmPolicy::AskDestructive, true);
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(m_askedIn, QStringList{ "launcher" });
        QCOMPARE(m_ran, QStringList{ "files.write" });

        init();
        result = execute("return await launcher.files.write();", {}, scopes, ConfirmPolicy::AskDestructive, false);
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("denied"));
        QCOMPARE(m_askedIn, QStringList{ "launcher" });
        QVERIFY(m_ran.isEmpty());
    }

    void busyLoopTimesOut()
    {
        const auto result = execute("while (true) {}", { { "timeoutSeconds", 1 } });
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("timeout"));
    }

    void waitingOnNothingFails()
    {
        const auto result = execute("await new Promise(() => {}); return 1;");
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("script_error"));
        QVERIFY(structured(result)["error"].toString().contains("nothing will settle"));
    }

    void memoryIsLimited()
    {
        const auto result = execute("const keep = []; for (;;) { keep.push('x'.repeat(1 << 20) + keep.length); }", { { "timeoutSeconds", 30 } });
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("out_of_memory"));
    }

    void resultSizeIsLimited()
    {
        const auto result = execute("return 'x'.repeat(2 * 1024 * 1024);");
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("script_error"));
        QVERIFY(structured(result)["error"].toString().contains("KiB"));
    }

    void operationIdReplays()
    {
        const QJsonObject arguments{ { "operation_id", "op-1" } };
        auto result = execute("return (await launcher.items.make()).items.length;", arguments);
        QVERIFY2(!result.isError, qPrintable(result.text));
        QCOMPARE(m_ran.size(), 1);

        result = execute("return (await launcher.items.make()).items.length;", arguments);
        QVERIFY2(!result.isError, qPrintable(result.text));
        QVERIFY(structured(result)["replayed"].toBool());
        QCOMPARE(structured(result)["value"].toInt(), 2);
        QCOMPARE(m_ran.size(), 1);

        result = execute("return 0;", arguments);
        QVERIFY(result.isError);
        QCOMPARE(structured(result)["code"].toString(), QString("conflict"));
    }

    void typeDeclarations()
    {
        const auto text = scriptTypeDeclarations(m_tools, m_ops);
        QVERIFY(text.contains("declare namespace launcher {"));
        QVERIFY(text.contains("namespace items {"));
        QVERIFY2(text.contains("function make(args?: Record<string, unknown>): Promise<{"), qPrintable(text));
        QVERIFY(text.contains("function echo(args: {"));
        QVERIFY(text.contains("function double_it(args: {"));
        QVERIFY(!text.contains("function batch("));
        QVERIFY(text.contains("declare class LauncherError"));
    }
};

QTEST_GUILESS_MAIN(McpScriptTest)

#include "McpScript_test.moc"
