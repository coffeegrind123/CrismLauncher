// SPDX-License-Identifier: GPL-3.0-only
#include "McpLauncherTools.h"

#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>

#include "Application.h"
#include "BuildConfig.h"
#include "InstanceList.h"
#include "mcp/McpInstanceTools.h"
#include "mcp/McpPlatformTools.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpSchema.h"
#include "mcp/McpSettingsTool.h"
#include "mcp/McpTool.h"
#include "minecraft/Component.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "modplatform/ModIndex.h"

namespace Mcp {

namespace {
constexpr auto MINECRAFT_UID = "net.minecraft";
constexpr int DEFAULT_INSTANCE_LIMIT = 200;
constexpr int MAX_INSTANCE_LIMIT = 1000;

QJsonValue isoOrNull(qint64 msecsSinceEpoch)
{
    if (msecsSinceEpoch <= 0) {
        return QJsonValue::Null;
    }
    return QDateTime::fromMSecsSinceEpoch(msecsSinceEpoch, QTimeZone::UTC).toString(Qt::ISODate);
}

QJsonObject nullableObject()
{
    return QJsonObject{ { "type", QJsonArray{ "object", "null" } } };
}

QJsonObject instanceSchema()
{
    using namespace Schema;
    auto managedPack = object(
        {
            { "platform", string("modrinth, flame (CurseForge), atlauncher, ftb, technic") },
            { "id", string("Project id on that platform") },
            { "name", string({}) },
            { "versionId", string({}) },
            { "versionName", string({}) },
        },
        {}, false);
    managedPack["type"] = QJsonArray{ "object", "null" };

    auto lastLaunch = string("ISO 8601, UTC; null if never launched");
    lastLaunch["type"] = QJsonArray{ "string", "null" };

    return object(
        {
            { "id", string("Instance id; pass it to every tool that takes an instance") },
            { "name", string({}) },
            { "group", string("Empty when ungrouped") },
            { "minecraft", string("Minecraft version") },
            { "loaders", array(object({ { "type", string({}) }, { "uid", string({}) }, { "version", string({}) } }, {}, false)) },
            { "managedPack", managedPack },
            { "running", boolean({}) },
            { "broken", boolean("The instance's components failed to load") },
            { "lastLaunch", lastLaunch },
            { "playTimeSeconds", integer({}) },
        },
        {}, false);
}

void launcherInfo(const CallContext& context, const QJsonObject&, const ToolReply& reply)
{
    QJsonArray running;
    auto* instances = APPLICATION->instances();
    for (int i = 0; i < instances->count(); ++i) {
        if (auto* instance = instances->at(i); instance->isRunning()) {
            running.append(instance->id());
        }
    }

    QJsonValue defaultAccount = QJsonValue::Null;
    if (auto account = APPLICATION->accounts()->defaultAccount()) {
        defaultAccount = QJsonObject{ { "name", account->profileName() }, { "type", account->typeString() } };
    }

    const QJsonObject result{
        { "launcher", QJsonObject{ { "name", BuildConfig.LAUNCHER_DISPLAYNAME },
                                   { "version", BuildConfig.printableVersionString() },
                                   { "dataDir", APPLICATION->dataRoot() } } },
        { "platforms", QJsonObject{ { "modrinth", true },
                                    { "curseforge", APPLICATION->capabilities().testFlag(Application::SupportsFlame) } } },
        { "instanceCount", instances->count() },
        { "runningInstances", running },
        { "defaultAccount", defaultAccount },
        { "caller", QJsonObject{ { "token", context.token.name },
                                 { "scopes", QJsonArray::fromStringList(scopeNames(context.token.scopes)) },
                                 { "confirmPolicy", policyName(context.token.policy) },
                                 { "protocolVersion", context.protocolVersion } } },
    };
    reply(ToolResult::ok(result));
}

void listInstances(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto query = arguments.value("query").toString().trimmed();
    const auto group = arguments.value("group").toString();
    const bool hasGroup = arguments.contains("group");
    const auto runningFilter = arguments.value("running");
    const int limit = arguments.value("limit").toInt(DEFAULT_INSTANCE_LIMIT);

    auto* instances = APPLICATION->instances();
    QJsonArray out;
    int matched = 0;
    for (int i = 0; i < instances->count(); ++i) {
        auto* instance = instances->at(i);
        const auto instanceGroup = instances->getInstanceGroup(instance->id());
        if (!query.isEmpty() && !instance->name().contains(query, Qt::CaseInsensitive) && !instance->id().contains(query, Qt::CaseInsensitive) &&
            !instanceGroup.contains(query, Qt::CaseInsensitive)) {
            continue;
        }
        if (hasGroup && instanceGroup != group) {
            continue;
        }
        if (runningFilter.isBool() && instance->isRunning() != runningFilter.toBool()) {
            continue;
        }
        ++matched;
        if (out.size() < limit) {
            out.append(instanceSummary(instance));
        }
    }

    QJsonObject result{ { "count", matched }, { "instances", out } };
    if (matched > out.size()) {
        result["truncated"] = true;
    }
    QJsonArray groups;
    for (const auto& name : instances->getGroups()) {
        groups.append(name);
    }
    result["groups"] = groups;
    reply(ToolResult::ok(result));
}
}  // namespace

QJsonObject instanceSummary(MinecraftInstance* instance)
{
    auto* profile = instance->getPackProfile();
    QString minecraft = profile->getComponentVersion(MINECRAFT_UID);
    if (minecraft.isEmpty()) {
        // Same lazy load the main window's status bar does: components load on first use
        if (auto res = profile->reload(Net::Mode::Offline); !res) {
            qWarning() << "MCP: failed to load components of" << instance->id() << ":" << res.error();
        }
        minecraft = profile->getComponentVersion(MINECRAFT_UID);
    }

    QJsonArray loaders;
    for (auto it = Component::KNOWN_MODLOADERS.cbegin(); it != Component::KNOWN_MODLOADERS.cend(); ++it) {
        auto component = profile->getComponent(it.key());
        if (!component || !component->isEnabled()) {
            continue;
        }
        loaders.append(QJsonObject{ { "type", ModPlatform::getModLoaderAsString(it.value().type) },
                                    { "uid", it.key() },
                                    { "version", component->getVersion() } });
    }

    QJsonValue managedPack = QJsonValue::Null;
    if (instance->isManagedPack()) {
        managedPack = QJsonObject{
            { "platform", instance->getManagedPackType() },     { "id", instance->getManagedPackID() },
            { "name", instance->getManagedPackName() },         { "versionId", instance->getManagedPackVersionID() },
            { "versionName", instance->getManagedPackVersionName() },
        };
    }

    return QJsonObject{
        { "id", instance->id() },
        { "name", instance->name() },
        { "group", APPLICATION->instances()->getInstanceGroup(instance->id()) },
        { "minecraft", minecraft },
        { "loaders", loaders },
        { "managedPack", managedPack },
        { "running", instance->isRunning() },
        { "broken", instance->hasVersionBroken() },
        { "lastLaunch", isoOrNull(instance->lastLaunch()) },
        { "playTimeSeconds", static_cast<qint64>(instance->totalTimePlayed()) },
    };
}

void registerLauncherTools(ToolRegistry& registry)
{
    using namespace Schema;

    {
        Tool tool;
        tool.name = "launcher_info";
        tool.title = "Launcher overview";
        tool.description =
            "Start here. Returns the launcher version and data folder, which mod platforms are available, how many instances exist and "
            "which are running, the default account's name and type, and what this token may do (its scopes and confirmation policy).";
        tool.inputSchema = object({});
        tool.outputSchema = object(
            {
                { "launcher", object({ { "name", string({}) }, { "version", string({}) }, { "dataDir", string({}) } }, {}, false) },
                { "platforms", object({ { "modrinth", boolean({}) }, { "curseforge", boolean({}) } }, {}, false) },
                { "instanceCount", integer({}) },
                { "runningInstances", array(string({}), "Ids of running instances") },
                { "defaultAccount", nullableObject() },
                { "caller", object({}, {}, false) },
            },
            { "launcher", "platforms", "instanceCount", "runningInstances", "caller" }, false);
        tool.scope = Scope::Read;
        tool.alwaysLoad = true;
        tool.handler = launcherInfo;
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "instances";
        tool.title = "List instances";
        tool.description =
            "Lists Minecraft instances with their id, name, group, Minecraft version, mod loaders, the modpack they were installed "
            "from, whether they are running, and play time. Filter by a name/id/group substring, an exact group, or running state. "
            "Instance ids from here are what every other tool takes.";
        tool.inputSchema = object({
            { "query", string("Case-insensitive substring of the name, id or group") },
            { "group", string("Exact group name; empty string for ungrouped instances") },
            { "running", boolean("Only running (true) or only stopped (false) instances") },
            { "limit", integer("Maximum instances returned (default 200)", 1, MAX_INSTANCE_LIMIT) },
        });
        tool.outputSchema = object(
            {
                { "count", integer("Instances that matched, including any beyond the limit") },
                { "instances", array(instanceSchema()) },
                { "truncated", boolean("More instances matched than were returned") },
                { "groups", array(string({}), "Every group name") },
            },
            { "count", "instances", "groups" }, false);
        tool.scope = Scope::Read;
        tool.alwaysLoad = true;
        tool.handler = listInstances;
        registry.add(std::move(tool));
    }

    registerInstanceTools(registry);
    registerPlatformTools(registry);
    registerSettingsTool(registry);
}

}  // namespace Mcp
