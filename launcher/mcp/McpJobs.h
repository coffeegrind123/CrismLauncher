// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

#include "tasks/Task.h"

namespace Mcp {

// A launcher Task started on an agent's behalf. Agents get its id back immediately and poll, wait or cancel through
// the `jobs` tool, so a long install never holds an HTTP request open.
struct JobInfo {
    enum class State { Running, Succeeded, Failed, Aborted };

    QString id;
    QString tokenId;
    QString tokenName;
    QString title;
    State state = State::Running;
    qint64 progress = 0;
    qint64 total = 0;
    QString status;
    QString error;
    QDateTime started;
    QDateTime finished;
    QJsonObject result;  // filled by the job's result builder once it succeeded

    bool isFinished() const { return state != State::Running; }
    QJsonObject toJson() const;
};

QString stateName(JobInfo::State state);

class JobRegistry : public QObject {
    Q_OBJECT

   public:
    static constexpr int MAX_FINISHED_KEPT = 100;

    using ResultBuilder = std::function<QJsonObject()>;
    using Waiter = std::function<void(const JobInfo&)>;

    explicit JobRegistry(QObject* parent = nullptr);

    // Takes ownership of running `task`; `onSuccess` builds the job's result when it succeeds
    QString start(const QString& tokenId, const QString& tokenName, const QString& title, const Task::Ptr& task,
                  ResultBuilder onSuccess = {});

    std::optional<JobInfo> get(const QString& id) const;
    // Jobs of one token, newest first
    QList<JobInfo> list(const QString& tokenId) const;
    QList<JobInfo> running() const;

    bool cancel(const QString& id);
    // The Stop button: aborts every running job of every token
    void cancelAll();

    // Calls `waiter` once the job finishes, or right away if it already has
    void whenFinished(const QString& id, Waiter waiter);

   signals:
    void jobChanged(const QString& id);

   private:
    struct Entry {
        JobInfo info;
        Task::Ptr task;
        ResultBuilder onSuccess;
        QList<Waiter> waiters;
    };

    void finish(const QString& id, JobInfo::State state, const QString& error);
    void prune();

    QHash<QString, Entry> m_jobs;
    QList<QString> m_order;  // oldest first
};

}  // namespace Mcp
