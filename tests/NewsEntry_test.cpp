#include <QDomDocument>
#include <QTest>

#include <news/NewsEntry.h>

// Entries as served by prismlauncher.org/feed/feed.xml and GitHub's releases.atom
class NewsEntryTest : public QObject {
    Q_OBJECT

   private slots:
    void link_data()
    {
        QTest::addColumn<QString>("xml");
        QTest::addColumn<QString>("link");

        QTest::newRow("prism") << "<entry><title>Release 11.1.1</title><id>https://prismlauncher.org/news/release-11.1.1</id>"
                                  "<link href=\"https://prismlauncher.org/news/release-11.1.1\"/><content>x</content></entry>"
                               << "https://prismlauncher.org/news/release-11.1.1";
        QTest::newRow("github") << "<entry><id>tag:github.com,2008:Repository/1037986415/12.0.28</id>"
                                   "<link rel=\"alternate\" type=\"text/html\" href=\"https://github.com/o/r/releases/tag/12.0.28\"/>"
                                   "<title>Crism Launcher 12.0.28</title><content type=\"html\">x</content></entry>"
                                << "https://github.com/o/r/releases/tag/12.0.28";
        QTest::newRow("only self link") << "<entry><id>https://example.com/a</id><link rel=\"self\" href=\"https://example.com/feed\"/></entry>"
                                        << "https://example.com/a";
        QTest::newRow("no link") << "<entry><id>https://example.com/a</id></entry>" << "https://example.com/a";
    }

    void link()
    {
        QFETCH(QString, xml);
        QFETCH(QString, link);

        QDomDocument doc;
        QVERIFY(doc.setContent(xml));
        NewsEntry entry;
        QVERIFY(NewsEntry::fromXmlElement(doc.documentElement(), &entry));
        QCOMPARE(entry.link, link);
    }
};

QTEST_GUILESS_MAIN(NewsEntryTest)

#include "NewsEntry_test.moc"
