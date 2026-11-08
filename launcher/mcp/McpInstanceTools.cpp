// SPDX-License-Identifier: GPL-3.0-only
#include "McpInstanceTools.h"

#include <QJsonArray>
#include <QHash>
#include <QTimer>

#include <memory>

#include "Application.h"
#include "InstanceList.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "mcp/McpLauncherTools.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "meta/VersionList.h"
#include "minecraft/Component.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/World.h"
#include "minecraft/WorldList.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "minecraft/mod/ResourceFolderModel.h"

namespace Mcp {

namespace {
constexpr int DEFAULT_VERSION_LIMIT = 50;
constexpr int MAX_VERSION_LIMIT = 2000;
constexpr int DEFAULT_WAIT_SECONDS = 30;
constexpr int MAX_WAIT_SECONDS = 120;
constexpr int DEFAULT_CONTENT_LIMIT = 500;

QString severityName(ProblemSeverity severity)
{
    switch (severity) {
        case ProblemSeverity::None:
            return "none";
        case ProblemSeverity::Warning:
            return "warning";
        case ProblemSeverity::Error:
            return "error";
    }
    return {};
}

QJsonArray componentsJson(MinecraftInstance* instance)
{
    auto* profile = instance->getPackProfile();
    QJsonArray out;
    for (int i = 0; i < profile->rowCount(QModelIndex()); ++i) {
        auto component = profile->getComponent(static_cast<size_t>(i));
        if (!component) {
            continue;
        }
        QJsonObject entry{
            { "uid", component->getID() },     { "name", component->getName() },          { "version", component->getVersion() },
            { "enabled", component->isEnabled() }, { "custom", component->isCustom() }, { "removable", component->isRemovable() },
            { "versionChangeable", component->isVersionChangeable() },
        };
        if (const auto problems = component->getProblems(); !problems.isEmpty()) {
            QJsonArray list;
            for (const auto& problem : problems) {
                list.append(QJsonObject{ { "severity", severityName(problem.m_severity) }, { "description", problem.m_description } });
            }
            entry["problems"] = list;
        }
        out.append(entry);
    }
    return out;
}

QJsonObject worldJson(const World& world)
{
    QJsonObject out{
        { "folder", world.folderName() },
        { "name", world.name() },
        { "gameType", world.gameType().toLogString() },
        { "sizeBytes", static_cast<qint64>(world.bytes()) },
    };
    if (world.lastPlayed().isValid()) {
        out["lastPlayed"] = world.lastPlayed().toUTC().toString(Qt::ISODate);
    }
    if (!world.isValid()) {
        out["valid"] = false;
    }
    return out;
}

Result<MinecraftInstance*> instanceArgument(const QJsonObject& arguments)
{
    return findInstance(arguments.value("instance").toString());
}

// --- tools ---

void instanceGet(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = instanceArgument(arguments);
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;

    auto out = instanceSummary(instance);
    out["components"] = componentsJson(instance);
    out["notes"] = instance->notes();
    out["crashedLastRun"] = instance->hasCrashed();
    out["paths"] = QJsonObject{ { "instance", instance->instanceRoot() }, { "game", instance->gameRoot() } };

    auto* worlds = instance->worldList();
    worlds->update();
    out["worldCount"] = static_cast<qint64>(worlds->size());

    // Counting content means scanning each folder; wait for all of them, then reply once
    struct State {
        int pending = 0;
        QJsonObject counts;
        QJsonObject out;
    };
    auto state = std::make_shared<State>();
    state->out = out;
    state->pending = static_cast<int>(contentKindNames().size());
    for (const auto& name : contentKindNames()) {
        auto modelResult = contentModel(instance, *contentKindFromName(name));
        if (!modelResult) {
            state->counts[name] = QJsonObject{ { "available", false }, { "reason", modelResult.error() } };
            if (--state->pending == 0) {
                state->out["content"] = state->counts;
                reply(ToolResult::ok(state->out));
            }
            continue;
        }
        auto* model = *modelResult;
        whenModelReady(model, [state, model, name, reply] {
            int enabled = 0;
            for (int i = 0; i < model->size(); ++i) {
                enabled += model->at(i).enabled() ? 1 : 0;
            }
            state->counts[name] = QJsonObject{ { "total", static_cast<qint64>(model->size()) }, { "enabled", enabled } };
            if (--state->pending == 0) {
                state->out["content"] = state->counts;
                reply(ToolResult::ok(state->out));
            }
        });
    }
}

void jobsTool(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* registry = jobs();
    if (!registry) {
        reply(ToolResult::error("Jobs are not available."));
        return;
    }
    const auto action = arguments.value("action").toString("list");
    const auto id = arguments.value("id").toString();

    if (action == "list") {
        QJsonArray out;
        for (const auto& info : registry->list(context.token.id)) {
            out.append(info.toJson());
        }
        reply(ToolResult::ok(QJsonObject{ { "jobs", out } }));
        return;
    }

    const auto info = registry->get(id);
    if (!info || info->tokenId != context.token.id) {
        reply(ToolResult::error(QString("No job '%1' started by this token.").arg(id), "Use action=list to see this token's jobs."));
        return;
    }

    if (action == "get") {
        reply(ToolResult::ok(QJsonObject{ { "job", info->toJson() } }));
        return;
    }
    if (action == "cancel") {
        const bool cancelled = registry->cancel(id);
        const auto now = registry->get(id);
        reply(ToolResult::ok(QJsonObject{ { "cancelled", cancelled }, { "job", now ? now->toJson() : info->toJson() } }));
        return;
    }

    // wait: reply when the job finishes, or with its current state when the timeout passes
    const int seconds = arguments.value("timeoutSeconds").toInt(DEFAULT_WAIT_SECONDS);
    auto replied = std::make_shared<bool>(false);
    auto finish = [replied, reply](const JobInfo& job, bool timedOut) {
        if (*replied) {
            return;
        }
        *replied = true;
        reply(ToolResult::ok(QJsonObject{ { "job", job.toJson() }, { "timedOut", timedOut } }));
    };
    registry->whenFinished(id, [finish](const JobInfo& job) { finish(job, false); });
    QTimer::singleShot(seconds * 1000, registry, [registry, id, finish] {
        if (const auto job = registry->get(id)) {
            finish(*job, !job->isFinished());
        }
    });
}

// --- ops ---

void groupsList(const CallContext&, const QJsonObject&, const ToolReply& reply)
{
    auto* instances = APPLICATION->instances();
    QHash<QString, int> counts;
    for (int i = 0; i < instances->count(); ++i) {
        counts[instances->getInstanceGroup(instances->at(i)->id())]++;
    }
    QJsonArray out;
    for (const auto& group : instances->getGroups()) {
        out.append(QJsonObject{ { "name", group }, { "instances", counts.value(group) }, { "collapsed", instances->isGroupCollapsed(group) } });
    }
    reply(ToolResult::ok(QJsonObject{ { "groups", out }, { "ungrouped", counts.value(QString()) } }));
}

void componentsList(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = instanceArgument(arguments);
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", (*found)->id() }, { "components", componentsJson(*found) } }));
}

void availableVersions(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto uid = arguments.value("uid").toString();
    const auto minecraft = arguments.value("minecraft").toString();
    const bool includeSnapshots = arguments.value("includeSnapshots").toBool(false);
    const int limit = arguments.value("limit").toInt(DEFAULT_VERSION_LIMIT);

    auto list = APPLICATION->metadataIndex()->get(uid);
    if (!list) {
        reply(ToolResult::error(QString("Unknown component '%1'.").arg(uid),
                                "Common uids: net.minecraft, net.fabricmc.fabric-loader, org.quiltmc.quilt-loader, net.minecraftforge, "
                                "net.neoforged, com.mumfrey.liteloader."));
        return;
    }

    auto respond = [list, uid, minecraft, includeSnapshots, limit, reply] {
        QJsonArray out;
        int matched = 0;
        for (const auto& version : list->versions()) {
            const bool release = version->type().isEmpty() || version->type() == "release";
            if (!includeSnapshots && !release) {
                continue;
            }
            if (!minecraft.isEmpty()) {
                // Loaders that pin a Minecraft version (Forge, NeoForge, LiteLoader) say so in their requirements
                bool pinned = false;
                bool matches = false;
                for (const auto& require : version->requiredSet()) {
                    if (require.uid == "net.minecraft" && !require.equalsVersion.isEmpty()) {
                        pinned = true;
                        matches = require.equalsVersion == minecraft;
                    }
                }
                if (pinned && !matches) {
                    continue;
                }
            }
            ++matched;
            if (out.size() >= limit) {
                continue;
            }
            QJsonObject entry{ { "version", version->version() }, { "type", version->type() } };
            if (version->isRecommended()) {
                entry["recommended"] = true;
            }
            if (version->rawTime() > 0) {
                entry["released"] = version->time().toUTC().toString(Qt::ISODate);
            }
            out.append(entry);
        }
        reply(ToolResult::ok(QJsonObject{ { "uid", uid }, { "name", list->name() }, { "count", matched }, { "versions", out } }));
    };

    if (list->isLoaded()) {
        respond();
        return;
    }
    runTask(list->getLoadTask(), [respond, reply, uid](bool ok, const QString& error) {
        if (!ok) {
            reply(ToolResult::error(QString("Could not load versions of %1: %2").arg(uid, error)));
            return;
        }
        respond();
    });
}

void contentList(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = instanceArgument(arguments);
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    const auto kindName = arguments.value("kind").toString("mods");
    const auto query = arguments.value("query").toString();
    const auto enabledFilter = arguments.value("enabled");
    const int limit = arguments.value("limit").toInt(DEFAULT_CONTENT_LIMIT);
    auto modelResult = contentModel(instance, *contentKindFromName(kindName));
    if (!modelResult) {
        reply(ToolResult::error(modelResult.error()));
        return;
    }
    auto* model = *modelResult;

    whenModelReady(model, [model, instance, kindName, query, enabledFilter, limit, reply] {
        QJsonArray out;
        int matched = 0;
        for (int i = 0; i < model->size(); ++i) {
            const auto& resource = model->at(i);
            if (!query.isEmpty() && !resource.name().contains(query, Qt::CaseInsensitive) &&
                !resource.fileinfo().fileName().contains(query, Qt::CaseInsensitive)) {
                continue;
            }
            if (enabledFilter.isBool() && resource.enabled() != enabledFilter.toBool()) {
                continue;
            }
            ++matched;
            if (out.size() < limit) {
                out.append(resourceJson(resource));
            }
        }
        reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() },
                                          { "kind", kindName },
                                          { "folder", model->dir().absolutePath() },
                                          { "count", matched },
                                          { "items", out } }));
    });
}

void worldsList(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = instanceArgument(arguments);
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* worlds = (*found)->worldList();
    worlds->update();
    QJsonArray out;
    for (const auto& world : worlds->allWorlds()) {
        out.append(worldJson(world));
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", (*found)->id() }, { "worlds", out } }));
}

QString accountStateName(AccountState state)
{
    switch (state) {
        case AccountState::Unchecked:
            return "unchecked";
        case AccountState::Offline:
            return "offline";
        case AccountState::Working:
            return "working";
        case AccountState::Online:
            return "online";
        case AccountState::Disabled:
            return "disabled";
        case AccountState::Errored:
            return "errored";
        case AccountState::Expired:
            return "expired";
        case AccountState::Gone:
            return "gone";
    }
    return {};
}

void accountsList(const CallContext&, const QJsonObject&, const ToolReply& reply)
{
    auto* accounts = APPLICATION->accounts();
    const auto defaultAccount = accounts->defaultAccount();
    QJsonArray out;
    for (int i = 0; i < accounts->count(); ++i) {
        const auto account = accounts->at(i);
        out.append(QJsonObject{
            { "id", account->internalId() },
            { "name", account->profileName() },
            { "type", account->typeString() },
            { "state", accountStateName(account->accountState()) },
            { "default", account == defaultAccount },
            { "inUse", account->isActive() },
        });
    }
    reply(ToolResult::ok(QJsonObject{ { "accounts", out } }));
}

Tool op(const QString& name, const QString& description, const QJsonObject& input, ToolHandler handler)
{
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = input;
    tool.scope = Scope::Read;
    tool.handler = std::move(handler);
    return tool;
}
}  // namespace

void registerInstanceTools(ToolRegistry& registry)
{
    using namespace Schema;

    {
        Tool tool;
        tool.name = "instance_get";
        tool.title = "Instance details";
        tool.description =
            "Everything about one instance: what `instances` returns plus its components (Minecraft, loaders, libraries) with any "
            "problems, notes, whether the last run crashed, its folders, world count, and how many mods, resource packs, texture "
            "packs, shader packs and data packs it has (total and enabled). Use call content.list for the items themselves.";
        tool.inputSchema = object({ { "instance", string("Instance id") } }, { "instance" });
        tool.outputSchema = object({ { "id", string({}) }, { "components", array(object({}, {}, false)) }, { "content", object({}, {}, false) } },
                                   { "id", "components", "content" }, false);
        tool.scope = Scope::Read;
        tool.handler = instanceGet;
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "jobs";
        tool.title = "Long-running jobs";
        tool.description =
            "Installs, downloads and other slow work run as jobs: the tool that starts one returns its id at once. list shows this "
            "token's jobs (newest first), get reads one, wait blocks until it finishes or timeoutSeconds passes (default 30, max 120; "
            "call again to keep waiting), cancel stops it.";
        tool.inputSchema = object(
            {
                { "action", string("What to do (default list)", { "list", "get", "wait", "cancel" }) },
                { "id", string("Job id, for get / wait / cancel") },
                { "timeoutSeconds", integer("wait: seconds before returning the current state", 1, MAX_WAIT_SECONDS) },
            },
            {});
        tool.outputSchema = object({ { "jobs", array(object({}, {}, false)) },
                                     { "job", object({}, {}, false) },
                                     { "timedOut", boolean({}) },
                                     { "cancelled", boolean({}) } },
                                   {}, false);
        tool.scope = Scope::Read;
        tool.handler = jobsTool;
        registry.add(std::move(tool));
    }
}

void registerInstanceOps(ToolRegistry& ops)
{
    using namespace Schema;
    const auto instanceArg = string("Instance id");

    ops.add(op("groups.list", "Lists instance groups with how many instances each holds, and how many instances have no group.", object({}),
               groupsList));

    ops.add(op("components.list",
               "Lists an instance's components in load order: Minecraft, the mod loader, LWJGL, custom jars, with versions and problems.",
               object({ { "instance", instanceArg } }, { "instance" }), componentsList));

    ops.add(op("components.available_versions",
               "Lists versions available for a component uid from the launcher's metadata: net.minecraft, net.fabricmc.fabric-loader, "
               "org.quiltmc.quilt-loader, net.minecraftforge, net.neoforged, com.mumfrey.liteloader, org.lwjgl3. Newest first. With "
               "minecraft set, loaders that pin a Minecraft version are filtered to it.",
               object(
                   {
                       { "uid", string("Component uid") },
                       { "minecraft", string("Only versions for this Minecraft version (where the component pins one)") },
                       { "includeSnapshots", boolean("Include snapshots, betas and other non-release versions (default false)") },
                       { "limit", integer("Maximum versions returned (default 50)", 1, MAX_VERSION_LIMIT) },
                   },
                   { "uid" }),
               availableVersions));

    ops.add(op("content.list",
               "Lists the mods, resource packs, texture packs, shader packs or data packs in an instance: file, name, version, enabled, "
               "and the Modrinth/CurseForge project it came from when known.",
               object(
                   {
                       { "instance", instanceArg },
                       { "kind", string("Which folder (default mods)", contentKindNames()) },
                       { "query", string("Substring of the name or file name") },
                       { "enabled", boolean("Only enabled (true) or disabled (false) items") },
                       { "limit", integer("Maximum items returned (default 500)", 1, 5000) },
                   },
                   { "instance" }),
               contentList));

    ops.add(op("worlds.list", "Lists an instance's single-player worlds: folder, name, game mode, size and when last played.",
               object({ { "instance", instanceArg } }, { "instance" }), worldsList));

    ops.add(op("accounts.list",
               "Lists the launcher's Minecraft accounts: id, player name, type (msa, offline, authlib-injector), state, which is the "
               "default and which is in use. Never includes tokens.",
               object({}), accountsList));
}

}  // namespace Mcp
