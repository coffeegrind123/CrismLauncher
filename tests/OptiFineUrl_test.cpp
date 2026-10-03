#include <QTest>

#include <modplatform/helpers/OptiFineUtils.h>

// Download page markup and mirror URLs were checked live against optifine.net and BMCLAPI
class OptiFineUrlTest : public QObject {
    Q_OBJECT

   private slots:
    void detectOptiFine_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<bool>("isOptiFine");

        QTest::newRow("release") << "OptiFine_1.12.2_HD_U_F5.jar" << true;
        QTest::newRow("old release") << "OptiFine_1.6.4_HD_U_D1.jar" << true;
        QTest::newRow("preview") << "preview_OptiFine_1.20.1_HD_U_I5_pre4.jar" << true;
        QTest::newRow("snapshot preview") << "preview_OptiFine_21w08b_HD_U_G9_pre2.jar" << true;
        QTest::newRow("other mod") << "journeymap-1.7.10-5.1.4p2-fairplay.jar" << false;
        QTest::newRow("installer suffix") << "OptiFine_1.12.2_HD_U_F5_MOD.jar" << false;
    }

    void detectOptiFine()
    {
        QFETCH(QString, fileName);
        QFETCH(bool, isOptiFine);

        QCOMPARE(OptiFine::isOptiFineFile(fileName), isOptiFine);
    }

    void parseDownloadPage_data()
    {
        QTest::addColumn<QByteArray>("html");
        QTest::addColumn<QString>("url");

        QTest::newRow("live markup")
            << QByteArray("<td><span class='downloadButton'>\n"
                          "    <a href='downloadx?f=OptiFine_1.12.2_HD_U_F5.jar&x=93ff2cfe89ae8517571eaa8885690e4c' "
                          "onclick='onDownload()'>Download</a>\n")
            << "https://optifine.net/downloadx?f=OptiFine_1.12.2_HD_U_F5.jar&x=93ff2cfe89ae8517571eaa8885690e4c";
        QTest::newRow("escaped ampersand") << QByteArray("<a href=\"downloadx?f=OptiFine_1.7.10_HD_U_E7.jar&amp;x=abc123\">")
                                           << "https://optifine.net/downloadx?f=OptiFine_1.7.10_HD_U_E7.jar&x=abc123";
        QTest::newRow("no link") << QByteArray("<html><body>Rate limited</body></html>") << "";
        QTest::newRow("empty") << QByteArray() << "";
    }

    void parseDownloadPage()
    {
        QFETCH(QByteArray, html);
        QFETCH(QString, url);

        QCOMPARE(OptiFine::parseDownloadPage(html), url);
    }

    void pageUrl()
    {
        QCOMPARE(OptiFine::downloadPageUrl("OptiFine_1.12.2_HD_U_F5.jar"), "https://optifine.net/adloadx?f=OptiFine_1.12.2_HD_U_F5.jar");
    }

    void mirrorUrl_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<QString>("url");

        QTest::newRow("release") << "OptiFine_1.12.2_HD_U_F5.jar" << "https://bmclapi2.bangbang93.com/optifine/1.12.2/HD_U/F5";
        QTest::newRow("preview") << "preview_OptiFine_1.14_HD_U_F1_pre6.jar"
                                 << "https://bmclapi2.bangbang93.com/optifine/1.14/HD_U_F1/pre6";
        QTest::newRow("1.8 is keyed 1.8.0") << "OptiFine_1.8_HD_U_I7.jar" << "https://bmclapi2.bangbang93.com/optifine/1.8.0/HD_U/I7";
        QTest::newRow("1.8.9 untouched") << "OptiFine_1.8.9_HD_U_I7.jar" << "https://bmclapi2.bangbang93.com/optifine/1.8.9/HD_U/I7";
        QTest::newRow("not optifine") << "fastcraft-1.21.jar" << "";
    }

    void mirrorUrl()
    {
        QFETCH(QString, fileName);
        QFETCH(QString, url);

        QCOMPARE(OptiFine::mirrorUrl(fileName), url);
    }
};

QTEST_GUILESS_MAIN(OptiFineUrlTest)

#include "OptiFineUrl_test.moc"
