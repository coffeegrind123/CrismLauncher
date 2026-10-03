#include <QTest>

#include <modplatform/flame/FlameAPI.h>

// Expected URLs were checked against the live CDN: each returns the jar whose md5 ATLauncher lists
class FlameCdnUrlTest : public QObject {
    Q_OBJECT

   private slots:
    void buildCdnUrl_data()
    {
        QTest::addColumn<int>("fileId");
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<QString>("url");

        QTest::newRow("plain") << 2269324 << "journeymap-1.8.8-5.1.3-unlimited.jar"
                               << "https://edge.forgecdn.net/files/2269/324/journeymap-1.8.8-5.1.3-unlimited.jar";
        QTest::newRow("no zero padding") << 2277024 << "VTweaks-1.8.x-1.4.6.jar"
                                         << "https://edge.forgecdn.net/files/2277/24/VTweaks-1.8.x-1.4.6.jar";
        QTest::newRow("space in name") << 2279851 << "Fluidity 4.0.0.4.jar"
                                       << "https://edge.forgecdn.net/files/2279/851/Fluidity%204.0.0.4.jar";
        QTest::newRow("plus in name") << 1234567 << "mod+extra.jar"
                                      << "https://edge.forgecdn.net/files/1234/567/mod%2Bextra.jar";
    }

    void buildCdnUrl()
    {
        QFETCH(int, fileId);
        QFETCH(QString, fileName);
        QFETCH(QString, url);

        QCOMPARE(FlameAPI::getCdnDownloadUrl(fileId, fileName), url);
    }

    void rejectUnaddressable_data()
    {
        QTest::addColumn<int>("fileId");
        QTest::addColumn<QString>("fileName");

        QTest::newRow("no id") << 0 << "mod.jar";
        QTest::newRow("negative id") << -5 << "mod.jar";
        QTest::newRow("no name") << 2269324 << "";
    }

    void rejectUnaddressable()
    {
        QFETCH(int, fileId);
        QFETCH(QString, fileName);

        QVERIFY(FlameAPI::getCdnDownloadUrl(fileId, fileName).isEmpty());
    }
};

QTEST_GUILESS_MAIN(FlameCdnUrlTest)

#include "FlameCdnUrl_test.moc"
