// SPDX-License-Identifier: GPL-3.0-only
#include "McpCommandOps.h"

#include <QDir>
#include <QJsonArray>

#include <memory>
#include <utility>

#include "Application.h"
#include "mcp/McpCommon.h"
#include "mcp/McpFileOps.h"
#include "mcp/McpJobs.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "settings/SettingsObject.h"

namespace Mcp {

namespace {
constexpr auto SETTING_ALLOW_COMMANDS = "McpAllowCommands";
constexpr int DEFAULT_TIMEOUT_SECONDS = 120;
constexpr int MAX_TIMEOUT_SECONDS = 60 * 60;
constexpr int DEFAULT_WAIT_SECONDS = 60;
constexpr int MAX_WAIT_SECONDS = 120;
}  // namespace

CommandTask::CommandTask(QString shellLine, QString program, QStringList arguments, QString workingDirectory, int timeoutSeconds)
    : m_shellLine(std::move(shellLine))
    , m_program(std::move(program))
    , m_arguments(std::move(arguments))
    , m_workingDirectory(std::move(workingDirectory))
    , m_timeoutSeconds(timeoutSeconds)
{
    setAbortable(true);
    m_timeout.setSingleShot(true);
}

void CommandTask::append(QByteArray& buffer, const QByteArray& data)
{
    buffer.append(data);
    if (buffer.size() > MAX_OUTPUT_BYTES) {
        buffer.remove(0, buffer.size() - MAX_OUTPUT_BYTES);
        m_truncated = true;
    }
}

void CommandTask::executeTask()
{
    if (!m_shellLine.isEmpty()) {
#ifdef Q_OS_WIN
        m_program = qEnvironmentVariable("ComSpec", "cmd.exe");
        m_arguments = { "/d", "/s", "/c", m_shellLine };
#else
        m_program = "/bin/sh";
        m_arguments = { "-c", m_shellLine };
#endif
    }
    setStatus(tr("Running %1").arg(m_shellLine.isEmpty() ? m_program : m_shellLine));

    m_process.setWorkingDirectory(m_workingDirectory);
    m_process.setProgram(m_program);
    m_process.setArguments(m_arguments);
    // No input: a command waiting on stdin would otherwise hang until the timeout
    m_process.setStandardInputFile(QProcess::nullDevice());

    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { append(m_stdout, m_process.readAllStandardOutput()); });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { append(m_stderr, m_process.readAllStandardError()); });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && isRunning()) {
            m_timeout.stop();
            emitFailed(tr("Could not start %1: %2").arg(m_program, m_process.errorString()));
        }
    });
    connect(&m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        m_timeout.stop();
        append(m_stdout, m_process.readAllStandardOutput());
        append(m_stderr, m_process.readAllStandardError());
        m_exitCode = exitCode;
        if (!isRunning()) {
            return;
        }
        if (m_timedOut) {
            emitFailed(tr("Stopped after %1 seconds (timeout)").arg(m_timeoutSeconds));
            return;
        }
        // A non-zero exit code is still a finished run: the agent reads it from the result
        emitSucceeded();
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        m_process.kill();
    });

    m_timeout.start(m_timeoutSeconds * 1000);
    m_process.start();
}

bool CommandTask::abort()
{
    if (!isRunning()) {
        return true;
    }
    m_timeout.stop();
    m_process.kill();
    emitAborted();
    return true;
}

QJsonObject CommandTask::resultJson() const
{
    QJsonObject out{
        { "exitCode", m_exitCode },
        { "stdout", QString::fromLocal8Bit(m_stdout) },
        { "stderr", QString::fromLocal8Bit(m_stderr) },
    };
    if (m_timedOut) {
        out["timedOut"] = true;
    }
    if (m_truncated) {
        out["truncated"] = true;
    }
    return out;
}

namespace {
void runCommand(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    if (!APPLICATION->settings()->get(SETTING_ALLOW_COMMANDS).toBool()) {
        reply(ToolResult::error("Running commands is turned off in the launcher.",
                                "Only the user can turn it on: Settings > AI agents (MCP) > Allow agents to run commands."));
        return;
    }
    auto* registry = jobs();
    if (!registry) {
        reply(ToolResult::error("Jobs are not available."));
        return;
    }

    auto cwd = resolveInsideRoot(APPLICATION->dataRoot(), arguments.value("cwd").toString("."));
    if (!cwd) {
        reply(ToolResult::error(cwd.error(), "cwd is relative to the launcher's data folder, e.g. instances/<id>/minecraft."));
        return;
    }
    if (!QDir(*cwd).exists()) {
        reply(ToolResult::error(QString("The folder %1 does not exist.").arg(arguments.value("cwd").toString())));
        return;
    }

    const auto line = arguments.value("command").toString();
    QStringList programArguments;
    for (const auto& value : arguments.value("arguments").toArray()) {
        programArguments.append(value.toString());
    }
    const bool useShell = arguments.value("shell").toBool(true);
    const int timeout = arguments.value("timeoutSeconds").toInt(DEFAULT_TIMEOUT_SECONDS);
    const int wait = arguments.value("waitSeconds").toInt(DEFAULT_WAIT_SECONDS);

    auto* command = new CommandTask(useShell ? line : QString(), useShell ? QString() : line, programArguments, *cwd, timeout);
    Task::Ptr task(command);
    const auto id = registry->start(context.token.id, context.token.name, QString("Run: %1").arg(line.left(80)), task,
                                    [command] { return command->resultJson(); });

    // Short commands answer in the call itself; long ones continue as a job (or a task, for clients that support them)
    auto replied = std::make_shared<bool>(false);
    registry->whenFinished(id, [id, reply, replied](const JobInfo& job) {
        if (std::exchange(*replied, true)) {
            return;
        }
        auto out = job.result;
        out["job"] = id;
        if (job.state != JobInfo::State::Succeeded) {
            out["error"] = job.error;
        }
        auto result = ToolResult::ok(out);
        result.isError = job.state != JobInfo::State::Succeeded;
        reply(result);
    });
    QTimer::singleShot(wait * 1000, registry, [id, reply, replied] {
        if (std::exchange(*replied, true)) {
            return;
        }
        auto result = ToolResult::ok(QJsonObject{ { "job", id }, { "running", true } },
                                     QString("Still running as job %1; jobs action=wait returns its output when it finishes.").arg(id));
        result.jobId = id;
        reply(result);
    });
}
}  // namespace

void registerCommandOps(ToolRegistry& ops)
{
    using namespace Schema;

    Tool tool;
    tool.name = "system.run";
    tool.description =
        "Runs a program on the user's computer, starting in the launcher's data folder (or `cwd` inside it), and returns its exit "
        "code, stdout and stderr (the last 512 KiB of each). By default `command` is one line for the system shell (cmd.exe on "
        "Windows, /bin/sh elsewhere); with shell=false it is a program run directly with `arguments`. There is no input; a run is "
        "killed after timeoutSeconds. Runs longer than waitSeconds continue as a job. Only available when the user has turned "
        "on \"Allow agents to run commands\" and given this token the 'commands' scope; each run is confirmed under the "
        "default policy. Prefer the dedicated tools when one does the job.";
    tool.inputSchema = object(
        {
            { "command", string("Shell line, or the program when shell=false") },
            { "shell", boolean("Run through the system shell (default true)") },
            { "arguments", withDescription(array(string({})), "Program arguments when shell=false") },
            { "cwd", string("Folder to start in, relative to the launcher's data folder (default the data folder)") },
            { "timeoutSeconds", integer("Kill the run after this long (default 120)", 1, MAX_TIMEOUT_SECONDS) },
            { "waitSeconds", integer("Answer within this long; longer runs continue as a job (default 60)", 1, MAX_WAIT_SECONDS) },
        },
        { "command" });
    tool.scope = Scope::Commands;
    tool.annotations.readOnly = false;
    tool.annotations.destructive = true;
    tool.annotations.idempotent = false;
    tool.annotations.openWorld = true;
    tool.describeAction = [](const QJsonObject& a) {
        auto text = QString("Run on your computer: %1").arg(a.value("command").toString());
        if (const auto args = a.value("arguments").toArray(); !args.isEmpty() && !a.value("shell").toBool(true)) {
            QStringList list;
            for (const auto& value : args) {
                list.append(value.toString());
            }
            text += " " + list.join(' ');
        }
        return text + QString("\n(in %1)").arg(a.value("cwd").toString("the launcher's data folder"));
    };
    tool.handler = runCommand;
    ops.add(tool);
}

}  // namespace Mcp
