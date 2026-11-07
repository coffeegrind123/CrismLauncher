#include <QJsonArray>
#include <QTest>

#include <mcp/McpSchema.h>
#include <mcp/McpTool.h>

using namespace Mcp;

class McpSchemaTest : public QObject {
    Q_OBJECT

    QJsonObject schema() const
    {
        return Schema::object(
            {
                { "id", Schema::string("instance id") },
                { "limit", Schema::integer("max", 1, 10) },
                { "kind", Schema::string("kind", { "mod", "shader" }) },
                { "tags", Schema::array(Schema::string({})) },
                { "nested", Schema::object({ { "flag", Schema::boolean({}) } }, { "flag" }) },
            },
            { "id" });
    }

   private slots:
    void accepts()
    {
        const QJsonObject value{ { "id", "abc" }, { "limit", 3 }, { "kind", "mod" }, { "tags", QJsonArray{ "a" } },
                                 { "nested", QJsonObject{ { "flag", true } } } };
        QVERIFY(Schema::validate(value, schema()).has_value());
    }

    void rejects_data()
    {
        QTest::addColumn<QJsonObject>("value");
        QTest::addColumn<QString>("expected");

        QTest::newRow("missing required") << QJsonObject{} << "(arguments): missing required property 'id'";
        QTest::newRow("wrong type") << QJsonObject{ { "id", 5 } } << "/id: expected string, got integer";
        QTest::newRow("fraction for integer") << QJsonObject{ { "id", "a" }, { "limit", 1.5 } } << "/limit: expected integer, got number";
        QTest::newRow("below minimum") << QJsonObject{ { "id", "a" }, { "limit", 0 } } << "/limit: below the minimum 1";
        QTest::newRow("not in enum") << QJsonObject{ { "id", "a" }, { "kind", "map" } } << "/kind: 'map' is not one of: mod, shader";
        QTest::newRow("unknown property") << QJsonObject{ { "id", "a" }, { "idd", 1 } }
                                          << "(arguments): unknown property 'idd' (known: id, kind, limit, nested, tags)";
        QTest::newRow("bad array item") << QJsonObject{ { "id", "a" }, { "tags", QJsonArray{ "a", 2 } } } << "/tags/1: expected string, got integer";
        QTest::newRow("nested required") << QJsonObject{ { "id", "a" }, { "nested", QJsonObject{} } }
                                         << "/nested: missing required property 'flag'";
    }

    void rejects()
    {
        QFETCH(QJsonObject, value);
        QFETCH(QString, expected);

        const auto result = Schema::validate(value, schema());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), expected);
    }

    void typeLists()
    {
        QJsonObject nullable{ { "type", QJsonArray{ "string", "null" } } };
        QVERIFY(Schema::validate(QJsonValue("x"), nullable).has_value());
        QVERIFY(Schema::validate(QJsonValue(QJsonValue::Null), nullable).has_value());
        QVERIFY(!Schema::validate(QJsonValue(1), nullable).has_value());
    }

    void suggestions()
    {
        QCOMPARE(closestMatches("instance", { "instances", "launcher_info", "logs" }).value(0), QString("instances"));
        QCOMPARE(closestMatches("lancher_info", { "instances", "launcher_info" }), QStringList{ "launcher_info" });
        QVERIFY(closestMatches("zzzzzz", { "instances", "launcher_info" }).isEmpty());
    }
};

QTEST_GUILESS_MAIN(McpSchemaTest)

#include "McpSchema_test.moc"
