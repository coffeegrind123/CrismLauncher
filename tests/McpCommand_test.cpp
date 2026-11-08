#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <mcp/McpCommandOps.h>

using Mcp::CommandTask;

class McpCommandTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    // Runs the task and pumps events until it finished
    static bool runToEnd(CommandTask& task, int timeoutMs = 20000)
    {
        QSignalSpy finished(&task, &Task::finished);
        task.start();
        return finished.count() > 0 || finished.wait(timeoutMs);
    }

   private slots:
    void capturesOutputAndExitCode()
    {
#ifdef Q_OS_WIN
        CommandTask task("echo hello& echo oops 1>&2& exit /b 3", {}, {}, m_dir.path(), 30);
#else
        CommandTask task("echo hello; echo oops 1>&2; exit 3", {}, {}, m_dir.path(), 30);
#endif
        QVERIFY(runToEnd(task));
        QVERIFY(task.wasSuccessful());
        const auto result = task.resultJson();
        QCOMPARE(result["exitCode"].toInt(), 3);
        QVERIFY(result["stdout"].toString().contains("hello"));
        QVERIFY(result["stderr"].toString().contains("oops"));
    }

    void runsInTheGivenFolder()
    {
#ifdef Q_OS_WIN
        CommandTask task("cd", {}, {}, m_dir.path(), 30);
#else
        CommandTask task("pwd", {}, {}, m_dir.path(), 30);
#endif
        QVERIFY(runToEnd(task));
        const auto out = task.resultJson()["stdout"].toString().trimmed();
        QCOMPARE(QFileInfo(out).canonicalFilePath(), QFileInfo(m_dir.path()).canonicalFilePath());
    }

    void killsOnTimeout()
    {
#ifdef Q_OS_WIN
        CommandTask task("ping -n 30 127.0.0.1", {}, {}, m_dir.path(), 1);
#else
        CommandTask task("sleep 30", {}, {}, m_dir.path(), 1);
#endif
        QVERIFY(runToEnd(task, 15000));
        QVERIFY(!task.wasSuccessful());
        QVERIFY(task.resultJson()["timedOut"].toBool());
    }

    void missingProgramFails()
    {
        CommandTask task({}, "this-program-does-not-exist-crism", {}, m_dir.path(), 10);
        QVERIFY(runToEnd(task));
        QVERIFY(!task.wasSuccessful());
    }
};

QTEST_GUILESS_MAIN(McpCommandTest)

#include "McpCommand_test.moc"
