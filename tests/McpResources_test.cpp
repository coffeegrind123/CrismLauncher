#include <QJsonArray>
#include <QTest>

#include <mcp/McpResources.h>

using namespace Mcp;

class McpResourcesTest : public QObject {
    Q_OBJECT

    static Result<ResourceContent> read(const ResourceCatalog& catalog, const QString& uri)
    {
        Result<ResourceContent> out = std::unexpected(QString("no reply"));
        catalog.read(uri, [&out](Result<ResourceContent> content) { out = std::move(content); });
        return out;
    }

   private slots:
    void fixedAndTemplated()
    {
        ResourceCatalog catalog;
        catalog.add({ "x://fixed", "fixed", "Fixed", "", "text/plain" },
                    [](const QString& uri, const QStringList&, const ResourceReply& reply) { reply(ResourceContent{ uri, "text/plain", "fixed" }); });
        catalog.add({ "x://things/{id}/part/{part}", "thing", "Thing", "", "text/plain" },
                    [](const QString& uri, const QStringList& captures, const ResourceReply& reply) {
                        reply(ResourceContent{ uri, "text/plain", captures.join("|") });
                    });

        QCOMPARE(catalog.listJson().size(), 1);
        QCOMPARE(catalog.templatesJson().size(), 1);
        QCOMPARE(catalog.templatesJson()[0].toObject()["uriTemplate"].toString(), QString("x://things/{id}/part/{part}"));

        auto content = read(catalog, "x://fixed");
        QVERIFY(content.has_value());
        QCOMPARE(content->text, QString("fixed"));

        content = read(catalog, "x://things/My%20Pack/part/a.b");
        QVERIFY(content.has_value());
        QCOMPARE(content->text, QString("My Pack|a.b"));

        QVERIFY(!read(catalog, "x://things/a/b/part/c").has_value());
        QVERIFY(!read(catalog, "x://nothing").has_value());
    }

    void prompts()
    {
        PromptCatalog catalog;
        catalog.add({ "fix", "Fix", "Fixes", { { "instance", "id", true }, { "note", "extra", false } },
                      [](const QJsonObject& a) { return "fix " + a.value("instance").toString(); } });

        QCOMPARE(catalog.listJson().size(), 1);
        QCOMPARE(catalog.listJson()[0].toObject()["arguments"].toArray().size(), 2);

        const auto prompt = catalog.get("fix", QJsonObject{ { "instance", "abc" } });
        QVERIFY(prompt.has_value());
        const auto message = (*prompt)["messages"].toArray()[0].toObject();
        QCOMPARE(message["role"].toString(), QString("user"));
        QCOMPARE(message["content"].toObject()["text"].toString(), QString("fix abc"));

        QVERIFY(!catalog.get("fix", {}).has_value());
        QVERIFY(!catalog.get("other", {}).has_value());
    }
};

QTEST_GUILESS_MAIN(McpResourcesTest)

#include "McpResources_test.moc"
