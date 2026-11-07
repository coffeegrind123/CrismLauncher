#include <QJsonObject>
#include <QTest>

#include <mcp/McpStdioRelay.h>

class McpStdioRelayTest : public QObject {
    Q_OBJECT

    static QByteArray header(const QList<std::pair<QByteArray, QByteArray>>& headers, const QByteArray& name)
    {
        for (const auto& [key, value] : headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

   private slots:
    void modernRequest()
    {
        const QJsonObject message{ { "jsonrpc", "2.0" },
                                   { "id", 1 },
                                   { "method", "tools/call" },
                                   { "params", QJsonObject{ { "name", "instances" },
                                                            { "_meta", QJsonObject{ { "io.modelcontextprotocol/protocolVersion", "2026-07-28" } } } } } };
        const auto headers = McpStdioRelay::headersFor(message, "2025-11-25");
        QCOMPARE(header(headers, "MCP-Protocol-Version"), QByteArray("2026-07-28"));
        QCOMPARE(header(headers, "Mcp-Method"), QByteArray("tools/call"));
        QCOMPARE(header(headers, "Mcp-Name"), QByteArray("instances"));
    }

    void legacyRequestUsesNegotiatedVersion()
    {
        const QJsonObject message{ { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/list" } };
        QCOMPARE(header(McpStdioRelay::headersFor(message, "2025-06-18"), "MCP-Protocol-Version"), QByteArray("2025-06-18"));

        const QJsonObject initialize{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "initialize" } };
        QVERIFY(header(McpStdioRelay::headersFor(initialize, "2025-06-18"), "MCP-Protocol-Version").isEmpty());
    }

    void nonAsciiNamesAreEncoded()
    {
        const QJsonObject message{ { "jsonrpc", "2.0" },
                                   { "id", 3 },
                                   { "method", "resources/read" },
                                   { "params", QJsonObject{ { "uri", QString::fromUtf8("launcher://instances/Caf\xC3\xA9/summary") } } } };
        const auto name = header(McpStdioRelay::headersFor(message, {}), "Mcp-Name");
        QVERIFY(name.startsWith("=?base64?"));
        QVERIFY(name.endsWith("?="));
        QCOMPARE(QString::fromUtf8(QByteArray::fromBase64(name.mid(9, name.size() - 11))),
                 QString::fromUtf8("launcher://instances/Caf\xC3\xA9/summary"));
    }
};

QTEST_GUILESS_MAIN(McpStdioRelayTest)

#include "McpStdioRelay_test.moc"
