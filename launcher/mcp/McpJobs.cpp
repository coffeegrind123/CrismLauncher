// SPDX-License-Identifier: GPL-3.0-only
#include "McpJobs.h"

#include <QUuid>

#include <utility>

namespace Mcp {

QString stateName(JobInfo::State state)
{
    switch (state) {
        case JobInfo::State::Running:
            return "running";
        case JobInfo::State::Succeeded:
            return "succeeded";
        case JobInfo::State::Failed:
            return "failed";
        case JobInfo::State::Aborted:
            return "aborted";
    }
    return {};
}

QJsonObject JobInfo::toJson() const
{
    QJsonObject out{
        { "id", id },
        { "title", title },
        { "state", stateName(state) },
        { "started", started.toString(Qt::ISODate) },
    };
    if (total > 0) {
        out["progress"] = progress;
        out["total"] = total;
    }
    if (!status.isEmpty()) {
        out["status"] = status;
    }
    if (!error.isEmpty()) {
        out["error"] = error;
    }
    if (finished.isValid()) {
        out["finished"] = finished.toString(Qt::ISODate);
    }
    if (!result.isEmpty()) {
        out["result"] = result;
    }
    return out;
}

JobRegistry::JobRegistry(QObject* parent) : QObject(parent) {}

QString JobRegistry::start(const QString& tokenId, const QString& tokenName, const QString& title, const Task::Ptr& task,
                           ResultBuilder onSuccess)
{
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

    Entry entry;
    entry.info.id = id;
    entry.info.tokenId = tokenId;
    entry.info.tokenName = tokenName;
    entry.info.title = title;
    entry.info.started = QDateTime::currentDateTimeUtc();
    entry.info.updated = entry.info.started;
    entry.task = task;
    entry.onSuccess = std::move(onSuccess);
    m_jobs.insert(id, entry);
    m_order.append(id);

    connect(task.get(), &Task::progress, this, [this, id](qint64 current, qint64 total) {
        if (auto it = m_jobs.find(id); it != m_jobs.end()) {
            it->info.progress = current;
            it->info.total = total;
            it->info.updated = QDateTime::currentDateTimeUtc();
            emit jobChanged(id);
        }
    });
    connect(task.get(), &Task::status, this, [this, id](const QString& status) {
        if (auto it = m_jobs.find(id); it != m_jobs.end()) {
            it->info.status = status;
            it->info.updated = QDateTime::currentDateTimeUtc();
            emit jobChanged(id);
        }
    });
    connect(task.get(), &Task::succeeded, this, [this, id] { finish(id, JobInfo::State::Succeeded, {}); });
    connect(task.get(), &Task::failed, this, [this, id](const QString& reason) { finish(id, JobInfo::State::Failed, reason); });
    connect(task.get(), &Task::aborted, this, [this, id] { finish(id, JobInfo::State::Aborted, tr("Cancelled")); });

    emit jobChanged(id);
    if (!task->isRunning()) {
        task->start();
    }
    return id;
}

void JobRegistry::finish(const QString& id, JobInfo::State state, const QString& error)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || it->info.isFinished()) {
        return;
    }
    it->info.state = state;
    it->info.error = error;
    it->info.finished = QDateTime::currentDateTimeUtc();
    it->info.updated = it->info.finished;
    if (state == JobInfo::State::Succeeded && it->onSuccess) {
        it->info.result = it->onSuccess();
    }
    // Safe inside the task's own signal: shared_qobject_ptr deletes with deleteLater()
    it->task.reset();

    const auto info = it->info;
    const auto waiters = std::exchange(it->waiters, {});
    emit jobChanged(id);
    for (const auto& waiter : waiters) {
        waiter(info);
    }
    prune();
}

void JobRegistry::prune()
{
    qsizetype finished = 0;
    for (const auto& id : m_order) {
        if (m_jobs.value(id).info.isFinished()) {
            ++finished;
        }
    }
    for (qsizetype i = 0; i < m_order.size() && finished > MAX_FINISHED_KEPT;) {
        const auto& id = m_order[i];
        if (m_jobs.value(id).info.isFinished()) {
            m_jobs.remove(id);
            m_order.removeAt(i);
            --finished;
            continue;
        }
        ++i;
    }
}

std::optional<JobInfo> JobRegistry::get(const QString& id) const
{
    if (auto it = m_jobs.constFind(id); it != m_jobs.cend()) {
        return it->info;
    }
    return std::nullopt;
}

QList<JobInfo> JobRegistry::list(const QString& tokenId) const
{
    QList<JobInfo> out;
    for (auto it = m_order.crbegin(); it != m_order.crend(); ++it) {
        const auto& info = m_jobs.value(*it).info;
        if (info.tokenId == tokenId) {
            out.append(info);
        }
    }
    return out;
}

QList<JobInfo> JobRegistry::running() const
{
    QList<JobInfo> out;
    for (const auto& id : m_order) {
        if (const auto& info = m_jobs.value(id).info; !info.isFinished()) {
            out.append(info);
        }
    }
    return out;
}

bool JobRegistry::cancel(const QString& id)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || it->info.isFinished() || !it->task) {
        return false;
    }
    // Hold a reference: abort() may finish the job synchronously, which drops the registry's copy
    const auto task = it->task;
    if (!task->canAbort() || !task->abort()) {
        return false;
    }
    finish(id, JobInfo::State::Aborted, tr("Cancelled"));
    return true;
}

void JobRegistry::cancelAll()
{
    for (const auto& info : running()) {
        cancel(info.id);
    }
}

void JobRegistry::whenFinished(const QString& id, Waiter waiter)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        return;
    }
    if (it->info.isFinished()) {
        waiter(it->info);
        return;
    }
    it->waiters.append(std::move(waiter));
}

}  // namespace Mcp
