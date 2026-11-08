// SPDX-License-Identifier: GPL-3.0-only
#include "McpWriteOps.h"

#include <QJsonArray>

#include "Application.h"
#include "InstanceList.h"
#include "mcp/McpCommon.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/Component.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "minecraft/mod/ResourceFolderModel.h"
#include "settings/SettingsObject.h"

namespace Mcp {

namespace {
constexpr auto MINECRAFT_UID = "net.minecraft";

QString instanceLabel(const QJsonObject& arguments)
{
    const auto id = arguments.value("instance").toString();
    if (auto* instance = APPLICATION->instances()->getInstanceById(id)) {
        return QString("'%1'").arg(instance->name());
    }
    return QString("'%1'").arg(id);
}

// Most ops start the same way: find the instance; some also refuse while the game runs
MinecraftInstance* requireInstance(const QJsonObject& arguments, const ToolReply& reply, bool mustBeStopped)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return nullptr;
    }
    if (mustBeStopped && (*found)->isRunning()) {
        reply(ToolResult::error(QString("Instance '%1' is running; changing it now could crash the game.").arg((*found)->name()),
                                "Stop it first (launch op stop) or wait until it exits."));
        return nullptr;
    }
    return *found;
}

Tool writeOp(const QString& name,
             Scope scope,
             Effect effect,
             const QString& description,
             const QJsonObject& input,
             std::function<QString(const QJsonObject&)> describe,
             ToolHandler handler)
{
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = input;
    tool.scope = scope;
    tool.annotations.readOnly = false;
    tool.annotations.destructive = effect == Effect::Destructive;
    tool.annotations.idempotent = effect != Effect::Destructive;
    tool.describeAction = std::move(describe);
    tool.handler = std::move(handler);
    return tool;
}

// --- instances and groups ---

void instanceRename(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = requireInstance(arguments, reply, false);
    if (!instance) {
        return;
    }
    const auto name = arguments.value("name").toString().trimmed();
    if (name.isEmpty()) {
        reply(ToolResult::error("The new name is empty."));
        return;
    }
    const auto old = instance->name();
    instance->setName(name);
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "old", old }, { "name", instance->name() } }));
}

void instanceSetGroup(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = requireInstance(arguments, reply, false);
    if (!instance) {
        return;
    }
    const auto group = arguments.value("group").toString().trimmed();
    APPLICATION->instances()->setInstanceGroup(instance->id(), group);
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "group", APPLICATION->instances()->getInstanceGroup(instance->id()) } }));
}

void instanceSetNotes(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = requireInstance(arguments, reply, false);
    if (!instance) {
        return;
    }
    instance->setNotes(arguments.value("notes").toString());
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "notes", instance->notes() } }));
}

void instanceDelete(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = requireInstance(arguments, reply, true);
    if (!instance) {
        return;
    }
    const auto id = instance->id();
    const auto name = instance->name();
    auto* instances = APPLICATION->instances();
    if (instances->trashInstance(id)) {
        reply(ToolResult::ok(QJsonObject{ { "instance", id }, { "name", name }, { "trashed", true } },
                             QString("Moved '%1' to the trash; instance.undo_delete restores it.").arg(name)));
        return;
    }
    if (!arguments.value("permanent").toBool(false)) {
        reply(ToolResult::error(QString("Could not move '%1' to the trash (none on this system, or it failed).").arg(name),
                                "Pass permanent=true to delete it for good; that cannot be undone."));
        return;
    }
    instances->deleteInstance(id);
    reply(ToolResult::ok(QJsonObject{ { "instance", id }, { "name", name }, { "trashed", false } }));
}

void instanceUndoDelete(const CallContext&, const QJsonObject&, const ToolReply& reply)
{
    auto* instances = APPLICATION->instances();
    if (!instances->trashedSomething()) {
        reply(ToolResult::error("Nothing deleted in this session can be restored."));
        return;
    }
    if (!instances->undoTrashInstance()) {
        reply(ToolResult::error("Restoring the most recently deleted instance failed."));
        return;
    }
    reply(ToolResult::ok(QJsonObject{ { "restored", true } }, "Restored the most recently deleted instance; call instances to see it."));
}

void groupsRename(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instances = APPLICATION->instances();
    const auto from = arguments.value("from").toString();
    const auto to = arguments.value("to").toString().trimmed();
    if (!instances->getGroups().contains(from)) {
        reply(ToolResult::error(QString("No group '%1'.").arg(from), "Call groups.list for the names."));
        return;
    }
    if (to.isEmpty()) {
        reply(ToolResult::error("The new group name is empty."));
        return;
    }
    instances->renameGroup(from, to);
    reply(ToolResult::ok(QJsonObject{ { "from", from }, { "to", to } }));
}

void groupsDelete(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instances = APPLICATION->instances();
    const auto name = arguments.value("name").toString();
    if (!instances->getGroups().contains(name)) {
        reply(ToolResult::error(QString("No group '%1'.").arg(name), "Call groups.list for the names."));
        return;
    }
    instances->deleteGroup(name);
    reply(ToolResult::ok(QJsonObject{ { "deleted", name } }, "The group is gone; its instances are now ungrouped, not deleted."));
}

// --- components ---

void componentsSetVersion(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = requireInstance(arguments, reply, true);
    if (!instance) {
        return;
    }
    auto* profile = instance->getPackProfile();
    const auto uid = arguments.value("uid").toString();
    const auto version = arguments.value("version").toString();
    const auto onConflict = arguments.value("conflicts").toString("error");

    // The same conflict handling InstallLoaderDialog offers the user, chosen up front
    QJsonArray handled;
    if (auto known = Component::KNOWN_MODLOADERS.find(uid); known != Component::KNOWN_MODLOADERS.cend()) {
        QList<ComponentPtr> conflicts;
        for (const auto& conflictId : known->knownConflictingComponents) {
            const auto conflict = profile->getComponent(conflictId);
            if (conflict && conflict->isEnabled() && !conflict->isCustom()) {
                conflicts.append(conflict);
            }
        }
        if (!conflicts.isEmpty() && onConflict == "error") {
            QStringList names;
            for (const auto& conflict : conflicts) {
                names.append(QString("%1 (%2)").arg(conflict->getName(), conflict->getID()));
            }
            reply(ToolResult::error(QString("%1 conflicts with %2 on this instance. Nothing was changed.").arg(uid, names.join(", ")),
                                    "Call again with conflicts=disable or conflicts=remove (or keep, which will likely break it)."));
            return;
        }
        for (const auto& conflict : conflicts) {
            if (onConflict == "disable" && conflict->canBeDisabled()) {
                conflict->setEnabled(false);
                handled.append(QJsonObject{ { "uid", conflict->getID() }, { "action", "disabled" } });
            } else if (onConflict == "remove" && conflict->isRemovable()) {
                profile->remove(conflict->getID());
                handled.append(QJsonObject{ { "uid", conflict->getID() }, { "action", "removed" } });
            } else {
                handled.append(QJsonObject{ { "uid", conflict->getID() }, { "action", "kept" } });
            }
        }
    }

    const auto old = profile->getComponentVersion(uid);
    const bool important = uid == MINECRAFT_UID;
    if (important) {
        // Same follow-ups as the Version page: let automatic Java pick a matching runtime again
        auto* settings = instance->settings();
        if (APPLICATION->settings()->get("AutomaticJavaSwitch").toBool() && settings->get("AutomaticJava").toBool() &&
            settings->get("OverrideJavaLocation").toBool()) {
            settings->set("OverrideJavaLocation", false);
            settings->set("JavaPath", "");
        }
        if (settings->get("UseLatestMinecraftVersion").toBool()) {
            settings->set("UseLatestMinecraftVersion", false);
        }
    }
    if (const auto component = profile->getComponent(uid); component && !component->isEnabled()) {
        component->setEnabled(true);
    }
    if (!profile->setComponentVersion(uid, version, important)) {
        reply(ToolResult::error(QString("Could not change %1 (the component refused the change).").arg(uid)));
        return;
    }
    profile->resolve(Net::Mode::Online);

    QJsonObject result{ { "instance", instance->id() }, { "uid", uid }, { "old", old }, { "version", version }, { "conflicts", handled } };
    reply(ToolResult::ok(result, QString("%1 set to %2 (was %3). The launcher is now resolving dependencies and downloading in the "
                                         "background; components.list shows any problem.")
                                     .arg(uid, version, old.isEmpty() ? QString("not installed") : old)));
}

void componentsRemove(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = requireInstance(arguments, reply, true);
    if (!instance) {
        return;
    }
    auto* profile = instance->getPackProfile();
    const auto uid = arguments.value("uid").toString();
    const auto component = profile->getComponent(uid);
    if (!component) {
        reply(ToolResult::error(QString("Instance has no component '%1'.").arg(uid), "Call components.list for its components."));
        return;
    }
    if (!component->isRemovable()) {
        reply(ToolResult::error(QString("%1 cannot be removed (it is required).").arg(uid)));
        return;
    }
    if (!profile->remove(uid)) {
        reply(ToolResult::error(QString("Removing %1 failed.").arg(uid)));
        return;
    }
    profile->resolve(Net::Mode::Online);
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "removed", uid } }));
}

// --- content ---

// Resolves file names to model rows; unknown names are reported, not skipped
Result<QModelIndexList> rowsFor(ResourceFolderModel* model, const QJsonArray& files)
{
    QModelIndexList rows;
    QStringList missing;
    QStringList available;
    for (int i = 0; i < model->size(); ++i) {
        available.append(model->at(i).fileinfo().fileName());
    }
    for (const auto& file : files) {
        const auto name = file.toString();
        const auto row = available.indexOf(name);
        if (row < 0) {
            missing.append(name);
            continue;
        }
        rows.append(model->index(static_cast<int>(row), 0));
    }
    if (!missing.isEmpty()) {
        QStringList hints;
        for (const auto& name : missing) {
            if (const auto close = closestMatches(name, available); !close.isEmpty()) {
                hints.append(QString("%1 -> %2").arg(name, close.first()));
            }
        }
        auto message = QString("No such file(s): %1. Nothing was changed.").arg(missing.join(", "));
        if (!hints.isEmpty()) {
            message += " Did you mean: " + hints.join("; ") + "?";
        }
        return std::unexpected(message + " content.list gives the exact file names.");
    }
    return rows;
}

void contentToggle(const QJsonObject& arguments, const ToolReply& reply, EnableAction action)
{
    auto* instance = requireInstance(arguments, reply, true);
    if (!instance) {
        return;
    }
    const auto kindName = arguments.value("kind").toString("mods");
    const auto kind = *contentKindFromName(kindName);
    auto modelResult = contentModel(instance, kind);
    if (!modelResult) {
        reply(ToolResult::error(modelResult.error()));
        return;
    }
    auto* model = *modelResult;
    const bool withDependencies = arguments.value("withDependencies").toBool(true);
    const auto files = arguments.value("files").toArray();

    whenModelReady(model, [model, kind, withDependencies, files, reply, action, instance] {
        auto rows = rowsFor(model, files);
        if (!rows) {
            reply(ToolResult::error(rows.error()));
            return;
        }
        auto targets = *rows;
        // Mods that require (or are required by) the targets follow them, as the Mods page offers to do
        if (kind == ContentKind::Mods && withDependencies) {
            auto* mods = static_cast<ModFolderModel*>(model);
            for (const auto& row : mods->getAffectedMods(*rows, action)) {
                if (!targets.contains(row)) {
                    targets.append(row);
                }
            }
        }
        QJsonArray changed;
        for (const auto& row : targets) {
            changed.append(model->at(row.row()).fileinfo().fileName());
        }
        // The base implementation: the Mods page's override would open a dialog about dependencies, decided above instead
        if (!model->ResourceFolderModel::setResourceEnabled(targets, action)) {
            reply(ToolResult::error("The launcher could not change some of the files (see its log)."));
            return;
        }
        reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "enabled", action == EnableAction::ENABLE }, { "files", changed } }));
    });
}

void contentRemove(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = requireInstance(arguments, reply, true);
    if (!instance) {
        return;
    }
    auto modelResult = contentModel(instance, *contentKindFromName(arguments.value("kind").toString("mods")));
    if (!modelResult) {
        reply(ToolResult::error(modelResult.error()));
        return;
    }
    auto* model = *modelResult;
    const auto files = arguments.value("files").toArray();
    whenModelReady(model, [model, files, reply, instance] {
        auto rows = rowsFor(model, files);
        if (!rows) {
            reply(ToolResult::error(rows.error()));
            return;
        }
        if (!model->deleteResources(*rows)) {
            reply(ToolResult::error("The launcher could not remove some of the files (see its log)."));
            return;
        }
        reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "removed", files } },
                             "Moved to the system trash where there is one."));
    });
}

QString filesLabel(const QJsonObject& arguments)
{
    QStringList names;
    for (const auto& file : arguments.value("files").toArray()) {
        names.append(file.toString());
    }
    return names.join(", ");
}
}  // namespace

void registerWriteOps(ToolRegistry& ops)
{
    using namespace Schema;
    const auto instanceArg = string("Instance id");
    auto filesArg = array(string({}), "Exact file names, as content.list returns them");
    filesArg["minItems"] = 1;

    ops.add(writeOp("instance.rename", Scope::Instances, Effect::Write, "Renames an instance (its id and folder stay the same).",
                    object({ { "instance", instanceArg }, { "name", string("New name") } }, { "instance", "name" }),
                    [](const QJsonObject& a) { return QString("Rename instance %1 to '%2'").arg(instanceLabel(a), a.value("name").toString()); },
                    instanceRename));

    ops.add(writeOp("instance.set_group", Scope::Instances, Effect::Write,
                    "Moves an instance into a group, creating the group if needed; an empty group removes it from its group.",
                    object({ { "instance", instanceArg }, { "group", string("Group name; empty for none") } }, { "instance", "group" }),
                    [](const QJsonObject& a) { return QString("Move instance %1 to group '%2'").arg(instanceLabel(a), a.value("group").toString()); },
                    instanceSetGroup));

    ops.add(writeOp("instance.set_notes", Scope::Instances, Effect::Write, "Replaces an instance's notes.",
                    object({ { "instance", instanceArg }, { "notes", string("The new notes (plain text)") } }, { "instance", "notes" }),
                    [](const QJsonObject& a) { return QString("Replace the notes of instance %1").arg(instanceLabel(a)); }, instanceSetNotes));

    ops.add(writeOp("instance.delete", Scope::Instances, Effect::Destructive,
                    "Deletes an instance by moving its folder to the system trash; instance.undo_delete brings back the most recent one "
                    "while the launcher stays open. Refused while it runs. If there is no trash, permanent=true deletes it for good.",
                    object({ { "instance", instanceArg }, { "permanent", boolean("Delete permanently if trashing fails (default false)") } },
                           { "instance" }),
                    [](const QJsonObject& a) { return QString("Delete instance %1 and its worlds").arg(instanceLabel(a)); }, instanceDelete));

    ops.add(writeOp("instance.undo_delete", Scope::Instances, Effect::Write,
                    "Restores the most recently trashed instance, if one was deleted since the launcher started.", object({}),
                    [](const QJsonObject&) { return QString("Restore the most recently deleted instance"); }, instanceUndoDelete));

    ops.add(writeOp("groups.rename", Scope::Instances, Effect::Write, "Renames an instance group.",
                    object({ { "from", string("Current name") }, { "to", string("New name") } }, { "from", "to" }),
                    [](const QJsonObject& a) { return QString("Rename group '%1' to '%2'").arg(a.value("from").toString(), a.value("to").toString()); },
                    groupsRename));

    ops.add(writeOp("groups.delete", Scope::Instances, Effect::Write, "Deletes a group. Its instances are kept and become ungrouped.",
                    object({ { "name", string("Group name") } }, { "name" }),
                    [](const QJsonObject& a) { return QString("Delete group '%1' (its instances stay)").arg(a.value("name").toString()); },
                    groupsDelete));

    ops.add(writeOp(
        "components.set_version", Scope::Instances, Effect::Write,
        "Sets a component's version, adding the component if the instance lacks it: change Minecraft (net.minecraft), install or "
        "change a mod loader (net.fabricmc.fabric-loader, org.quiltmc.quilt-loader, net.minecraftforge, net.neoforged, "
        "com.mumfrey.liteloader), or LWJGL. Versions come from components.available_versions. A loader that conflicts with one "
        "already enabled is refused unless conflicts says what to do with the old one. Refused while the instance runs. Dependencies "
        "then resolve and download in the background.",
        object(
            {
                { "instance", instanceArg },
                { "uid", string("Component uid") },
                { "version", string("Exact version string") },
                { "conflicts", string("What to do with a conflicting loader (default error)", { "error", "disable", "remove", "keep" }) },
            },
            { "instance", "uid", "version" }),
        [](const QJsonObject& a) {
            return QString("Set %1 to version %2 on instance %3").arg(a.value("uid").toString(), a.value("version").toString(), instanceLabel(a));
        },
        componentsSetVersion));

    auto removeComponent =
        writeOp("components.remove", Scope::Instances, Effect::Destructive,
                "Removes a component (a mod loader, a custom jar) from an instance. Minecraft itself cannot be removed. Refused while "
                "the instance runs.",
                object({ { "instance", instanceArg }, { "uid", string("Component uid") } }, { "instance", "uid" }),
                [](const QJsonObject& a) { return QString("Remove %1 from instance %2").arg(a.value("uid").toString(), instanceLabel(a)); },
                componentsRemove);
    ops.add(removeComponent);

    const auto toggleInput = object(
        {
            { "instance", instanceArg },
            { "kind", string("Which folder (default mods)", contentKindNames()) },
            { "files", filesArg },
            { "withDependencies", boolean("Mods only: also enable the mods they require (or disable the mods requiring them); default true") },
        },
        { "instance", "files" });

    ops.add(writeOp("content.enable", Scope::Content, Effect::Write,
                    "Enables mods or packs in an instance by file name. Refused while the instance runs.", toggleInput,
                    [](const QJsonObject& a) { return QString("Enable in instance %1: %2").arg(instanceLabel(a), filesLabel(a)); },
                    [](const CallContext&, const QJsonObject& a, const ToolReply& reply) { contentToggle(a, reply, EnableAction::ENABLE); }));

    ops.add(writeOp("content.disable", Scope::Content, Effect::Write,
                    "Disables mods or packs in an instance by file name (they stay installed). Refused while the instance runs.",
                    toggleInput, [](const QJsonObject& a) { return QString("Disable in instance %1: %2").arg(instanceLabel(a), filesLabel(a)); },
                    [](const CallContext&, const QJsonObject& a, const ToolReply& reply) { contentToggle(a, reply, EnableAction::DISABLE); }));

    ops.add(writeOp("content.remove", Scope::Content, Effect::Destructive,
                    "Removes mods or packs from an instance by file name, moving them to the system trash where there is one. Refused "
                    "while the instance runs.",
                    object({ { "instance", instanceArg }, { "kind", string("Which folder (default mods)", contentKindNames()) }, { "files", filesArg } },
                           { "instance", "files" }),
                    [](const QJsonObject& a) { return QString("Remove from instance %1: %2").arg(instanceLabel(a), filesLabel(a)); },
                    contentRemove));
}

}  // namespace Mcp
