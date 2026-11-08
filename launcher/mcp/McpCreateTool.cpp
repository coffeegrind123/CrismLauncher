// SPDX-License-Identifier: GPL-3.0-only
#include "McpCreateTool.h"

#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QUrl>

#include <memory>
#include <utility>

#include "Application.h"
#include "InstanceImportTask.h"
#include "InstanceList.h"
#include "Version.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "mcp/McpLauncherTools.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "meta/VersionList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "modplatform/ModIndex.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"

namespace Mcp {

namespace {
constexpr auto MINECRAFT_UID = "net.minecraft";
constexpr auto DEFAULT_ICON = "default";

const QHash<QString, QString>& loaderUids()
{
    static const QHash<QString, QString> s_uids{
        { "fabric", "net.fabricmc.fabric-loader" }, { "quilt", "org.quiltmc.quilt-loader" }, { "forge", "net.minecraftforge" },
        { "neoforge", "net.neoforged" },            { "liteloader", "com.mumfrey.liteloader" },
    };
    return s_uids;
}

struct Options {
    QString name;
    QString group;
    QString icon;
    InstanceTask::UnattendedChoices choices;
};

// Loads a metadata version list if needed, then calls `ready`
void withVersionList(const QString& uid, const ToolReply& reply, std::function<void(const Meta::VersionList::Ptr&)> ready)
{
    auto list = APPLICATION->metadataIndex()->get(uid);
    if (!list) {
        reply(ToolResult::error(QString("No metadata for %1.").arg(uid)));
        return;
    }
    if (list->isLoaded()) {
        ready(list);
        return;
    }
    runTask(list->getLoadTask(), [list, uid, reply, ready](bool ok, const QString& error) {
        if (!ok) {
            reply(ToolResult::error(QString("Could not load the versions of %1: %2").arg(uid, error)));
            return;
        }
        ready(list);
    });
}

// Wraps the creation task in the instance list's staging/commit step and runs it as a job
void startCreation(const CallContext& context, InstanceTask* task, const Options& options, const QString& title, const ToolReply& reply)
{
    auto* registry = jobs();
    if (!registry) {
        delete task;
        reply(ToolResult::error("Jobs are not available."));
        return;
    }
    if (!options.name.isEmpty()) {
        task->setName(options.name);
    }
    task->setGroup(options.group);
    task->setIcon(options.icon.isEmpty() ? QString(DEFAULT_ICON) : options.icon);
    task->setUnattended(options.choices);

    Task::Ptr wrapped(APPLICATION->instances()->wrapInstanceTask(task));

    // The list announces the committed instance's id; the first announcement after starting is ours
    auto newId = std::make_shared<QString>();
    QObject::connect(APPLICATION->instances(), &InstanceList::instanceSelectRequest, wrapped.get(), [newId](const QString& id) {
        if (newId->isEmpty()) {
            *newId = id;
        }
    });

    const auto jobId = registry->start(context.token.id, context.token.name, title, wrapped, [newId] {
        if (auto* instance = APPLICATION->instances()->getInstanceById(*newId)) {
            return QJsonObject{ { "instance", instanceSummary(instance) } };
        }
        return QJsonObject{ { "instanceId", *newId } };
    });
    auto result = ToolResult::ok(QJsonObject{ { "job", jobId } },
                                 QString("Creating the instance as job %1; follow it with jobs action=wait. Its result holds the new instance.").arg(jobId));
    result.jobId = jobId;
    reply(result);
}

void createVanilla(const CallContext& context, const QJsonObject& arguments, Options options, const ToolReply& reply)
{
    const auto minecraft = arguments.value("minecraft").toString();
    if (minecraft.isEmpty()) {
        reply(ToolResult::error("source=vanilla needs `minecraft`, e.g. \"1.21.1\".", "components.available_versions uid=net.minecraft lists them."));
        return;
    }
    const auto loader = arguments.value("loader").toString();
    const auto loaderVersionWanted = arguments.value("loaderVersion").toString();
    if (!loader.isEmpty() && !loaderUids().contains(loader)) {
        reply(ToolResult::error(QString("Unknown loader '%1'.").arg(loader), "Use fabric, quilt, forge, neoforge or liteloader."));
        return;
    }

    withVersionList(MINECRAFT_UID, reply, [context, minecraft, loader, loaderVersionWanted, options, reply](const Meta::VersionList::Ptr& list) {
        auto version = list->getVersion(minecraft);
        if (!version || !list->hasVersion(minecraft)) {
            QStringList all;
            for (const auto& v : list->versions()) {
                all.append(v->version());
            }
            const auto close = closestMatches(minecraft, all);
            reply(ToolResult::error(QString("Minecraft %1 does not exist.").arg(minecraft),
                                    close.isEmpty() ? QString() : QString("Closest: %1").arg(close.join(", "))));
            return;
        }

        auto finalOptions = options;
        if (loader.isEmpty()) {
            if (finalOptions.name.isEmpty()) {
                finalOptions.name = minecraft;
            }
            startCreation(context, new VanillaCreationTask(version), finalOptions, QString("Create Minecraft %1 instance").arg(minecraft), reply);
            return;
        }

        const auto uid = loaderUids().value(loader);
        withVersionList(uid, reply, [context, minecraft, version, loader, uid, loaderVersionWanted, finalOptions, reply](
                                        const Meta::VersionList::Ptr& loaders) mutable {
            Meta::Version::Ptr chosen;
            Meta::Version::Ptr firstRelease;
            for (const auto& candidate : loaders->versions()) {
                bool fits = true;
                for (const auto& require : candidate->requiredSet()) {
                    if (require.uid == MINECRAFT_UID && !require.equalsVersion.isEmpty()) {
                        fits = require.equalsVersion == minecraft;
                    }
                }
                if (!fits) {
                    continue;
                }
                if (!loaderVersionWanted.isEmpty()) {
                    if (candidate->version() == loaderVersionWanted) {
                        chosen = candidate;
                        break;
                    }
                    continue;
                }
                if (candidate->isRecommended()) {
                    chosen = candidate;
                    break;
                }
                if (!firstRelease && (candidate->type().isEmpty() || candidate->type() == "release")) {
                    firstRelease = candidate;
                }
            }
            if (!chosen) {
                chosen = firstRelease;
            }
            if (!chosen) {
                reply(ToolResult::error(loaderVersionWanted.isEmpty()
                                            ? QString("%1 has no version for Minecraft %2.").arg(loader, minecraft)
                                            : QString("%1 %2 does not exist for Minecraft %3.").arg(loader, loaderVersionWanted, minecraft),
                                        QString("components.available_versions uid=%1 minecraft=%2 lists them.").arg(uid, minecraft)));
                return;
            }
            if (finalOptions.name.isEmpty()) {
                finalOptions.name = QString("%1 %2").arg(minecraft, loader);
            }
            startCreation(context, new VanillaCreationTask(version, uid, chosen), finalOptions,
                          QString("Create Minecraft %1 + %2 %3 instance").arg(minecraft, loader, chosen->version()), reply);
        });
    });
}

void createFromModpack(const CallContext& context, const QJsonObject& arguments, const Options& options, const ToolReply& reply)
{
    const auto platform = arguments.value("platform").toString();
    const auto projectId = arguments.value("projectId").toString();
    const auto versionId = arguments.value("versionId").toString();
    if (platform.isEmpty() || projectId.isEmpty()) {
        reply(ToolResult::error("source=modpack needs `platform` and `projectId` (search_content kind=modpack finds them)."));
        return;
    }
    const bool curseForge = platform == "curseforge";
    if (curseForge && !APPLICATION->capabilities().testFlag(Application::SupportsFlame)) {
        reply(ToolResult::error("CurseForge is not available in this build (no API key)."));
        return;
    }
    const ResourceAPI& api = curseForge ? static_cast<const ResourceAPI&>(FlameAPI::get()) : static_cast<const ResourceAPI&>(ModrinthAPI::get());

    auto pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = projectId;
    pack->provider = curseForge ? ModPlatform::ResourceProvider::FLAME : ModPlatform::ResourceProvider::MODRINTH;
    pack->resourceType = ModPlatform::ResourceType::Modpack;

    auto keepAlive = std::make_shared<Task::Ptr>();
    ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks;
    callbacks.onSucceed = [context, options, reply, projectId, versionId, keepAlive](QVector<ModPlatform::IndexedVersion>& versions) {
        keepAlive->reset();
        std::optional<ModPlatform::IndexedVersion> chosen;
        for (const auto& version : versions) {
            if (!versionId.isEmpty()) {
                if (version.fileId.toString() == versionId) {
                    chosen = version;
                    break;
                }
                continue;
            }
            if (version.versionType.value() == ModPlatform::IndexedVersionTypeValue::Release) {
                chosen = version;
                break;
            }
            if (!chosen) {
                chosen = version;
            }
        }
        if (!chosen) {
            reply(ToolResult::error(versionId.isEmpty() ? QString("The modpack has no versions.") : QString("The modpack has no version '%1'.").arg(versionId),
                                    "content_versions kind=modpack lists them."));
            return;
        }
        if (chosen->downloadUrl.isEmpty()) {
            reply(ToolResult::error("That modpack version has no download link."));
            return;
        }
        QMap<QString, QString> extraInfo;
        extraInfo.insert("pack_id", projectId);
        extraInfo.insert("pack_version_id", chosen->fileId.toString());
        auto* task = new InstanceImportTask(QUrl(chosen->downloadUrl), true, nullptr, std::move(extraInfo));
        startCreation(context, task, options, QString("Install modpack version %1").arg(chosen->version), reply);
    };
    callbacks.onFail = [reply, keepAlive](const QString& reason, int) {
        keepAlive->reset();
        reply(ToolResult::error(QString("Could not list the modpack's versions: %1").arg(reason), "Check platform and projectId."));
    };
    callbacks.onAbort = [reply, keepAlive] {
        keepAlive->reset();
        reply(ToolResult::error("Cancelled"));
    };

    auto task = api.getProjectVersions({ .pack = pack, .mcVersions = {}, .loaders = {}, .resourceType = ModPlatform::ResourceType::Modpack },
                                       callbacks);
    if (!task) {
        reply(ToolResult::error("This platform has no version listing for modpacks."));
        return;
    }
    if (auto* netJob = dynamic_cast<NetJob*>(task.get())) {
        netJob->setAskRetry(false);
    }
    *keepAlive = task;
    task->start();
}

void createFromImport(const CallContext& context, const QJsonObject& arguments, Options options, const ToolReply& reply)
{
    const auto source = arguments.value("url").toString().trimmed();
    const auto url = QUrl::fromUserInput(source);
    if (source.isEmpty() || !url.isValid()) {
        reply(ToolResult::error("source=import needs `url`: an http(s) link to a pack archive or a local file path."));
        return;
    }
    // The user allowed this import (it is confirmed as a destructive action), so files the pack ships itself are kept
    options.choices.acceptUntrustedFiles = true;
    startCreation(context, new InstanceImportTask(url, false, nullptr, {}), options, QString("Import %1").arg(source), reply);
}

void createInstance(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    Options options;
    options.name = arguments.value("name").toString().trimmed();
    options.group = arguments.value("group").toString().trimmed();
    options.icon = arguments.value("icon").toString();
    for (const auto& file : arguments.value("optionalFiles").toArray()) {
        options.choices.optionalFiles.append(file.toString());
    }

    const auto source = arguments.value("source").toString();
    if (source == "vanilla") {
        createVanilla(context, arguments, options, reply);
    } else if (source == "modpack") {
        createFromModpack(context, arguments, options, reply);
    } else {
        createFromImport(context, arguments, options, reply);
    }
}

// --- managed pack updates ---

void updatePack(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    if (!instance->isManagedPack()) {
        reply(ToolResult::error(QString("'%1' was not installed from a modpack platform.").arg(instance->name())));
        return;
    }
    const auto type = instance->getManagedPackType();
    if (type != "modrinth" && type != "flame") {
        reply(ToolResult::error(QString("Updating %1 packs is not supported here; use the launcher's Modpack page.").arg(type)));
        return;
    }
    if (instance->isRunning()) {
        reply(ToolResult::error(QString("'%1' is running; stop it before updating.").arg(instance->name())));
        return;
    }
    const bool curseForge = type == "flame";
    if (curseForge && !APPLICATION->capabilities().testFlag(Application::SupportsFlame)) {
        reply(ToolResult::error("CurseForge is not available in this build (no API key)."));
        return;
    }
    const ResourceAPI& api = curseForge ? static_cast<const ResourceAPI&>(FlameAPI::get()) : static_cast<const ResourceAPI&>(ModrinthAPI::get());

    auto pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = instance->getManagedPackID();
    pack->provider = curseForge ? ModPlatform::ResourceProvider::FLAME : ModPlatform::ResourceProvider::MODRINTH;
    pack->resourceType = ModPlatform::ResourceType::Modpack;
    const auto versionId = arguments.value("versionId").toString();
    const bool allowDuplicates = arguments.value("allowDuplicateFiles").toBool(false);
    const auto instanceId = instance->id();

    auto keepAlive = std::make_shared<Task::Ptr>();
    ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks;
    callbacks.onSucceed = [context, reply, versionId, allowDuplicates, instanceId, keepAlive](QVector<ModPlatform::IndexedVersion>& versions) {
        keepAlive->reset();
        auto* target = APPLICATION->instances()->getInstanceById(instanceId);
        if (!target) {
            reply(ToolResult::error("The instance disappeared."));
            return;
        }
        std::optional<ModPlatform::IndexedVersion> chosen;
        for (const auto& version : versions) {
            if (!versionId.isEmpty() ? version.fileId.toString() == versionId
                                     : version.versionType.value() == ModPlatform::IndexedVersionTypeValue::Release) {
                chosen = version;
                break;
            }
        }
        if (!chosen && versionId.isEmpty() && !versions.isEmpty()) {
            chosen = versions.first();
        }
        if (!chosen) {
            reply(ToolResult::error(versionId.isEmpty() ? QString("The pack has no versions.") : QString("The pack has no version '%1'.").arg(versionId),
                                    "content_versions kind=modpack lists them."));
            return;
        }
        if (chosen->fileId.toString() == target->getManagedPackVersionID()) {
            reply(ToolResult::ok(QJsonObject{ { "instance", instanceId }, { "upToDate", true }, { "version", chosen->version } },
                                 QString("'%1' is already on %2.").arg(target->name(), chosen->version)));
            return;
        }
        if (chosen->downloadUrl.isEmpty()) {
            reply(ToolResult::error("That pack version has no download link."));
            return;
        }

        // The same setup as the Modpack page's update button
        QMap<QString, QString> extraInfo;
        extraInfo.insert("pack_id", target->getManagedPackID());
        extraInfo.insert("pack_version_id", chosen->fileId.toString());
        extraInfo.insert("original_instance_id", target->id());
        auto* task = new InstanceImportTask(QUrl(chosen->downloadUrl), true, nullptr, std::move(extraInfo));
        task->setOriginalName(target->getManagedPackName(), chosen->version);
        task->setConfirmUpdate(false);

        Options options;
        options.name = target->getManagedPackVersionName().isEmpty() ? target->name()
                                                                     : QString(target->name()).replace(target->getManagedPackVersionName(), chosen->version);
        options.group = APPLICATION->instances()->getInstanceGroup(target->id());
        options.icon = target->iconKey();
        options.choices.allowDuplicateFiles = allowDuplicates;
        startCreation(context, task, options, QString("Update '%1' to %2").arg(target->name(), chosen->version), reply);
    };
    callbacks.onFail = [reply, keepAlive](const QString& reason, int) {
        keepAlive->reset();
        reply(ToolResult::error(QString("Could not list the pack's versions: %1").arg(reason)));
    };
    callbacks.onAbort = [reply, keepAlive] {
        keepAlive->reset();
        reply(ToolResult::error("Cancelled"));
    };

    auto task = api.getProjectVersions({ .pack = pack, .mcVersions = {}, .loaders = {}, .resourceType = ModPlatform::ResourceType::Modpack },
                                       callbacks);
    if (!task) {
        reply(ToolResult::error("This platform has no version listing for modpacks."));
        return;
    }
    if (auto* netJob = dynamic_cast<NetJob*>(task.get())) {
        netJob->setAskRetry(false);
    }
    *keepAlive = task;
    task->start();
}
}  // namespace

void registerPackOps(ToolRegistry& ops)
{
    using namespace Schema;
    Tool tool;
    tool.name = "pack.update";
    tool.description =
        "Updates an instance installed from a Modrinth or CurseForge modpack to another version of that pack (default: its newest "
        "release), in place, as a job: pack files are replaced, files the pack no longer ships are removed, worlds are kept. "
        "content_versions kind=modpack with the instance's managedPack id lists versions. Refused while the game runs. If the "
        "instance lacks the old version's file index, updating could leave old files behind; that fails unless "
        "allowDuplicateFiles=true.";
    tool.inputSchema = object({ { "instance", string("Instance id") },
                                { "versionId", string("Pack version to move to (default newest release)") },
                                { "allowDuplicateFiles", boolean("Update even without the old file index (default false)") } },
                              { "instance" });
    tool.scope = Scope::Instances;
    tool.annotations.readOnly = false;
    tool.annotations.destructive = true;
    tool.annotations.openWorld = true;
    tool.describeAction = [](const QJsonObject& a) {
        const auto id = a.value("instance").toString();
        auto* instance = APPLICATION->instances()->getInstanceById(id);
        return QString("Update the modpack of '%1'%2 (pack files are replaced)")
            .arg(instance ? instance->name() : id, a.value("versionId").toString().isEmpty() ? QString(" to its newest release")
                                                                                              : QString(" to version %1").arg(a.value("versionId").toString()));
    };
    tool.handler = updatePack;
    ops.add(tool);
}

void registerCreateTool(ToolRegistry& registry)
{
    using namespace Schema;

    Tool tool;
    tool.name = "create_instance";
    tool.title = "Create an instance";
    tool.description =
        "Creates a new instance and returns a job id at once (follow it with jobs action=wait; its result holds the new instance).\n"
        "source=vanilla: Minecraft `minecraft`, optionally a `loader` (fabric, quilt, forge, neoforge, liteloader) at `loaderVersion` "
        "(default: the recommended or newest release for that Minecraft version).\n"
        "source=modpack: a Modrinth or CurseForge modpack by `platform` + `projectId` (search_content kind=modpack), at `versionId` "
        "or its newest release. Optional pack files are left disabled unless listed in `optionalFiles`. Always creates a separate "
        "instance, even if the pack is already installed.\n"
        "source=import: a pack archive (CurseForge, Modrinth .mrpack, MultiMC/Prism zip) from an http(s) `url` or a local path; "
        "the user is asked first because such a pack can bring any files.";
    tool.inputSchema = object(
        {
            { "source", string("Where the instance comes from", { "vanilla", "modpack", "import" }) },
            { "name", string("Instance name (default: from the version or pack)") },
            { "group", string("Group to put it in") },
            { "icon", string("Icon key (default \"default\")") },
            { "minecraft", string("vanilla: Minecraft version, e.g. 1.21.1") },
            { "loader", string("vanilla: mod loader", { "fabric", "quilt", "forge", "neoforge", "liteloader" }) },
            { "loaderVersion", string("vanilla: exact loader version") },
            { "platform", string("modpack: where it lives", { "modrinth", "curseforge" }) },
            { "projectId", string("modpack: project id") },
            { "versionId", string("modpack: a versionId from content_versions kind=modpack") },
            { "optionalFiles", withDescription(array(string({})), "modpack/import: optional pack files to enable, by path inside the pack") },
            { "url", string("import: http(s) URL or local path of the archive") },
        },
        { "source" });
    tool.outputSchema = object({ { "job", string("Job id") } }, { "job" }, false);
    tool.annotations.readOnly = false;
    tool.annotations.destructive = false;
    tool.annotations.idempotent = false;
    tool.annotations.openWorld = true;
    tool.scope = Scope::Instances;
    tool.effect = [](const QJsonObject& arguments) {
        return arguments.value("source").toString() == "import" ? Effect::Destructive : Effect::Write;
    };
    tool.describeAction = [](const QJsonObject& arguments) {
        const auto source = arguments.value("source").toString();
        if (source == "vanilla") {
            const auto loader = arguments.value("loader").toString();
            return QString("Create a Minecraft %1%2 instance")
                .arg(arguments.value("minecraft").toString(), loader.isEmpty() ? QString() : QString(" + %1").arg(loader));
        }
        if (source == "modpack") {
            return QString("Install the %1 modpack %2 as a new instance").arg(arguments.value("platform").toString(), arguments.value("projectId").toString());
        }
        return QString("Import and keep every file of the pack at %1").arg(arguments.value("url").toString());
    };
    tool.handler = createInstance;
    registry.add(std::move(tool));
}

}  // namespace Mcp
