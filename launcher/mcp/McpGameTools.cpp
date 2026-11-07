// SPDX-License-Identifier: GPL-3.0-only
#include "McpGameTools.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include "Application.h"
#include "GZip.h"
#include "InstanceList.h"
#include "LaunchMode.h"
#include "java/JavaInstall.h"
#include "java/JavaInstallList.h"
#include "launch/LaunchTask.h"
#include "launch/LogModel.h"
#include "mcp/McpCommon.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "minecraft/launch/MinecraftTarget.h"
#include "settings/SettingsObject.h"

namespace Mcp {

namespace {
constexpr int DEFAULT_LOG_LINES = 200;
constexpr int MAX_LOG_LINES = 5000;
constexpr qint64 MAX_LOG_FILE_BYTES = 32 * 1024 * 1024;
constexpr QLatin1StringView LOGS_FOLDER("logs");
constexpr QLatin1StringView CRASH_FOLDER("crash-reports");

// An account by internal id or player name
MinecraftAccountPtr findAccount(const QString& key)
{
    auto* accounts = APPLICATION->accounts();
    for (int i = 0; i < accounts->count(); ++i) {
        const auto account = accounts->at(i);
        if (account->internalId() == key || account->profileName().compare(key, Qt::CaseInsensitive) == 0) {
            return account;
        }
    }
    return nullptr;
}

QString accountHint()
{
    return "Call accounts.list for account ids and names.";
}

QString instanceName(const QJsonObject& arguments)
{
    const auto id = arguments.value("instance").toString();
    auto* instance = APPLICATION->instances()->getInstanceById(id);
    return instance ? instance->name() : id;
}

// --- launch ---

void launchTool(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    const auto action = arguments.value("action").toString("start");

    if (action == "stop") {
        if (!instance->isRunning()) {
            reply(ToolResult::error(QString("'%1' is not running.").arg(instance->name())));
            return;
        }
        const bool killed = APPLICATION->kill(instance);
        reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "stopped", killed } }));
        return;
    }

    if (instance->isRunning()) {
        reply(ToolResult::error(QString("'%1' is already running.").arg(instance->name()), "Use action=stop first to restart it."));
        return;
    }

    MinecraftAccountPtr account;
    if (const auto key = arguments.value("account").toString(); !key.isEmpty()) {
        account = findAccount(key);
        if (!account) {
            reply(ToolResult::error(QString("No account '%1'.").arg(key), accountHint()));
            return;
        }
    }

    MinecraftTarget::Ptr target;
    if (const auto server = arguments.value("server").toString(); !server.isEmpty()) {
        target = std::make_shared<MinecraftTarget>(MinecraftTarget::parse(server, false));
    } else if (const auto world = arguments.value("world").toString(); !world.isEmpty()) {
        target = std::make_shared<MinecraftTarget>(MinecraftTarget::parse(world, true));
    }

    const auto offlineName = arguments.value("offlineName").toString();
    auto mode = LaunchMode::Normal;
    if (arguments.value("demo").toBool(false)) {
        mode = LaunchMode::Demo;
    } else if (!offlineName.isEmpty()) {
        mode = LaunchMode::Offline;
    }

    if (!APPLICATION->launch(instance, mode, target, account, offlineName)) {
        reply(ToolResult::error(QString("The launcher could not start '%1'.").arg(instance->name()), "See the launcher window for the reason."));
        return;
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "launching", true } },
                         "Launching. If the account needs signing in again, the launcher asks the user. Follow the game with logs "
                         "source=game, and instance_get for whether it is running."));
}

// --- logs ---

struct LogLine {
    QString text;
    int level = -1;
};

Result<QList<LogLine>> readLogFile(const QString& path)
{
    QFile file(path);
    if (file.size() > MAX_LOG_FILE_BYTES) {
        return std::unexpected(QString("%1 is too large to read (%2 MiB)").arg(QFileInfo(path).fileName()).arg(file.size() / (1024 * 1024)));
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QString("Could not read %1: %2").arg(path, file.errorString()));
    }
    auto data = file.readAll();
    if (path.endsWith(".gz")) {
        QByteArray unzipped;
        if (!GZip::unzip(data, unzipped)) {
            return std::unexpected(QString("Could not decompress %1").arg(QFileInfo(path).fileName()));
        }
        data = unzipped;
    }
    QList<LogLine> out;
    for (const auto& line : QString::fromUtf8(data).split('\n')) {
        out.append(LogLine{ line.endsWith('\r') ? line.chopped(1) : line, -1 });
    }
    return out;
}

QStringList logFiles(MinecraftInstance* instance)
{
    QStringList out;
    for (const auto& folder : { LOGS_FOLDER, CRASH_FOLDER }) {
        const QDir dir(QDir(instance->gameRoot()).filePath(folder));
        for (const auto& name : dir.entryList(QDir::Files, QDir::Time)) {
            out.append(QString("%1/%2").arg(folder, name));
        }
    }
    return out;
}

void logsTool(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    const auto source = arguments.value("source").toString("game");
    const int wanted = arguments.value("lines").toInt(DEFAULT_LOG_LINES);
    const auto grep = arguments.value("grep").toString();
    const auto minimumLevel = arguments.value("minLevel").toString();

    QList<LogLine> lines;
    QString origin;

    if (source == "game") {
        auto* launch = instance->getLaunchTask();
        if (!launch || !launch->getLogModel()) {
            reply(ToolResult::error(QString("'%1' has not run since the launcher started.").arg(instance->name()),
                                    "Use source=latest for the game's own latest.log, or source=crash."));
            return;
        }
        auto model = launch->getLogModel();
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto index = model->index(row, 0);
            lines.append(LogLine{ model->data(index, Qt::DisplayRole).toString(), model->data(index, LogModel::LevelRole).toInt() });
        }
        origin = "launcher console";
    } else {
        QString relative;
        if (source == "latest") {
            relative = QString("%1/latest.log").arg(LOGS_FOLDER);
        } else if (source == "crash") {
            const QDir crashes(QDir(instance->gameRoot()).filePath(CRASH_FOLDER));
            const auto newest = crashes.entryList({ "*.txt" }, QDir::Files, QDir::Time);
            if (newest.isEmpty()) {
                reply(ToolResult::error(QString("'%1' has no crash reports.").arg(instance->name())));
                return;
            }
            relative = QString("%1/%2").arg(CRASH_FOLDER, newest.first());
        } else {
            const auto available = logFiles(instance);
            relative = arguments.value("file").toString();
            if (relative.isEmpty()) {
                reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "files", QJsonArray::fromStringList(available) } }));
                return;
            }
            // Only names this listing produced: nothing outside the logs and crash-report folders can be read
            if (!available.contains(relative)) {
                reply(ToolResult::error(QString("No log file '%1'.").arg(relative), "Call logs source=file without `file` for the list."));
                return;
            }
        }
        const auto path = QDir(instance->gameRoot()).filePath(relative);
        if (!QFileInfo::exists(path)) {
            reply(ToolResult::error(QString("%1 does not exist yet.").arg(relative), "The game writes it once it has run."));
            return;
        }
        auto read = readLogFile(path);
        if (!read) {
            reply(ToolResult::error(read.error()));
            return;
        }
        lines = *read;
        origin = relative;
    }

    const auto level = minimumLevel.isEmpty() ? -1 : static_cast<int>(MessageLevel::fromName(minimumLevel).value());
    const QRegularExpression pattern(grep, QRegularExpression::CaseInsensitiveOption);
    if (!grep.isEmpty() && !pattern.isValid()) {
        reply(ToolResult::error(QString("Invalid grep pattern: %1").arg(pattern.errorString())));
        return;
    }
    QStringList kept;
    for (const auto& line : lines) {
        if (level >= 0 && line.level >= 0 && line.level < level) {
            continue;
        }
        if (!grep.isEmpty() && !pattern.match(line.text).hasMatch()) {
            continue;
        }
        kept.append(line.text);
    }
    const auto total = kept.size();
    if (kept.size() > wanted) {
        kept = kept.mid(kept.size() - wanted);
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() },
                                      { "source", origin },
                                      { "running", instance->isRunning() },
                                      { "matchedLines", total },
                                      { "returnedLines", kept.size() },
                                      { "text", kept.join('\n') } },
                         kept.join('\n')));
}

// --- account ops ---

void accountsSetDefault(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto account = findAccount(arguments.value("account").toString());
    if (!account) {
        reply(ToolResult::error(QString("No account '%1'.").arg(arguments.value("account").toString()), accountHint()));
        return;
    }
    APPLICATION->accounts()->setDefaultAccount(account);
    reply(ToolResult::ok(QJsonObject{ { "default", account->profileName() } }));
}

void accountsSetForInstance(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* settings = (*found)->settings();
    const auto key = arguments.value("account").toString();
    if (key.isEmpty()) {
        settings->set("UseAccountForInstance", false);
        settings->set("InstanceAccountId", "");
        reply(ToolResult::ok(QJsonObject{ { "instance", (*found)->id() }, { "account", QJsonValue::Null } }, "The instance uses the default account again."));
        return;
    }
    const auto account = findAccount(key);
    if (!account) {
        reply(ToolResult::error(QString("No account '%1'.").arg(key), accountHint()));
        return;
    }
    // The launcher keys instance accounts by profile id (see LaunchController::decideAccount)
    settings->set("UseAccountForInstance", true);
    settings->set("InstanceAccountId", account->profileId());
    reply(ToolResult::ok(QJsonObject{ { "instance", (*found)->id() }, { "account", account->profileName() } }));
}

void accountsAddOffline(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    static const QRegularExpression s_validName("^[A-Za-z0-9_]{3,16}$");
    const auto name = arguments.value("name").toString();
    if (!s_validName.match(name).hasMatch()) {
        reply(ToolResult::error("Offline names are 3-16 characters: letters, digits and underscores."));
        return;
    }
    if (findAccount(name)) {
        reply(ToolResult::error(QString("An account named '%1' already exists.").arg(name)));
        return;
    }
    auto account = MinecraftAccount::createOffline(name);
    APPLICATION->accounts()->addAccount(account);
    if (arguments.value("makeDefault").toBool(false)) {
        APPLICATION->accounts()->setDefaultAccount(account);
    }
    reply(ToolResult::ok(QJsonObject{ { "id", account->internalId() }, { "name", name } }));
}

void accountsRemove(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* accounts = APPLICATION->accounts();
    const auto account = findAccount(arguments.value("account").toString());
    if (!account) {
        reply(ToolResult::error(QString("No account '%1'.").arg(arguments.value("account").toString()), accountHint()));
        return;
    }
    if (account->isActive()) {
        reply(ToolResult::error(QString("'%1' is in use by a running game.").arg(account->profileName())));
        return;
    }
    for (int i = 0; i < accounts->count(); ++i) {
        if (accounts->at(i) == account) {
            accounts->removeAccount(accounts->index(i));
            break;
        }
    }
    reply(ToolResult::ok(QJsonObject{ { "removed", account->profileName() } }));
}

void accountsRefresh(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto account = findAccount(arguments.value("account").toString());
    if (!account) {
        reply(ToolResult::error(QString("No account '%1'.").arg(arguments.value("account").toString()), accountHint()));
        return;
    }
    APPLICATION->accounts()->requestRefresh(account->internalId());
    reply(ToolResult::ok(QJsonObject{ { "refreshing", account->profileName() } }, "Refresh queued; accounts.list shows the new state."));
}

void accountsOpenManager(const CallContext&, const QJsonObject&, const ToolReply& reply)
{
    APPLICATION->ShowGlobalSettings(nullptr, "accounts");
    reply(ToolResult::ok(QJsonObject{ { "opened", true } },
                         "Opened the launcher's account manager. Signing in to a Microsoft account is done there by the user; "
                         "agents never handle account credentials."));
}

// --- java ---

void javaListInstalls(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* list = APPLICATION->javalist();
    auto respond = [list, reply] {
        QJsonArray out;
        for (int i = 0; i < list->count(); ++i) {
            auto install = std::dynamic_pointer_cast<JavaInstall>(list->at(i));
            if (!install) {
                continue;
            }
            out.append(QJsonObject{ { "version", install->id.toString() },
                                    { "major", install->id.major() },
                                    { "arch", install->arch },
                                    { "path", install->path },
                                    { "is64bit", install->is_64bit } });
        }
        reply(ToolResult::ok(QJsonObject{ { "installs", out } }));
    };
    if (list->isLoaded() && !arguments.value("rescan").toBool(false)) {
        respond();
        return;
    }
    runTask(list->getLoadTask(arguments.value("rescan").toBool(false)), [respond, reply](bool ok, const QString& error) {
        if (!ok) {
            reply(ToolResult::error(QString("Java detection failed: %1").arg(error)));
            return;
        }
        respond();
    });
}

Tool makeOp(const QString& name, Scope scope, Effect effect, const QString& description, const QJsonObject& input,
            std::function<QString(const QJsonObject&)> describe, ToolHandler handler)
{
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = input;
    tool.scope = scope;
    tool.annotations.readOnly = effect == Effect::Read;
    tool.annotations.destructive = effect == Effect::Destructive;
    tool.describeAction = std::move(describe);
    tool.handler = std::move(handler);
    return tool;
}
}  // namespace

void registerGameTools(ToolRegistry& registry)
{
    using namespace Schema;

    {
        Tool tool;
        tool.name = "launch";
        tool.title = "Start or stop the game";
        tool.description =
            "Starts an instance (action=start, the default) or kills its running game (action=stop). account picks an account by "
            "id or player name (default: the instance's account, else the launcher default). offlineName plays offline under that "
            "name. server joins a server (host[:port]) on start; world opens a single-player world by folder name. Starting returns "
            "once the launch began; follow it with logs source=game.";
        tool.inputSchema = object(
            {
                { "instance", string("Instance id") },
                { "action", string("Start or stop (default start)", { "start", "stop" }) },
                { "account", string("Account id or player name") },
                { "offlineName", string("Play offline under this name") },
                { "server", string("Server to join, host[:port]") },
                { "world", string("World folder to open (worlds.list)") },
                { "demo", boolean("Start the demo") },
            },
            { "instance" });
        tool.outputSchema = object({ { "instance", string({}) }, { "launching", boolean({}) }, { "stopped", boolean({}) } }, { "instance" }, false);
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.annotations.idempotent = false;
        tool.scope = Scope::Launch;
        tool.effect = [](const QJsonObject& arguments) {
            // Killing the game can lose unsaved progress
            return arguments.value("action").toString() == "stop" ? Effect::Destructive : Effect::Write;
        };
        tool.describeAction = [](const QJsonObject& arguments) {
            if (arguments.value("action").toString() == "stop") {
                return QString("Kill the running game of '%1' (unsaved progress is lost)").arg(instanceName(arguments));
            }
            auto text = QString("Launch '%1'").arg(instanceName(arguments));
            if (const auto account = arguments.value("account").toString(); !account.isEmpty()) {
                text += QString(" as %1").arg(account);
            }
            if (const auto server = arguments.value("server").toString(); !server.isEmpty()) {
                text += QString(" and join %1").arg(server);
            }
            return text;
        };
        tool.handler = launchTool;
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "logs";
        tool.title = "Game logs and crash reports";
        tool.description =
            "Reads an instance's logs. source=game: the launcher console of the current or last run (with log levels); latest: the "
            "game's logs/latest.log; crash: the newest crash report; file: any file in logs/ or crash-reports/ (omit `file` to list "
            "them; .gz is decompressed). Returns the last `lines` lines (default 200) after filtering by grep (a case-insensitive "
            "regular expression) and minLevel (game only).";
        tool.inputSchema = object(
            {
                { "instance", string("Instance id") },
                { "source", string("Which log (default game)", { "game", "latest", "crash", "file" }) },
                { "file", string("file: path as listed, e.g. logs/2026-11-07-1.log.gz") },
                { "lines", integer("Lines to return from the end (default 200)", 1, MAX_LOG_LINES) },
                { "grep", string("Only lines matching this regular expression") },
                { "minLevel", string("game: lowest level kept", { "debug", "info", "warning", "error", "fatal" }) },
            },
            { "instance" });
        tool.outputSchema = object({ { "instance", string({}) },
                                     { "source", string({}) },
                                     { "running", boolean({}) },
                                     { "matchedLines", integer({}) },
                                     { "returnedLines", integer({}) },
                                     { "text", string({}) },
                                     { "files", array(string({})) } },
                                   { "instance" }, false);
        tool.scope = Scope::Read;
        tool.handler = logsTool;
        registry.add(std::move(tool));
    }
}

void registerGameOps(ToolRegistry& ops)
{
    using namespace Schema;
    const auto accountArg = string("Account id or player name (accounts.list)");

    ops.add(makeOp("accounts.set_default", Scope::Accounts, Effect::Write, "Makes an account the launcher's default.",
                   object({ { "account", accountArg } }, { "account" }),
                   [](const QJsonObject& a) { return QString("Make %1 the default account").arg(a.value("account").toString()); },
                   accountsSetDefault));

    ops.add(makeOp("accounts.set_for_instance", Scope::Accounts, Effect::Write,
                   "Makes an instance always launch with one account; an empty account returns it to the default account.",
                   object({ { "instance", string("Instance id") }, { "account", accountArg } }, { "instance", "account" }),
                   [](const QJsonObject& a) {
                       return QString("Launch '%1' with account %2").arg(instanceName(a), a.value("account").toString());
                   },
                   accountsSetForInstance));

    ops.add(makeOp("accounts.add_offline", Scope::Accounts, Effect::Write,
                   "Adds an offline account (no sign-in; plays offline and on servers that allow it).",
                   object({ { "name", string("Player name, 3-16 letters, digits or underscores") },
                            { "makeDefault", boolean("Also make it the default (default false)") } },
                          { "name" }),
                   [](const QJsonObject& a) { return QString("Add the offline account %1").arg(a.value("name").toString()); },
                   accountsAddOffline));

    ops.add(makeOp("accounts.remove", Scope::Accounts, Effect::Destructive,
                   "Removes an account from the launcher. A Microsoft account then needs signing in again to be used.",
                   object({ { "account", accountArg } }, { "account" }),
                   [](const QJsonObject& a) { return QString("Remove the account %1").arg(a.value("account").toString()); },
                   accountsRemove));

    ops.add(makeOp("accounts.refresh", Scope::Accounts, Effect::Write, "Refreshes an account's sign-in in the background.",
                   object({ { "account", accountArg } }, { "account" }),
                   [](const QJsonObject& a) { return QString("Refresh the account %1").arg(a.value("account").toString()); },
                   accountsRefresh));

    ops.add(makeOp("accounts.open_manager", Scope::Accounts, Effect::Write,
                   "Opens the launcher's account manager for the user, e.g. to sign in a Microsoft account. Agents never see or "
                   "enter credentials.",
                   object({}), [](const QJsonObject&) { return QString("Open the account manager"); }, accountsOpenManager));

    ops.add(makeOp("java.list_installs", Scope::Read, Effect::Read,
                   "Lists the Java installations the launcher found: version, major version, architecture, path. rescan=true searches "
                   "the system again.",
                   object({ { "rescan", boolean("Search the system again (default false)") } }), {}, javaListInstalls));
}

}  // namespace Mcp
