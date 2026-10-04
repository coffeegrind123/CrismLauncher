#include <QRegularExpression>
#include <QTest>

#include <net/DownloadMirror.h>

#include "BuildConfig.h"

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

    void mcim_data()
    {
        QTest::addColumn<QString>("original");
        QTest::addColumn<int>("verb");
        QTest::addColumn<bool>("pinned");
        QTest::addColumn<QString>("mirrored");

        using Net::DownloadMirror::Verb;
        const int get = static_cast<int>(Verb::Get);
        const int post = static_cast<int>(Verb::Post);
        const int other = static_cast<int>(Verb::Other);
        const QString mcim = "https://mod.mcimirror.top/";

        QTest::newRow("modrinth project") << "https://api.modrinth.com/v2/project/P7dR8mSH" << get << false
                                          << mcim + "modrinth/v2/project/P7dR8mSH";
        QTest::newRow("modrinth search query") << "https://api.modrinth.com/v2/search?query=sodium&limit=1" << get << false
                                               << mcim + "modrinth/v2/search?query=sodium&limit=1";
        QTest::newRow("modrinth projects batch") << "https://api.modrinth.com/v2/projects?ids=[%22P7dR8mSH%22,%22AANobbMI%22]" << get
                                                 << false << mcim + "modrinth/v2/projects?ids=[%22P7dR8mSH%22,%22AANobbMI%22]";
        QTest::newRow("modrinth version_files post") << "https://api.modrinth.com/v2/version_files" << post << false
                                                     << mcim + "modrinth/v2/version_files";
        QTest::newRow("curseforge mod") << "https://api.curseforge.com/v1/mods/238222" << get << false
                                        << mcim + "curseforge/v1/mods/238222";
        QTest::newRow("curseforge fingerprints post") << "https://api.curseforge.com/v1/fingerprints" << post << false
                                                      << mcim + "curseforge/v1/fingerprints";
        QTest::newRow("modrinth file") << "https://cdn.modrinth.com/data/P7dR8mSH/versions/WCnCDG9V/fabric-api-0.161.2%2B26.4.jar"
                                       << get << true << mcim + "data/P7dR8mSH/versions/WCnCDG9V/fabric-api-0.161.2%2B26.4.jar";
        QTest::newRow("forgecdn edge file") << "https://edge.forgecdn.net/files/4712/866/jei-1.19.2-forge-11.6.0.1018.jar" << get << true
                                            << mcim + "files/4712/866/jei-1.19.2-forge-11.6.0.1018.jar";
        QTest::newRow("forgecdn mediafilez file") << "https://mediafilez.forgecdn.net/files/4712/866/a%20b.jar" << get << true
                                                  << mcim + "files/4712/866/a%20b.jar";

        QTest::newRow("unpinned modrinth file") << "https://cdn.modrinth.com/data/P7dR8mSH/versions/x/a.jar" << get << false << "";
        QTest::newRow("unpinned forgecdn file") << "https://edge.forgecdn.net/files/1/2/a.jar" << get << false << "";
        QTest::newRow("file post") << "https://cdn.modrinth.com/data/a/versions/x/a.jar" << post << true << "";
        QTest::newRow("api delete") << "https://api.modrinth.com/v2/project/x" << other << false << "";
        QTest::newRow("modrinth staging") << "https://staging-api.modrinth.com/v2/project/x" << get << false << "";
        QTest::newRow("modrinth other version") << "https://api.modrinth.com/v3/project/x" << get << false << "";
        QTest::newRow("modrinth cdn non-data") << "https://cdn.modrinth.com/modpacks/a.mrpack" << get << true << "";
        QTest::newRow("forgecdn avatars") << "https://media.forgecdn.net/avatars/1/2/a.png" << get << true << "";
        QTest::newRow("lookalike host") << "https://api.modrinth.com.evil.com/v2/project/x" << get << false << "";
        QTest::newRow("userinfo") << "https://u:p@api.modrinth.com/v2/project/x" << get << false << "";
        QTest::newRow("other port") << "https://api.curseforge.com:8443/v1/mods/1" << get << false << "";
        QTest::newRow("dot segments") << "https://api.modrinth.com/v2/../../etc" << get << false << "";
        QTest::newRow("encoded dot segments") << "https://cdn.modrinth.com/data/%2e%2e/%2e%2e/a.jar" << get << true << "";
        QTest::newRow("bmclapi host") << "https://libraries.minecraft.net/a/b.jar" << get << true << "";
    }

    void mcim()
    {
        QFETCH(QString, original);
        QFETCH(int, verb);
        QFETCH(bool, pinned);
        QFETCH(QString, mirrored);

        Net::DownloadMirror::Config config;
        config.modrinth = Net::DownloadMirror::ModPlatformMirror::Mcim;
        config.curseForge = Net::DownloadMirror::ModPlatformMirror::Mcim;

        const auto result = Net::DownloadMirror::rewriteMcim(QUrl(original, QUrl::TolerantMode), config,
                                                             static_cast<Net::DownloadMirror::Verb>(verb), pinned);
        if (mirrored.isEmpty()) {
            QVERIFY2(!result.has_value(), qPrintable(result ? result->toString() : QString()));
        } else {
            QVERIFY(result.has_value());
            QCOMPARE(result->toString(QUrl::FullyEncoded), QUrl(mirrored, QUrl::TolerantMode).toString(QUrl::FullyEncoded));
        }
    }

    void mcimPerPlatform()
    {
        using namespace Net::DownloadMirror;
        const QUrl modrinth("https://api.modrinth.com/v2/project/x");
        const QUrl curseForge("https://api.curseforge.com/v1/mods/1");

        Config config;
        QVERIFY(!rewriteMcim(modrinth, config, Verb::Get, false));
        QVERIFY(!rewriteMcim(curseForge, config, Verb::Get, false));

        config.modrinth = ModPlatformMirror::Mcim;
        QVERIFY(rewriteMcim(modrinth, config, Verb::Get, false));
        QVERIFY(!rewriteMcim(curseForge, config, Verb::Get, false));

        config.modrinth = ModPlatformMirror::Official;
        config.curseForge = ModPlatformMirror::Mcim;
        QVERIFY(!rewriteMcim(modrinth, config, Verb::Get, false));
        QVERIFY(rewriteMcim(curseForge, config, Verb::Get, false));
    }

    // The API key and token are added by official host only (ApiHeaderProxy); MCIM must not be one of them
    void mcimGetsNoCredentials()
    {
        const auto mcimHost = QUrl(Net::DownloadMirror::MCIM_BASE_URL).host();
        QVERIFY(mcimHost != QUrl(BuildConfig.FLAME_BASE_URL).host());
        QVERIFY(mcimHost != BuildConfig.FLAME_DOWNLOAD_HOST);
        QVERIFY(mcimHost != QUrl(BuildConfig.MODRINTH_PROD_URL).host());
        QVERIFY(mcimHost != QUrl(BuildConfig.MODRINTH_STAGING_URL).host());
        QVERIFY(mcimHost != BuildConfig.MODRINTH_DOWNLOAD_HOST);
    }

    // MCIM's launcher list (mcmod-info-mirror/mcim-rust-api#12) expects "<Name>/<version>" and nothing else
    void userAgentHasMcimFormat()
    {
        static const QRegularExpression s_mcimUserAgent(QRegularExpression::anchoredPattern(R"([A-Za-z][A-Za-z0-9.-]*/\d+\.\d+\.\d+)"));
        QVERIFY2(s_mcimUserAgent.match(BuildConfig.USER_AGENT).hasMatch(), qPrintable(BuildConfig.USER_AGENT));
    }

    void choose()
    {
        using namespace Net::DownloadMirror;
        reportSuccess(Provider::Bmclapi);
        reportSuccess(Provider::Mcim);

        const QUrl library("https://libraries.minecraft.net/a/b.jar");
        const QUrl modrinthApi("https://api.modrinth.com/v2/project/x");

        Config config;
        QVERIFY(!Net::DownloadMirror::choose(library, config, Verb::Get, true));
        QVERIFY(!Net::DownloadMirror::choose(modrinthApi, config, Verb::Get, false));

        config.mode = Mode::PreferMirror;
        config.base = QUrl(DEFAULT_BASE_URL);
        auto target = Net::DownloadMirror::choose(library, config, Verb::Get, true);
        QVERIFY(target);
        QVERIFY(target->provider == Provider::Bmclapi);
        QVERIFY(target->mayFallBack);
        QVERIFY(!Net::DownloadMirror::choose(library, config, Verb::Get, false));
        QVERIFY(!Net::DownloadMirror::choose(library, config, Verb::Post, true));

        config.mode = Mode::MirrorOnly;
        target = Net::DownloadMirror::choose(library, config, Verb::Get, true);
        QVERIFY(target);
        QVERIFY(!target->mayFallBack);

        config.modrinth = ModPlatformMirror::Mcim;
        target = Net::DownloadMirror::choose(modrinthApi, config, Verb::Get, false);
        QVERIFY(target);
        QVERIFY(target->provider == Provider::Mcim);
        QVERIFY(target->mayFallBack);
    }

    void chooseSkipsDisabledMirror()
    {
        using namespace Net::DownloadMirror;
        Config config;
        config.mode = Mode::PreferMirror;
        config.base = QUrl(DEFAULT_BASE_URL);
        config.modrinth = ModPlatformMirror::Mcim;
        const QUrl library("https://libraries.minecraft.net/a/b.jar");
        const QUrl modrinthApi("https://api.modrinth.com/v2/project/x");

        reportSuccess(Provider::Bmclapi);
        reportSuccess(Provider::Mcim);
        for (int i = 0; i < FAILURES_BEFORE_DISABLING; i++) {
            reportFailure(Provider::Mcim);
        }
        QVERIFY(!Net::DownloadMirror::choose(modrinthApi, config, Verb::Get, false));
        QVERIFY(Net::DownloadMirror::choose(library, config, Verb::Get, true));

        // MirrorOnly ignores the breaker: there is nothing to fall back to
        for (int i = 0; i < FAILURES_BEFORE_DISABLING; i++) {
            reportFailure(Provider::Bmclapi);
        }
        QVERIFY(!Net::DownloadMirror::choose(library, config, Verb::Get, true));
        config.mode = Mode::MirrorOnly;
        QVERIFY(Net::DownloadMirror::choose(library, config, Verb::Get, true));

        reportSuccess(Provider::Bmclapi);
        reportSuccess(Provider::Mcim);
    }

    void circuitBreaker_data()
    {
        QTest::addColumn<int>("provider");
        QTest::newRow("bmclapi") << static_cast<int>(Net::DownloadMirror::Provider::Bmclapi);
        QTest::newRow("mcim") << static_cast<int>(Net::DownloadMirror::Provider::Mcim);
    }

    void circuitBreaker()
    {
        using namespace Net::DownloadMirror;
        QFETCH(int, provider);
        const auto p = static_cast<Provider>(provider);
        const auto other = p == Provider::Bmclapi ? Provider::Mcim : Provider::Bmclapi;

        reportSuccess(p);
        reportSuccess(other);
        for (int i = 0; i < FAILURES_BEFORE_DISABLING - 1; i++) {
            reportFailure(p);
        }
        QVERIFY(!isDisabledForSession(p));

        reportSuccess(p);
        for (int i = 0; i < FAILURES_BEFORE_DISABLING - 1; i++) {
            reportFailure(p);
        }
        QVERIFY(!isDisabledForSession(p));

        reportFailure(p);
        QVERIFY(isDisabledForSession(p));
        QVERIFY(!isDisabledForSession(other));
        reportSuccess(p);
        QVERIFY(!isDisabledForSession(p));
    }
};

QTEST_GUILESS_MAIN(DownloadMirrorTest)

#include "DownloadMirror_test.moc"
