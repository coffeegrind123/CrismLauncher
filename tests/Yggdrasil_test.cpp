#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <minecraft/auth/AccountData.h>
#include <minecraft/auth/Yggdrasil.h>

// Metadata and latest.json fixtures were captured from the live servers; error bodies are the ones
// Ely.by and LittleSkin return for a wrong password
class YggdrasilTest : public QObject {
    Q_OBJECT

    QByteArray readTestData(const QString& name)
    {
        QFile file(QFINDTESTDATA("testdata/Yggdrasil/" + name));
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("Could not open test data %s", qPrintable(name));
        }
        return file.readAll();
    }

   private slots:
    void normalizeServerInput_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("url");

        QTest::newRow("bare host") << "authserver.ely.by" << "https://authserver.ely.by";
        QTest::newRow("trailing slash") << "https://littleskin.cn/api/yggdrasil/" << "https://littleskin.cn/api/yggdrasil";
        QTest::newRow("whitespace") << "  littleskin.cn  " << "https://littleskin.cn";
        QTest::newRow("http kept") << "http://localhost:25585/" << "http://localhost:25585";
        QTest::newRow("drag link") << "authlib-injector:yggdrasil-server:https%3A%2F%2Flittleskin.cn%2Fapi%2Fyggdrasil"
                                   << "https://littleskin.cn/api/yggdrasil";
        QTest::newRow("empty") << "" << "";
        QTest::newRow("other scheme") << "ftp://example.com" << "";
        QTest::newRow("no host") << "https://" << "";
    }

    void normalizeServerInput()
    {
        QFETCH(QString, input);
        QFETCH(QString, url);

        const auto result = Yggdrasil::normalizeServerInput(input);
        if (url.isEmpty()) {
            QVERIFY(!result.isValid());
        } else {
            QCOMPARE(result.toString(), url);
        }
    }

    void resolveApiLocation_data()
    {
        QTest::addColumn<QString>("responseUrl");
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<QString>("apiRoot");

        // authserver.ely.by sends a relative location, littleskin.cn an absolute one
        QTest::newRow("relative") << "https://authserver.ely.by/" << QByteArray("/api/authlib-injector")
                                  << "https://authserver.ely.by/api/authlib-injector";
        QTest::newRow("absolute") << "https://littleskin.cn/" << QByteArray("https://littleskin.cn/api/yggdrasil")
                                  << "https://littleskin.cn/api/yggdrasil";
        QTest::newRow("no header") << "https://littleskin.cn/api/yggdrasil" << QByteArray() << "https://littleskin.cn/api/yggdrasil";
        QTest::newRow("trailing slash") << "https://example.com/" << QByteArray("api/yggdrasil/") << "https://example.com/api/yggdrasil";
    }

    void resolveApiLocation()
    {
        QFETCH(QString, responseUrl);
        QFETCH(QByteArray, header);
        QFETCH(QString, apiRoot);

        QCOMPARE(Yggdrasil::resolveApiLocation(QUrl(responseUrl), header).toString(), apiRoot);
    }

    void metadata()
    {
        const auto ely = readTestData("elyby-metadata.json");
        const auto littleSkin = readTestData("littleskin-metadata.json");

        QVERIFY(Yggdrasil::isMetadata(ely));
        QVERIFY(Yggdrasil::isMetadata(littleSkin));
        QVERIFY(!Yggdrasil::isMetadata("<html></html>"));
        QVERIFY(!Yggdrasil::isMetadata("{\"meta\": \"not an object\"}"));

        const auto elyBase64 = QString::fromLatin1(ely.toBase64());
        QCOMPARE(Yggdrasil::serverNameFromMetadata(elyBase64), "Ely.by");
        QCOMPARE(Yggdrasil::linkFromMetadata(elyBase64, "register"), "https://account.ely.by/register");
        QCOMPARE(Yggdrasil::serverNameFromMetadata(QString::fromLatin1(littleSkin.toBase64())), "LittleSkin");
        QCOMPARE(Yggdrasil::serverNameFromMetadata(""), "");
    }

    void parseAuthResponse()
    {
        auto parsed = Yggdrasil::parseAuthResponse("{\"accessToken\":\"at\",\"clientToken\":\"ct\",\"availableProfiles\":[{\"id\":\"a1\",\"name\":\"Alice\"},{\"id\":\"b2\",\"name\":\"Bob\"}],\"selectedProfile\":{\"id\":\"a1\",\"name\":\"Alice\"}}");
        QVERIFY(std::holds_alternative<Yggdrasil::AuthResponse>(parsed));
        auto response = std::get<Yggdrasil::AuthResponse>(parsed);
        QCOMPARE(response.accessToken, "at");
        QCOMPARE(response.clientToken, "ct");
        QVERIFY(response.selectedProfile.has_value());
        QCOMPARE(response.selectedProfile->name, "Alice");
        QCOMPARE(response.availableProfiles.size(), 2);

        parsed = Yggdrasil::parseAuthResponse("{\"accessToken\":\"at\",\"availableProfiles\":[]}");
        response = std::get<Yggdrasil::AuthResponse>(parsed);
        QVERIFY(!response.selectedProfile.has_value());
        QVERIFY(response.availableProfiles.isEmpty());

        QVERIFY(std::holds_alternative<QString>(Yggdrasil::parseAuthResponse("{\"clientToken\":\"ct\"}")));
        QVERIFY(std::holds_alternative<QString>(Yggdrasil::parseAuthResponse("not json")));
    }

    void parseError()
    {
        auto error = Yggdrasil::parseError("{\"error\":\"ForbiddenOperationException\",\"errorMessage\":\"Invalid credentials. Invalid username or password.\"}");
        QVERIFY(error.has_value());
        QCOMPARE(error->error, "ForbiddenOperationException");
        QVERIFY(!Yggdrasil::isTwoFactorRequired(*error));

        error = Yggdrasil::parseError("{\"error\":\"ForbiddenOperationException\",\"errorMessage\":\"Account protected with two factor auth.\"}");
        QVERIFY(error.has_value());
        QVERIFY(Yggdrasil::isTwoFactorRequired(*error));

        QVERIFY(!Yggdrasil::parseError("{\"accessToken\":\"at\"}").has_value());
    }

    void parseAgentArtifact()
    {
        for (const auto* name : { "authlib-injector-latest.json", "authlib-injector-latest-bmclapi.json" }) {
            const auto artifact = Yggdrasil::parseAgentArtifact(readTestData(name));
            QVERIFY2(artifact.has_value(), name);
            QCOMPARE(artifact->version, "1.2.8");
            QCOMPARE(artifact->sha256.toHex(), "9c7f4343e6c82034958ffb48c14a2cb0c85928be7283103ce17da00c6d5a7b10");
        }

        const auto foreignHost = "{\"version\":\"1.2.8\",\"download_url\":\"https://evil.example/a.jar\",\"checksums\":{\"sha256\":\"9c7f4343e6c82034958ffb48c14a2cb0c85928be7283103ce17da00c6d5a7b10\"}}";
        QVERIFY(!Yggdrasil::parseAgentArtifact(foreignHost).has_value());

        const auto plainHttp = "{\"version\":\"1.2.8\",\"download_url\":\"http://authlib-injector.yushi.moe/a.jar\",\"checksums\":{\"sha256\":\"9c7f4343e6c82034958ffb48c14a2cb0c85928be7283103ce17da00c6d5a7b10\"}}";
        QVERIFY(!Yggdrasil::parseAgentArtifact(plainHttp).has_value());

        const auto badHash = "{\"version\":\"1.2.8\",\"download_url\":\"https://authlib-injector.yushi.moe/a.jar\",\"checksums\":{\"sha256\":\"abc\"}}";
        QVERIFY(!Yggdrasil::parseAgentArtifact(badHash).has_value());
    }

    void agentArguments()
    {
        QCOMPARE(Yggdrasil::agentArguments("/c/ai.jar", "https://littleskin.cn/api/yggdrasil", "eyJ9"),
                 QStringList({ "-javaagent:/c/ai.jar=https://littleskin.cn/api/yggdrasil", "-Dauthlibinjector.yggdrasil.prefetched=eyJ9" }));
        QCOMPARE(Yggdrasil::agentArguments("/c/ai.jar", "https://x", ""), QStringList({ "-javaagent:/c/ai.jar=https://x" }));
        QCOMPARE(Yggdrasil::agentArguments("/c/ai.jar", "https://x", QString(20000, 'A')).size(), 1);
    }

    void accountRoundTrip()
    {
        AccountData account;
        account.type = AccountType::AuthlibInjector;
        account.authlibInjectorUrl = "https://littleskin.cn/api/yggdrasil";
        account.authlibInjectorMetadata = QString::fromLatin1(readTestData("littleskin-metadata.json").toBase64());
        account.yggdrasilToken.token = "at";
        account.yggdrasilToken.extra["userName"] = "steve@example.com";
        account.generateClientToken();
        account.minecraftProfile.id = "a1";
        account.minecraftProfile.name = "Alice";
        account.minecraftProfile.canUploadSkins = true;
        account.minecraftProfile.validity = Validity::Certain;

        const auto json = account.saveState();
        QCOMPARE(json.value("type").toString(), "AuthlibInjector");
        QCOMPARE(json.value("customAuthServerUrl").toString(), "https://littleskin.cn/api/yggdrasil/authserver");

        AccountData loaded;
        QVERIFY(loaded.resumeStateFromV3(json));
        QCOMPARE(loaded.type, AccountType::AuthlibInjector);
        QCOMPARE(loaded.authlibInjectorUrl, account.authlibInjectorUrl);
        QCOMPARE(loaded.authlibInjectorMetadata, account.authlibInjectorMetadata);
        QCOMPARE(loaded.accessToken(), "at");
        QCOMPARE(loaded.userName(), "steve@example.com");
        QCOMPARE(loaded.clientToken(), account.clientToken());
        QCOMPARE(loaded.serverName(), "LittleSkin");
        QCOMPARE(loaded.sessionServerUrl(), "https://littleskin.cn/api/yggdrasil/sessionserver");
        QVERIFY(loaded.minecraftProfile.canUploadSkins);
    }

    void fjordAccountImport()
    {
        // Fjord Launcher stores the endpoints but not always the API root
        const auto json = QJsonDocument::fromJson("{\"type\":\"AuthlibInjector\",\"customAuthServerUrl\":\"https://authserver.ely.by/api/authlib-injector/authserver\",\"ygg\":{\"token\":\"at\",\"extra\":{\"userName\":\"steve\",\"clientToken\":\"ct\"}}}")
                              .object();
        AccountData loaded;
        QVERIFY(loaded.resumeStateFromV3(json));
        QCOMPARE(loaded.authlibInjectorUrl, Yggdrasil::ELYBY_API_ROOT);

        AccountData legacyEly;
        QVERIFY(legacyEly.resumeStateFromV3(QJsonDocument::fromJson("{\"type\":\"Elyby\"}").object()));
        QCOMPARE(legacyEly.authlibInjectorUrl, Yggdrasil::ELYBY_API_ROOT);

        AccountData noServer;
        QVERIFY(!noServer.resumeStateFromV3(QJsonDocument::fromJson("{\"type\":\"AuthlibInjector\"}").object()));
    }
};

QTEST_GUILESS_MAIN(YggdrasilTest)

#include "Yggdrasil_test.moc"
