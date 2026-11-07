// SPDX-License-Identifier: GPL-3.0-only
#include "McpUpdateOps.h"

#include <QJsonArray>

#include <memory>

#include "Application.h"
#include "InstanceList.h"
#include "Json.h"
#include "ResourceDownloadTask.h"
#include "Version.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/Resource.h"
#include "minecraft/mod/ResourceFolderModel.h"
#include "modplatform/CheckUpdateTask.h"
#include "modplatform/EnsureMetadataTask.h"
#include "modplatform/flame/FlameCheckUpdate.h"
#include "modplatform/modrinth/ModrinthCheckUpdate.h"
#include "settings/SettingsObject.h"
#include "tasks/ConcurrentTask.h"
#include "tasks/SequentialTask.h"

namespace Mcp {

namespace {

// Everything one update check needs to keep alive: the check tasks hold references to these lists
struct UpdateCheck {
    MinecraftInstance* instance = nullptr;
    ResourceFolderModel* model = nullptr;
    std::vector<Version> versions;
    QList<ModPlatform::ModLoaderType> loaders;
    std::vector<ModPlatform::IndexedVersionType> releaseTypes;

    QList<Resource*> needMetadata;
    QList<Resource*> retryOnFlame;
    QList<Resource*> modrinth;
    QList<Resource*> flame;

    QJsonArray untracked;  // no metadata found on any platform
    QJsonArray locked;
    QJsonArray failed;

    shared_qobject_ptr<CheckUpdateTask> modrinthCheck;
    shared_qobject_ptr<CheckUpdateTask> flameCheck;
    std::vector<CheckUpdateTask::Update> updates;
    QList<std::shared_ptr<GetModDependenciesTask::PackDependency>> dependencies;
};
using CheckPtr = std::shared_ptr<UpdateCheck>;

bool curseForgeAvailable()
{
    return APPLICATION->capabilities().testFlag(Application::SupportsFlame);
}

void track(const CheckPtr& check, Resource* resource)
{
    if (!resource->metadata()) {
        return;
    }
    if (resource->lockUpdate()) {
        check->locked.append(resource->fileinfo().fileName());
        return;
    }
    if (resource->metadata()->provider == ModPlatform::ResourceProvider::FLAME) {
        check->flame.append(resource);
    } else {
        check->modrinth.append(resource);
    }
}

// Looks up metadata for files that have none, on one platform; `done` runs either way
void ensureMetadata(const CheckPtr& check, QList<Resource*>& resources, ModPlatform::ResourceProvider provider, std::function<void()> done)
{
    if (resources.isEmpty()) {
        done();
        return;
    }
    auto ensure = makeShared<EnsureMetadataTask>(resources, check->model->indexDir(), provider);
    QObject::connect(ensure.get(), &EnsureMetadataTask::metadataReady, ensure.get(), [check](Resource* resource) { track(check, resource); });
    QObject::connect(ensure.get(), &EnsureMetadataTask::metadataFailed, ensure.get(), [check, provider](Resource* resource) {
        if (provider == ModPlatform::ResourceProvider::MODRINTH && curseForgeAvailable()) {
            check->retryOnFlame.append(resource);
        } else {
            check->untracked.append(resource->fileinfo().fileName());
        }
    });
    auto sequence = makeShared<SequentialTask>(QObject::tr("Looking for metadata"));
    if (auto hashing = ensure->getHashingTask()) {
        sequence->addTask(hashing);
    }
    sequence->addTask(ensure);
    runTask(sequence, [done](bool, const QString&) { done(); });
}

void runChecks(const CheckPtr& check, std::function<void(Result<>)> done)
{
    auto sequence = makeShared<SequentialTask>(QObject::tr("Checking for updates"));
    auto onFailed = [check](Resource* resource, const QString& reason, const QUrl&) {
        check->failed.append(QJsonObject{ { "file", resource->fileinfo().fileName() }, { "reason", reason } });
    };
    if (!check->modrinth.isEmpty()) {
        check->modrinthCheck.reset(new ModrinthCheckUpdate(check->modrinth, check->versions, check->loaders, check->model, check->releaseTypes));
        QObject::connect(check->modrinthCheck.get(), &CheckUpdateTask::checkFailed, check->modrinthCheck.get(), onFailed);
        sequence->addTask(check->modrinthCheck);
    }
    if (!check->flame.isEmpty() && curseForgeAvailable()) {
        check->flameCheck.reset(new FlameCheckUpdate(check->flame, check->versions, check->loaders, check->model, check->releaseTypes));
        QObject::connect(check->flameCheck.get(), &CheckUpdateTask::checkFailed, check->flameCheck.get(), onFailed);
        sequence->addTask(check->flameCheck);
    }
    runTask(sequence, [check, done](bool ok, const QString& error) {
        for (auto* task : { check->modrinthCheck.get(), check->flameCheck.get() }) {
            if (!task) {
                continue;
            }
            for (auto& update : task->getUpdates()) {
                check->updates.push_back(std::move(update));
            }
            check->dependencies.append(task->getDependencies());
        }
        if (!ok && check->updates.empty()) {
            done(std::unexpected(error));
            return;
        }
        done({});
    });
}

// The whole check: metadata for untracked files (Modrinth first, then CurseForge), then the platforms' update checks
void checkForUpdates(MinecraftInstance* instance, ResourceFolderModel* model, const QStringList& onlyFiles, bool findMetadata,
                     std::function<void(Result<CheckPtr>)> done)
{
    whenModelReady(model, [instance, model, onlyFiles, findMetadata, done] {
        auto check = std::make_shared<UpdateCheck>();
        check->instance = instance;
        check->model = model;
        auto* profile = instance->getPackProfile();
        check->versions = { Version(profile->getComponentVersion("net.minecraft")) };
        check->loaders = profile->getModLoadersList();
        check->releaseTypes = ModPlatform::IndexedVersionType::fromStringList(
            Json::toStringList(APPLICATION->settings()->get("ModUpdateReleaseTypes").toString()));

        for (auto* resource : model->allResources()) {
            if (!onlyFiles.isEmpty() && !onlyFiles.contains(resource->fileinfo().fileName())) {
                continue;
            }
            if (resource->status() == ResourceStatus::NoMetadata) {
                if (resource->type() != ResourceType::FOLDER) {
                    if (findMetadata) {
                        check->needMetadata.append(resource);
                    } else {
                        check->untracked.append(resource->fileinfo().fileName());
                    }
                }
                continue;
            }
            track(check, resource);
        }

        ensureMetadata(check, check->needMetadata, ModPlatform::ResourceProvider::MODRINTH, [check, done] {
            ensureMetadata(check, check->retryOnFlame, ModPlatform::ResourceProvider::FLAME, [check, done] {
                runChecks(check, [check, done](Result<> result) {
                    if (!result) {
                        done(std::unexpected(result.error()));
                        return;
                    }
                    done(check);
                });
            });
        });
    });
}

QJsonObject report(const CheckPtr& check)
{
    QJsonArray updates;
    for (const auto& update : check->updates) {
        updates.append(QJsonObject{
            { "name", update.name },
            { "file", update.download ? update.download->getFilename() : QString() },
            { "currentVersion", update.oldVersion },
            { "newVersion", update.newVersion },
            { "newVersionType", update.newVersionType ? update.newVersionType->toString().toLower() : QString() },
            { "platform", QString::fromLatin1(ModPlatform::ProviderCapabilities::name(update.provider)) },
        });
    }
    QJsonArray dependencies;
    for (const auto& dependency : check->dependencies) {
        dependencies.append(QJsonObject{ { "name", dependency->pack->name }, { "projectId", dependency->pack->addonId.toString() } });
    }
    return QJsonObject{ { "instance", check->instance->id() },
                        { "updates", updates },
                        { "newDependencies", dependencies },
                        { "untracked", check->untracked },
                        { "locked", check->locked },
                        { "failed", check->failed } };
}

QStringList fileFilter(const QJsonObject& arguments)
{
    QStringList files;
    for (const auto& file : arguments.value("files").toArray()) {
        files.append(file.toString());
    }
    return files;
}

void checkUpdatesOp(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* model = contentModel(*found, *contentKindFromName(arguments.value("kind").toString("mods")));
    checkForUpdates(*found, model, fileFilter(arguments), arguments.value("findMetadata").toBool(true), [reply](Result<CheckPtr> result) {
        if (!result) {
            reply(ToolResult::error(QString("Checking for updates failed: %1").arg(result.error())));
            return;
        }
        reply(ToolResult::ok(report(*result)));
    });
}

void updateOp(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    if (instance->isRunning()) {
        reply(ToolResult::error(QString("'%1' is running; replacing its files now could crash the game.").arg(instance->name())));
        return;
    }
    auto* model = contentModel(instance, *contentKindFromName(arguments.value("kind").toString("mods")));
    const bool withDependencies = arguments.value("dependencies").toString("required") == "required";

    checkForUpdates(instance, model, fileFilter(arguments), true, [context, instance, model, withDependencies, reply](Result<CheckPtr> result) {
        if (!result) {
            reply(ToolResult::error(QString("Checking for updates failed: %1").arg(result.error())));
            return;
        }
        auto check = *result;
        const auto summary = report(check);
        if (check->updates.empty()) {
            reply(ToolResult::ok(summary, "Everything checked is up to date."));
            return;
        }
        auto* registry = jobs();
        if (!registry) {
            reply(ToolResult::error("Jobs are not available."));
            return;
        }
        auto tasks = makeShared<ConcurrentTask>(QObject::tr("Update content"), APPLICATION->settings()->get("NumberOfConcurrentDownloads").toInt());
        for (auto& update : check->updates) {
            if (update.download) {
                tasks->addTask(update.download);
            }
        }
        if (withDependencies) {
            const bool indexed = !APPLICATION->settings()->get("ModMetadataDisabled").toBool();
            for (const auto& dependency : check->dependencies) {
                tasks->addTask(makeShared<ResourceDownloadTask>(dependency->pack, dependency->version, model, indexed, "dependency"));
            }
        }
        const auto id = registry->start(context.token.id, context.token.name,
                                        QString("Update %1 file(s) in %2").arg(check->updates.size()).arg(instance->name()), tasks, [model, summary] {
                                            model->update();
                                            return summary;
                                        });
        auto out = summary;
        out["job"] = id;
        reply(ToolResult::ok(out, QString("Updating %1 file(s) as job %2; follow it with jobs action=wait.").arg(check->updates.size()).arg(id)));
    });
}

QString instanceLabel(const QJsonObject& arguments)
{
    const auto id = arguments.value("instance").toString();
    auto* instance = APPLICATION->instances()->getInstanceById(id);
    return instance ? instance->name() : id;
}
}  // namespace

void registerUpdateOps(ToolRegistry& ops)
{
    using namespace Schema;
    const auto common = QJsonObject{
        { "instance", string("Instance id") },
        { "kind", string("Which folder (default mods)", contentKindNames()) },
        { "files", withDescription(array(string({})), "Only these files (content.list names); default all") },
    };

    {
        auto input = common;
        input["findMetadata"] = boolean("Look up files with no known origin on Modrinth, then CurseForge (default true)");
        Tool tool;
        tool.name = "content.check_updates";
        tool.description =
            "Checks an instance's mods (or packs) for updates on Modrinth and CurseForge, using the release types allowed in the "
            "launcher's settings. Files with no known origin are looked up by hash first. Reports available updates, new "
            "dependencies they bring, and files that are untracked, locked against updates, or failed to check.";
        tool.inputSchema = object(input, { "instance" });
        tool.scope = Scope::Read;
        tool.annotations.openWorld = true;
        tool.handler = checkUpdatesOp;
        ops.add(tool);
    }

    {
        auto input = common;
        input["dependencies"] = string("Also install new required dependencies (default required)", { "required", "none" });
        Tool tool;
        tool.name = "content.update";
        tool.description =
            "Updates an instance's mods (or packs) to their newest allowed versions, as a job: checks like content.check_updates, "
            "then replaces each outdated file. Refused while the game runs. The result lists what was updated.";
        tool.inputSchema = object(input, { "instance" });
        tool.scope = Scope::Content;
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.annotations.openWorld = true;
        tool.describeAction = [](const QJsonObject& a) {
            const auto files = fileFilter(a);
            return QString("Update %1 in '%2'").arg(files.isEmpty() ? a.value("kind").toString("mods") : files.join(", "), instanceLabel(a));
        };
        tool.handler = updateOp;
        ops.add(tool);
    }
}

}  // namespace Mcp
