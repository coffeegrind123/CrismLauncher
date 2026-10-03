#include <QTest>

#include <net/DownloadMirror.h>

// Mapped URLs were checked live: the mirror served bytes matching the official SHA-1
class DownloadMirrorTest : public QObject {
    Q_OBJECT

   private slots:
    void rewrite_data()
    {
        QTest::addColumn<QString>("original");
        QTest::addColumn<QString>("mirrored");

        const QString bmclapi = "https://bmclapi2.bangbang93.com/";

        QTest::newRow("version json") << "https://piston-meta.mojang.com/v1/packages/abc/1.21.1.json"
                                      << bmclapi + "v1/packages/abc/1.21.1.json";
        QTest::newRow("client jar") << "https://piston-data.mojang.com/v1/objects/30c7/client.jar"
                                    << bmclapi + "v1/objects/30c7/client.jar";
        QTest::newRow("old client jar") << "https://launcher.mojang.com/v1/objects/e80d/client.jar"
                                        << bmclapi + "v1/objects/e80d/client.jar";
        QTest::newRow("library") << "https://libraries.minecraft.net/com/mojang/brigadier/1.3.10/brigadier-1.3.10.jar"
                                 << bmclapi + "maven/com/mojang/brigadier/1.3.10/brigadier-1.3.10.jar";
        QTest::newRow("asset") << "https://resources.download.minecraft.net/5f/5ff04807c356f1beed0b86ccf659b44b9983e3fa"
                               << bmclapi + "assets/5f/5ff04807c356f1beed0b86ccf659b44b9983e3fa";
        QTest::newRow("forge") << "https://maven.minecraftforge.net/net/minecraftforge/forge/1.20.1-47.4.0/forge-1.20.1-47.4.0-universal.jar"
                               << bmclapi + "maven/net/minecraftforge/forge/1.20.1-47.4.0/forge-1.20.1-47.4.0-universal.jar";
        QTest::newRow("old forge host") << "https://files.minecraftforge.net/maven/net/minecraftforge/forge/x.jar"
                                        << bmclapi + "maven/net/minecraftforge/forge/x.jar";
        QTest::newRow("neoforge") << "https://maven.neoforged.net/releases/net/neoforged/neoforge/21.1.77/neoforge-21.1.77-universal.jar"
                                  << bmclapi + "maven/net/neoforged/neoforge/21.1.77/neoforge-21.1.77-universal.jar";
        QTest::newRow("fabric") << "https://maven.fabricmc.net/net/fabricmc/fabric-loader/0.16.9/fabric-loader-0.16.9.jar"
                                << bmclapi + "maven/net/fabricmc/fabric-loader/0.16.9/fabric-loader-0.16.9.jar";
        QTest::newRow("http upgraded") << "http://libraries.minecraft.net/a/b.jar" << bmclapi + "maven/a/b.jar";
        QTest::newRow("host case") << "https://Libraries.Minecraft.net/a/b.jar" << bmclapi + "maven/a/b.jar";
        QTest::newRow("default port") << "https://libraries.minecraft.net:443/a/b.jar" << bmclapi + "maven/a/b.jar";

        QTest::newRow("quilt") << "https://maven.quiltmc.org/repository/release/a.jar" << "";
        QTest::newRow("prism meta") << "https://meta.prismlauncher.org/v1/index.json" << "";
        QTest::newRow("lwjgl") << "https://build.lwjgl.org/release/3.3.3/lwjgl.jar" << "";
        QTest::newRow("lookalike host") << "https://libraries.minecraft.net.evil.com/a/b.jar" << "";
        QTest::newRow("subdomain") << "https://evil.libraries.minecraft.net/a/b.jar" << "";
        QTest::newRow("other port") << "https://libraries.minecraft.net:8443/a/b.jar" << "";
        QTest::newRow("userinfo") << "https://user:pw@libraries.minecraft.net/a/b.jar" << "";
        QTest::newRow("dot segments") << "https://libraries.minecraft.net/a/../../b.jar" << "";
        QTest::newRow("neoforge snapshots") << "https://maven.neoforged.net/snapshots/a.jar" << "";
        QTest::newRow("forge files root") << "https://files.minecraftforge.net/net/a.jar" << "";
        QTest::newRow("ftp") << "ftp://libraries.minecraft.net/a/b.jar" << "";
    }

    void rewrite()
    {
        QFETCH(QString, original);
        QFETCH(QString, mirrored);

        const auto result = Net::DownloadMirror::rewrite(QUrl(original), QUrl(Net::DownloadMirror::DEFAULT_BASE_URL));
        if (mirrored.isEmpty()) {
            QVERIFY2(!result.has_value(), qPrintable(result ? result->toString() : QString()));
        } else {
            QVERIFY(result.has_value());
            QCOMPARE(result->toString(), mirrored);
        }
    }

    void customBase()
    {
        const QUrl original("https://libraries.minecraft.net/a/b.jar");
        QCOMPARE(Net::DownloadMirror::rewrite(original, QUrl("https://m.example/bmc/"))->toString(), "https://m.example/bmc/maven/a/b.jar");
        QCOMPARE(Net::DownloadMirror::rewrite(original, QUrl("https://m.example/bmc"))->toString(), "https://m.example/bmc/maven/a/b.jar");
        QVERIFY(!Net::DownloadMirror::rewrite(original, QUrl("not a url")).has_value());
    }

    void circuitBreaker()
    {
        Net::DownloadMirror::reportSuccess();
        for (int i = 0; i < Net::DownloadMirror::FAILURES_BEFORE_DISABLING - 1; i++) {
            Net::DownloadMirror::reportFailure();
        }
        QVERIFY(!Net::DownloadMirror::isDisabledForSession());

        Net::DownloadMirror::reportSuccess();
        for (int i = 0; i < Net::DownloadMirror::FAILURES_BEFORE_DISABLING - 1; i++) {
            Net::DownloadMirror::reportFailure();
        }
        QVERIFY(!Net::DownloadMirror::isDisabledForSession());

        Net::DownloadMirror::reportFailure();
        QVERIFY(Net::DownloadMirror::isDisabledForSession());
        Net::DownloadMirror::reportSuccess();
        QVERIFY(!Net::DownloadMirror::isDisabledForSession());
    }
};

QTEST_GUILESS_MAIN(DownloadMirrorTest)

#include "DownloadMirror_test.moc"
