// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "tasks/Task.h"

// Lets an agent run programs on the user's computer. It is arbitrary code execution, so it takes two switches the
// user sets by hand: the launcher-wide "Allow agents to run commands" setting (McpAllowCommands) and the token's
// `commands` scope. Every run counts as destructive, so the default confirmation policy asks each time.
namespace Mcp {

class ToolRegistry;

// One program run as a launcher Task: output captured (the last MAX_OUTPUT_BYTES of each stream), killed on timeout
// or cancel
class CommandTask : public Task {
    Q_OBJECT

   public:
    static constexpr qsizetype MAX_OUTPUT_BYTES = 512 * 1024;

    // `shellLine` non-empty: run it through the platform shell (cmd.exe /c, /bin/sh -c); otherwise `program` + `arguments`
    CommandTask(QString shellLine, QString program, QStringList arguments, QString workingDirectory, int timeoutSeconds);

    QJsonObject resultJson() const;
    bool canAbort() const override { return true; }
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    void append(QByteArray& buffer, const QByteArray& data);

    QString m_shellLine;
    QString m_program;
    QStringList m_arguments;
    QString m_workingDirectory;
    int m_timeoutSeconds;
    QProcess m_process;
    QTimer m_timeout;
    QByteArray m_stdout;
    QByteArray m_stderr;
    bool m_timedOut = false;
    bool m_truncated = false;
    int m_exitCode = -1;
};

void registerCommandOps(ToolRegistry& ops);

}  // namespace Mcp
