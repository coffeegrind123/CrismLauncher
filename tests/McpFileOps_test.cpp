#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <mcp/McpFileOps.h>

class McpFileOpsTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_root;

   private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_root = m_dir.filePath("instance");
        QVERIFY(QDir().mkpath(m_root + "/minecraft/config"));
        QVERIFY(QDir().mkpath(m_dir.filePath("outside")));
    }

    void insideIsAllowed_data()
    {
        QTest::addColumn<QString>("relative");
        QTest::addColumn<QString>("expected");

        QTest::newRow("file") << "minecraft/options.txt" << "minecraft/options.txt";
        QTest::newRow("new nested") << "minecraft/config/new/a.toml" << "minecraft/config/new/a.toml";
        QTest::newRow("root") << "." << "";
        QTest::newRow("dot dot that stays inside") << "minecraft/../minecraft/x" << "minecraft/x";
    }

    void insideIsAllowed()
    {
        QFETCH(QString, relative);
        QFETCH(QString, expected);
        const auto resolved = Mcp::resolveInsideRoot(m_root, relative);
        QVERIFY2(resolved.has_value(), qPrintable(resolved ? QString() : resolved.error()));
        QCOMPARE(QDir(m_root).relativeFilePath(*resolved), expected.isEmpty() ? QString(".") : expected);
    }

    void outsideIsRefused_data()
    {
        QTest::addColumn<QString>("relative");

        QTest::newRow("parent") << "../outside/x";
        QTest::newRow("deep parent") << "minecraft/../../outside";
        QTest::newRow("absolute") << QDir::rootPath() + "etc/passwd";
        QTest::newRow("home") << "~/x";
        QTest::newRow("empty") << "";
        QTest::newRow("sibling prefix") << "../instance-evil/x";
    }

    void outsideIsRefused()
    {
        QFETCH(QString, relative);
        QVERIFY(!Mcp::resolveInsideRoot(m_root, relative).has_value());
    }

    void symlinkEscapeIsRefused()
    {
#ifdef Q_OS_WIN
        QSKIP("Creating symlinks needs extra rights on Windows");
#else
        QVERIFY(QFile::link(m_dir.filePath("outside"), m_root + "/minecraft/escape"));
        QVERIFY(!Mcp::resolveInsideRoot(m_root, "minecraft/escape").has_value());
        QVERIFY(!Mcp::resolveInsideRoot(m_root, "minecraft/escape/new-file").has_value());
#endif
    }
};

QTEST_GUILESS_MAIN(McpFileOpsTest)

#include "McpFileOps_test.moc"
