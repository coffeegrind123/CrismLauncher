#include <QTest>

#include <mcp/McpHttp.h>

using Mcp::Http::RequestParser;
using State = Mcp::Http::RequestParser::State;

class McpHttpTest : public QObject {
    Q_OBJECT

   private slots:
    void parsesContentLengthBody()
    {
        RequestParser parser;
        QCOMPARE(parser.feed("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:1\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"),
                 State::Complete);
        const auto request = parser.take();
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.path(), QByteArray("/mcp"));
        QCOMPARE(request.header("content-type"), QByteArray("application/json"));
        QCOMPARE(request.body, QByteArray("{}"));
        QVERIFY(request.keepAlive());
        QCOMPARE(parser.state(), State::NeedMore);
    }

    void headerNamesAreCaseInsensitiveAndQueryIsStripped()
    {
        RequestParser parser;
        QCOMPARE(parser.feed("GET /mcp?x=1 HTTP/1.1\r\nMCP-Protocol-Version:  2026-07-28  \r\n\r\n"), State::Complete);
        const auto request = parser.take();
        QCOMPARE(request.header("mcp-protocol-version"), QByteArray("2026-07-28"));
        QCOMPARE(request.path(), QByteArray("/mcp"));
        QCOMPARE(request.target, QByteArray("/mcp?x=1"));
    }

    void byteByByteFeeding()
    {
        const QByteArray raw = "POST /mcp HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
        RequestParser parser;
        for (qsizetype i = 0; i < raw.size() - 1; ++i) {
            QCOMPARE(parser.feed(raw.mid(i, 1)), State::NeedMore);
        }
        QCOMPARE(parser.feed(raw.right(1)), State::Complete);
        QCOMPARE(parser.take().body, QByteArray("hello"));
    }

    void pipelinedRequestsStayBuffered()
    {
        RequestParser parser;
        QCOMPARE(parser.feed("POST /a HTTP/1.1\r\nContent-Length: 1\r\n\r\nAPOST /b HTTP/1.1\r\nContent-Length: 1\r\n\r\nB"), State::Complete);
        const auto first = parser.take();
        QCOMPARE(first.target, QByteArray("/a"));
        QCOMPARE(first.body, QByteArray("A"));
        QCOMPARE(parser.state(), State::Complete);
        const auto second = parser.take();
        QCOMPARE(second.target, QByteArray("/b"));
        QCOMPARE(second.body, QByteArray("B"));
        QCOMPARE(parser.state(), State::NeedMore);
    }

    void chunkedBody()
    {
        RequestParser parser;
        QCOMPARE(parser.feed("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Trailer: y\r\n\r\n"),
                 State::Complete);
        QCOMPARE(parser.take().body, QByteArray("Wikipedia"));
    }

    void connectionCloseAndHttp10()
    {
        RequestParser parser;
        QCOMPARE(parser.feed("GET / HTTP/1.1\r\nConnection: close\r\n\r\n"), State::Complete);
        QVERIFY(!parser.take().keepAlive());
        QCOMPARE(parser.feed("GET / HTTP/1.0\r\n\r\n"), State::Complete);
        QVERIFY(!parser.take().keepAlive());
        QCOMPARE(parser.feed("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"), State::Complete);
        QVERIFY(parser.take().keepAlive());
    }

    void rejects_data()
    {
        QTest::addColumn<QByteArray>("raw");
        QTest::addColumn<int>("status");

        QTest::newRow("both lengths") << QByteArray("POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n") << 400;
        QTest::newRow("negative length") << QByteArray("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n") << 400;
        QTest::newRow("non-numeric length") << QByteArray("POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n") << 400;
        QTest::newRow("conflicting lengths") << QByteArray("POST / HTTP/1.1\r\nContent-Length: 1, 2\r\n\r\n") << 400;
        QTest::newRow("body too large") << QByteArray("POST / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n") << 413;
        QTest::newRow("bad request line") << QByteArray("POST /\r\n\r\n") << 400;
        QTest::newRow("http/2") << QByteArray("GET / HTTP/1.2\r\n\r\n") << 505;
        QTest::newRow("folded header") << QByteArray("GET / HTTP/1.1\r\nA: b\r\n c\r\n\r\n") << 400;
        QTest::newRow("bare LF") << QByteArray("GET / HTTP/1.1\nA: b\r\n\r\n") << 400;
        QTest::newRow("space in name") << QByteArray("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n") << 400;
        QTest::newRow("gzip encoding") << QByteArray("POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n") << 501;
        QTest::newRow("bad chunk size") << QByteArray("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n") << 400;
    }

    void rejects()
    {
        QFETCH(QByteArray, raw);
        QFETCH(int, status);

        RequestParser parser;
        QCOMPARE(parser.feed(raw), State::Error);
        QCOMPARE(parser.errorStatus(), status);
    }

    void headerTooLarge()
    {
        RequestParser parser;
        QCOMPARE(parser.feed("GET / HTTP/1.1\r\nX: " + QByteArray(Mcp::Http::MAX_HEADER_BYTES, 'a')), State::Error);
        QCOMPARE(parser.errorStatus(), 431);
    }
};

QTEST_GUILESS_MAIN(McpHttpTest)

#include "McpHttp_test.moc"
