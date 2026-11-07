// SPDX-License-Identifier: GPL-3.0-only
#include "McpWorldOps.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>

#include <tag_list.h>

#include "Application.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/ServersDat.h"
#include "minecraft/World.h"
#include "minecraft/WorldList.h"

namespace Mcp {

namespace {
constexpr auto SERVERS_FILE = "servers.dat";

QString instanceLabel(const QJsonObject& arguments)
{
    const auto id = arguments.value("instance").toString();
    auto* instance = APPLICATION->instances()->getInstanceById(id);
    return instance ? instance->name() : id;
}

MinecraftInstance* instanceFrom(const QJsonObject& arguments, const ToolReply& reply, bool mustBeStopped)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return nullptr;
    }
    if (mustBeStopped && (*found)->isRunning()) {
        reply(ToolResult::error(QString("'%1' is running; the game may be using its worlds.").arg((*found)->name()), "Stop it first."));
        return nullptr;
    }
    return *found;
}

// The row of a world by folder name, after rescanning the saves folder
int worldRow(WorldList* worlds, const QString& folder)
{
    worlds->update();
    const auto& all = worlds->allWorlds();
    for (int i = 0; i < all.size(); ++i) {
        if (all[i].folderName() == folder) {
            return i;
        }
    }
    return -1;
}

void noSuchWorld(const QString& folder, const ToolReply& reply)
{
    reply(ToolResult::error(QString("No world folder '%1'.").arg(folder), "worlds.list gives the folder names."));
}

void startWorldJob(const CallContext& context, std::unique_ptr<Task> task, const QString& title, const ToolReply& reply)
{
    auto* registry = jobs();
    if (!registry || !task) {
        reply(ToolResult::error("The launcher could not start that."));
        return;
    }
    const auto id = registry->start(context.token.id, context.token.name, title, Task::Ptr(task.release()));
    reply(ToolResult::ok(QJsonObject{ { "job", id } }, QString("Started job %1; follow it with jobs action=wait.").arg(id)));
}

// --- worlds ---

void worldsRename(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply, true);
    if (!instance) {
        return;
    }
    auto* worlds = instance->worldList();
    const auto folder = arguments.value("world").toString();
    const int row = worldRow(worlds, folder);
    if (row < 0) {
        noSuchWorld(folder, reply);
        return;
    }
    World world = worlds->allWorlds().at(row);
    const auto name = arguments.value("name").toString().trimmed();
    if (name.isEmpty() || !world.rename(name)) {
        reply(ToolResult::error(QString("Could not rename '%1'.").arg(folder)));
        return;
    }
    worlds->update();
    reply(ToolResult::ok(QJsonObject{ { "world", folder }, { "name", name } }));
}

void worldsCopy(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply, bool backup)
{
    auto* instance = instanceFrom(arguments, reply, false);
    if (!instance) {
        return;
    }
    auto* worlds = instance->worldList();
    const auto folder = arguments.value("world").toString();
    const int row = worldRow(worlds, folder);
    if (row < 0) {
        noSuchWorld(folder, reply);
        return;
    }
    const auto source = worlds->allWorlds().at(row).name();
    auto name = arguments.value("name").toString().trimmed();
    if (name.isEmpty()) {
        name = backup ? QString("%1 backup %2").arg(source, QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm"))
                      : QString("%1 copy").arg(source);
    }
    startWorldJob(context, worlds->createCopyWorldTask(row, name), QString("Copy world '%1' to '%2'").arg(source, name), reply);
}

void worldsDelete(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply, true);
    if (!instance) {
        return;
    }
    auto* worlds = instance->worldList();
    const auto folder = arguments.value("world").toString();
    const int row = worldRow(worlds, folder);
    if (row < 0) {
        noSuchWorld(folder, reply);
        return;
    }
    const auto path = QDir(instance->worldDir()).filePath(folder);
    if (FS::trash(path)) {
        worlds->update();
        reply(ToolResult::ok(QJsonObject{ { "world", folder }, { "trashed", true } }));
        return;
    }
    if (!arguments.value("permanent").toBool(false)) {
        reply(ToolResult::error("Moving the world to the trash failed (or there is none).",
                                "Pass permanent=true to delete it for good, perhaps after worlds.backup."));
        return;
    }
    startWorldJob(context, worlds->createDeleteWorldTask(row), QString("Delete world '%1'").arg(folder), reply);
}

// --- servers ---

QString serversPath(MinecraftInstance* instance)
{
    return FS::PathCombine(instance->gameRoot(), SERVERS_FILE);
}

QList<::Server> readServers(MinecraftInstance* instance)
{
    QList<::Server> out;
    auto root = ServersDat::parseServersDat(serversPath(instance));
    if (!root || !root->has_key("servers", nbt::tag_type::List)) {
        return out;
    }
    for (auto& entry : root->at("servers").as<nbt::tag_list>()) {
        out.append(::Server(entry.as<nbt::tag_compound>()));
    }
    return out;
}

bool writeServers(MinecraftInstance* instance, QList<::Server>& servers)
{
    nbt::tag_compound out;
    nbt::tag_list list;
    for (auto& server : servers) {
        nbt::tag_compound entry;
        server.serialize(entry);
        list.push_back(std::move(entry));
    }
    out.insert("servers", nbt::value(std::move(list)));
    return ServersDat::serializeServerDat(serversPath(instance), &out);
}

QString texturesName(::Server::AcceptsTextures value)
{
    switch (value) {
        case ::Server::AcceptsTextures::ALWAYS:
            return "always";
        case ::Server::AcceptsTextures::NEVER:
            return "never";
        case ::Server::AcceptsTextures::ASK:
            break;
    }
    return "ask";
}

Server::AcceptsTextures texturesFrom(const QString& name)
{
    if (name == "always") {
        return ::Server::AcceptsTextures::ALWAYS;
    }
    if (name == "never") {
        return ::Server::AcceptsTextures::NEVER;
    }
    return ::Server::AcceptsTextures::ASK;
}

QJsonArray serversJson(const QList<::Server>& servers)
{
    QJsonArray out;
    for (int i = 0; i < servers.size(); ++i) {
        out.append(QJsonObject{ { "index", i },
                                { "name", servers[i].m_name },
                                { "address", servers[i].m_address },
                                { "resourcePacks", texturesName(servers[i].m_acceptsTextures) } });
    }
    return out;
}

void serversList(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply, false);
    if (!instance) {
        return;
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "servers", serversJson(readServers(instance)) } }));
}

void serversChange(const QJsonObject& arguments, const ToolReply& reply, const QString& action)
{
    auto* instance = instanceFrom(arguments, reply, false);
    if (!instance) {
        return;
    }
    auto servers = readServers(instance);
    const int index = arguments.value("index").toInt(-1);
    if (action != "add" && (index < 0 || index >= servers.size())) {
        reply(ToolResult::error(QString("No server at index %1.").arg(index), "servers.list gives the indexes."));
        return;
    }

    if (action == "add") {
        ::Server server(arguments.value("name").toString("Minecraft Server"), arguments.value("address").toString());
        server.m_acceptsTextures = texturesFrom(arguments.value("resourcePacks").toString("ask"));
        servers.append(server);
    } else if (action == "edit") {
        auto& server = servers[index];
        if (arguments.contains("name")) {
            server.m_name = arguments.value("name").toString();
        }
        if (arguments.contains("address")) {
            server.m_address = arguments.value("address").toString();
        }
        if (arguments.contains("resourcePacks")) {
            server.m_acceptsTextures = texturesFrom(arguments.value("resourcePacks").toString());
        }
    } else {
        servers.removeAt(index);
    }

    if (!writeServers(instance, servers)) {
        reply(ToolResult::error("Could not write servers.dat."));
        return;
    }
    auto result = ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "servers", serversJson(servers) } });
    if (instance->isRunning()) {
        result.text = "Saved. The game is running and rewrites servers.dat when its server list changes, which can undo this.";
    }
    reply(result);
}

Tool worldOp(const QString& name, Scope scope, Effect effect, const QString& description, const QJsonObject& input,
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

void registerWorldOps(ToolRegistry& ops)
{
    using namespace Schema;
    const auto instanceArg = string("Instance id");
    const auto worldArg = string("World folder name (worlds.list)");
    const auto texturesArg = string("::Server resource packs", { "ask", "always", "never" });

    ops.add(worldOp("worlds.rename", Scope::Content, Effect::Write, "Changes a world's display name (its folder stays). Refused while the game runs.",
                    object({ { "instance", instanceArg }, { "world", worldArg }, { "name", string("New name") } }, { "instance", "world", "name" }),
                    [](const QJsonObject& a) {
                        return QString("Rename world %1 of '%2' to '%3'").arg(a.value("world").toString(), instanceLabel(a), a.value("name").toString());
                    },
                    worldsRename));

    ops.add(worldOp("worlds.copy", Scope::Content, Effect::Write, "Copies a world within the instance, as a job.",
                    object({ { "instance", instanceArg }, { "world", worldArg }, { "name", string("Name of the copy (default '<name> copy')") } },
                           { "instance", "world" }),
                    [](const QJsonObject& a) { return QString("Copy world %1 of '%2'").arg(a.value("world").toString(), instanceLabel(a)); },
                    [](const CallContext& c, const QJsonObject& a, const ToolReply& r) { worldsCopy(c, a, r, false); }));

    ops.add(worldOp("worlds.backup", Scope::Content, Effect::Write,
                    "Makes a dated copy of a world ('<name> backup <date time>') next to it, as a job. Do this before risky changes.",
                    object({ { "instance", instanceArg }, { "world", worldArg } }, { "instance", "world" }),
                    [](const QJsonObject& a) { return QString("Back up world %1 of '%2'").arg(a.value("world").toString(), instanceLabel(a)); },
                    [](const CallContext& c, const QJsonObject& a, const ToolReply& r) { worldsCopy(c, a, r, true); }));

    ops.add(worldOp("worlds.delete", Scope::Content, Effect::Destructive,
                    "Deletes a world by moving it to the system trash; permanent=true deletes for good when there is no trash. Refused "
                    "while the game runs.",
                    object({ { "instance", instanceArg }, { "world", worldArg }, { "permanent", boolean("Delete for good if trashing fails") } },
                           { "instance", "world" }),
                    [](const QJsonObject& a) { return QString("Delete world %1 of '%2'").arg(a.value("world").toString(), instanceLabel(a)); },
                    worldsDelete));

    ops.add(worldOp("servers.list", Scope::Read, Effect::Read, "Lists the multiplayer servers saved in an instance, in order.",
                    object({ { "instance", instanceArg } }, { "instance" }), {}, serversList));

    ops.add(worldOp("servers.add", Scope::Content, Effect::Write, "Adds a server to an instance's multiplayer list.",
                    object({ { "instance", instanceArg },
                             { "name", string("Display name") },
                             { "address", string("host[:port]") },
                             { "resourcePacks", texturesArg } },
                           { "instance", "address" }),
                    [](const QJsonObject& a) { return QString("Add server %1 to '%2'").arg(a.value("address").toString(), instanceLabel(a)); },
                    [](const CallContext&, const QJsonObject& a, const ToolReply& r) { serversChange(a, r, "add"); }));

    ops.add(worldOp("servers.edit", Scope::Content, Effect::Write, "Changes a saved server's name, address or resource-pack setting.",
                    object({ { "instance", instanceArg },
                             { "index", integer("::Server index (servers.list)", 0, std::nullopt) },
                             { "name", string("Display name") },
                             { "address", string("host[:port]") },
                             { "resourcePacks", texturesArg } },
                           { "instance", "index" }),
                    [](const QJsonObject& a) { return QString("Edit server #%1 of '%2'").arg(a.value("index").toInt()).arg(instanceLabel(a)); },
                    [](const CallContext&, const QJsonObject& a, const ToolReply& r) { serversChange(a, r, "edit"); }));

    ops.add(worldOp("servers.remove", Scope::Content, Effect::Destructive, "Removes a server from an instance's multiplayer list.",
                    object({ { "instance", instanceArg }, { "index", integer("::Server index (servers.list)", 0, std::nullopt) } }, { "instance", "index" }),
                    [](const QJsonObject& a) { return QString("Remove server #%1 of '%2'").arg(a.value("index").toInt()).arg(instanceLabel(a)); },
                    [](const CallContext&, const QJsonObject& a, const ToolReply& r) { serversChange(a, r, "remove"); }));
}

}  // namespace Mcp
