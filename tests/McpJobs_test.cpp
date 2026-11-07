#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>

#include <mcp/McpJobs.h>

using namespace Mcp;

namespace {
// A task finished by the test, not by itself
class ManualTask : public Task {
    Q_OBJECT
   public:
    ManualTask() { setAbortable(true); }
    void succeed() { emitSucceeded(); }
    void fail(const QString& reason) { emitFailed(reason); }

   protected:
    void executeTask() override {}
};
}  // namespace

class McpJobsTest : public QObject {
    Q_OBJECT

   private slots:
    void succeedsWithResult()
    {
        JobRegistry registry;
        auto task = makeShared<ManualTask>();
        const auto id = registry.start("token-a", "Agent", "Install things", task, [] { return QJsonObject{ { "installed", 3 } }; });
        QVERIFY(task->isRunning());

        auto info = registry.get(id);
        QVERIFY(info.has_value());
        QVERIFY(info->state == JobInfo::State::Running);

        task->setProgress(2, 5);
        task->setStatus("Downloading");
        info = registry.get(id);
        QCOMPARE(info->progress, 2);
        QCOMPARE(info->total, 5);
        QCOMPARE(info->status, QString("Downloading"));

        bool waited = false;
        registry.whenFinished(id, [&waited](const JobInfo& job) { waited = job.state == JobInfo::State::Succeeded; });
        task->succeed();
        QVERIFY(waited);

        info = registry.get(id);
        QVERIFY(info->state == JobInfo::State::Succeeded);
        QCOMPARE(info->result["installed"].toInt(), 3);
        QVERIFY(info->finished.isValid());
        QCOMPARE(info->toJson()["state"].toString(), QString("succeeded"));

        // The registry let go of the task; it must survive until its own signal emission returned
        QTest::qWait(10);
    }

    void failureCarriesReason()
    {
        JobRegistry registry;
        auto task = makeShared<ManualTask>();
        const auto id = registry.start("t", "Agent", "Fails", task);
        task->fail("disk full");
        const auto info = registry.get(id);
        QVERIFY(info->state == JobInfo::State::Failed);
        QCOMPARE(info->error, QString("disk full"));
    }

    void cancel()
    {
        JobRegistry registry;
        auto task = makeShared<ManualTask>();
        const auto id = registry.start("t", "Agent", "Cancel me", task);
        QVERIFY(registry.cancel(id));
        QVERIFY(registry.get(id)->state == JobInfo::State::Aborted);
        QVERIFY(!registry.cancel(id));
        QVERIFY(!registry.cancel("missing"));
    }

    void whenFinishedAfterTheFact()
    {
        JobRegistry registry;
        auto task = makeShared<ManualTask>();
        const auto id = registry.start("t", "Agent", "Done", task);
        task->succeed();
        bool called = false;
        registry.whenFinished(id, [&called](const JobInfo&) { called = true; });
        QVERIFY(called);
    }

    void listIsPerTokenNewestFirst()
    {
        JobRegistry registry;
        auto first = makeShared<ManualTask>();
        auto second = makeShared<ManualTask>();
        auto other = makeShared<ManualTask>();
        const auto a = registry.start("mine", "Agent", "first", first);
        const auto b = registry.start("mine", "Agent", "second", second);
        registry.start("theirs", "Other", "other", other);

        const auto mine = registry.list("mine");
        QCOMPARE(mine.size(), 2);
        QCOMPARE(mine[0].id, b);
        QCOMPARE(mine[1].id, a);
        QCOMPARE(registry.running().size(), 3);

        registry.cancelAll();
        QVERIFY(registry.running().isEmpty());
    }

    void prunesOldFinishedJobs()
    {
        JobRegistry registry;
        QString firstId;
        for (int i = 0; i < JobRegistry::MAX_FINISHED_KEPT + 5; ++i) {
            auto task = makeShared<ManualTask>();
            const auto id = registry.start("t", "Agent", QString::number(i), task);
            if (i == 0) {
                firstId = id;
            }
            task->succeed();
        }
        QVERIFY(!registry.get(firstId).has_value());
        QCOMPARE(registry.list("t").size(), JobRegistry::MAX_FINISHED_KEPT);
    }
};

QTEST_GUILESS_MAIN(McpJobsTest)

#include "McpJobs_test.moc"
