// SPDX-License-Identifier: GPL-3.0-only
#include "McpPlatformTools.h"

#include <QJsonArray>
#include <QHash>
#include <QTimer>

#include <memory>
#include <utility>

#include "Application.h"
#include "InstanceList.h"
#include "ResourceDownloadTask.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "minecraft/mod/ResourceFolderModel.h"
#include "minecraft/mod/tasks/GetModDependenciesTask.h"
#include "settings/SettingsObject.h"
#include "tasks/ConcurrentTask.h"
#include "Version.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "modplatform/ModIndex.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"

namespace Mcp {

namespace {
constexpr auto PLATFORM_MODRINTH = "modrinth";
constexpr auto PLATFORM_CURSEFORGE = "curseforge";
constexpr int DEFAULT_VERSION_LIMIT = 20;
constexpr int MAX_VERSION_LIMIT = 500;

const QStringList& kindNames()
{
    static const QStringList s_names{ "mod", "modpack", "resourcepack", "shaderpack", "datapack" };
    return s_names;
}

ModPlatform::ResourceType resourceTypeFor(const QString& kind)
{
    if (kind == "modpack") {
        return ModPlatform::ResourceType::Modpack;
    }
    if (kind == "resourcepack") {
        return ModPlatform::ResourceType::ResourcePack;
    }
    if (kind == "shaderpack") {
        return ModPlatform::ResourceType::ShaderPack;
    }
    if (kind == "datapack") {
        return ModPlatform::ResourceType::DataPack;
    }
    return ModPlatform::ResourceType::Mod;
}

const ResourceAPI& apiFor(const QString& platform)
{
    if (platform == PLATFORM_CURSEFORGE) {
        return FlameAPI::get();
    }
    return ModrinthAPI::get();
}

bool curseForgeAvailable()
{
    return APPLICATION->capabilities().testFlag(Application::SupportsFlame);
}

QStringList loaderNames(ModPlatform::ModLoaderTypes loaders)
{
    QStringList out;
    for (auto loader : ModPlatform::modLoaderTypesToList(loaders)) {
        out.append(ModPlatform::getModLoaderAsString(loader));
    }
    return out;
}

// Our sort names mapped onto each platform's sorting method names
std::optional<ResourceAPI::SortingMethod> sortingFor(const QString& platform, const QString& sort)
{
    static const QHash<QString, std::pair<QString, QString>> s_names{
        { "relevance", { "relevance", "Featured" } },
        { "downloads", { "downloads", "TotalDownloads" } },
        { "updated", { "updated", "LastUpdated" } },
        { "newest", { "newest", "LastUpdated" } },
    };
    const auto names = s_names.value(sort, s_names.value("relevance"));
    const auto wanted = platform == PLATFORM_CURSEFORGE ? names.second : names.first;
    for (const auto& method : apiFor(platform).getSortingMethods()) {
        if (method.name == wanted) {
            return method;
        }
    }
    return std::nullopt;
}

QJsonObject packJson(const ModPlatform::IndexedPack& pack, const QString& platform)
{
    QJsonArray authors;
    for (const auto& author : pack.authors) {
        authors.append(author.name);
    }
    return QJsonObject{
        { "platform", platform },
        { "projectId", pack.addonId.toString() },
        { "slug", pack.slug },
        { "name", pack.name },
        { "description", pack.description },
        { "authors", authors },
        { "url", pack.websiteUrl },
    };
}

QJsonObject versionJson(const ModPlatform::IndexedVersion& version)
{
    QJsonArray dependencies;
    for (const auto& dependency : version.dependencies) {
        dependencies.append(QJsonObject{ { "projectId", dependency.addonId.toString() }, { "type", dependency.type.toString().toLower() } });
    }
    QJsonObject out{
        { "versionId", version.fileId.toString() },
        { "name", version.version },
        { "versionNumber", version.versionNumber },
        { "type", version.versionType.toString().toLower() },
        { "date", version.date },
        { "minecraft", QJsonArray::fromStringList(version.mcVersion) },
        { "loaders", QJsonArray::fromStringList(loaderNames(version.loaders)) },
        { "fileName", version.fileName },
        { "dependencies", dependencies },
    };
    if (!version.hash.isEmpty()) {
        out["hash"] = QJsonObject{ { "algorithm", version.hashType }, { "value", version.hash } };
    }
    return out;
}

// Search one platform; `done` gets the results or an error
void searchPlatform(const QString& platform, const ResourceAPI::SearchArgs& args,
                    std::function<void(Result<QJsonArray>)> done)
{
    auto [job, response] = apiFor(platform).searchProjectsTask(args);
    auto keepAlive = std::make_shared<NetJob::Ptr>(job);
    auto finish = [keepAlive, done = std::move(done)](Result<QJsonArray> result) {
        if (!*keepAlive) {
            return;
        }
        keepAlive->reset();
        done(std::move(result));
    };
    QObject::connect(job.get(), &Task::succeeded, job.get(), [finish, response, platform] {
        QJsonArray out;
        for (const auto& pack : *response) {
            out.append(packJson(pack, platform));
        }
        finish(out);
    });
    QObject::connect(job.get(), &Task::failed, job.get(), [finish](const QString& reason) { finish(std::unexpected(reason)); });
    QObject::connect(job.get(), &Task::aborted, job.get(), [finish] { finish(std::unexpected(QString("Cancelled"))); });
    job->setAskRetry(false);
    job->start();
}

void searchContent(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto kind = arguments.value("kind").toString("mod");
    const auto platform = arguments.value("platform").toString("both");

    ResourceAPI::SearchArgs args;
    args.type = resourceTypeFor(kind);
    args.offset = arguments.value("offset").toInt(0);
    if (const auto query = arguments.value("query").toString(); !query.isEmpty()) {
        args.search = query;
    }
    if (const auto minecraft = arguments.value("minecraft").toString(); !minecraft.isEmpty()) {
        args.versions = std::vector<Version>{ Version(minecraft) };
    }
    if (const auto loader = arguments.value("loader").toString(); !loader.isEmpty()) {
        const auto type = ModPlatform::getModLoaderFromString(loader);
        if (static_cast<int>(type) == 0) {
            reply(ToolResult::error(QString("Unknown loader '%1'.").arg(loader),
                                    "Use one of: neoforge, forge, fabric, quilt, liteloader, cauldron, babric, bta-babric, legacy-fabric, "
                                    "ornithe, rift."));
            return;
        }
        args.loaders = ModPlatform::ModLoaderTypes(type);
    }
    const auto sort = arguments.value("sort").toString("relevance");

    QStringList platforms;
    QJsonArray notes;
    if (platform == "both" || platform == PLATFORM_MODRINTH) {
        platforms.append(PLATFORM_MODRINTH);
    }
    if (platform == "both" || platform == PLATFORM_CURSEFORGE) {
        if (curseForgeAvailable()) {
            platforms.append(PLATFORM_CURSEFORGE);
        } else {
            notes.append("CurseForge is not available in this build (no API key).");
        }
    }
    if (platforms.isEmpty()) {
        reply(ToolResult::error("No platform available to search.", notes.isEmpty() ? QString() : notes.first().toString()));
        return;
    }

    struct State {
        int pending = 0;
        QJsonArray results;
        QJsonArray notes;
    };
    auto state = std::make_shared<State>();
    state->pending = static_cast<int>(platforms.size());
    state->notes = notes;

    for (const auto& name : platforms) {
        auto platformArgs = args;
        platformArgs.sorting = sortingFor(name, sort);
        searchPlatform(name, platformArgs, [state, name, reply, offset = args.offset](Result<QJsonArray> result) {
            if (result) {
                for (const auto& item : *result) {
                    state->results.append(item);
                }
            } else {
                state->notes.append(QString("%1 search failed: %2").arg(name, result.error()));
            }
            if (--state->pending > 0) {
                return;
            }
            QJsonObject out{ { "results", state->results }, { "offset", offset }, { "count", state->results.size() } };
            if (!state->notes.isEmpty()) {
                out["notes"] = state->notes;
            }
            reply(ToolResult::ok(out));
        });
    }
}

void contentVersions(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto platform = arguments.value("platform").toString();
    if (platform == PLATFORM_CURSEFORGE && !curseForgeAvailable()) {
        reply(ToolResult::error("CurseForge is not available in this build (no API key)."));
        return;
    }
    const int limit = arguments.value("limit").toInt(DEFAULT_VERSION_LIMIT);

    auto pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = arguments.value("projectId").toString();
    pack->provider = platform == PLATFORM_CURSEFORGE ? ModPlatform::ResourceProvider::FLAME : ModPlatform::ResourceProvider::MODRINTH;
    pack->resourceType = resourceTypeFor(arguments.value("kind").toString("mod"));

    ResourceAPI::VersionSearchArgs args{ .pack = pack, .mcVersions = {}, .loaders = {}, .resourceType = pack->resourceType };
    if (const auto minecraft = arguments.value("minecraft").toString(); !minecraft.isEmpty()) {
        args.mcVersions = std::vector<Version>{ Version(minecraft) };
    }
    if (const auto loader = arguments.value("loader").toString(); !loader.isEmpty()) {
        const auto type = ModPlatform::getModLoaderFromString(loader);
        if (static_cast<int>(type) == 0) {
            reply(ToolResult::error(QString("Unknown loader '%1'.").arg(loader)));
            return;
        }
        args.loaders = ModPlatform::ModLoaderTypes(type);
    }

    // The task must outlive its callbacks; they release it once one of them ran
    auto keepAlive = std::make_shared<Task::Ptr>();
    auto replied = std::make_shared<bool>(false);
    auto finish = [keepAlive, replied, reply](const ToolResult& result) {
        if (*replied) {
            return;
        }
        *replied = true;
        reply(result);
        keepAlive->reset();
    };

    ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks;
    callbacks.onSucceed = [finish, limit, platform, pack](QVector<ModPlatform::IndexedVersion>& versions) {
        QJsonArray out;
        for (const auto& version : versions) {
            if (out.size() >= limit) {
                break;
            }
            out.append(versionJson(version));
        }
        finish(ToolResult::ok(QJsonObject{ { "platform", platform },
                                           { "projectId", pack->addonId.toString() },
                                           { "count", versions.size() },
                                           { "versions", out } }));
    };
    callbacks.onFail = [finish](const QString& reason, int status) {
        finish(ToolResult::error(QString("Could not load versions: %1").arg(reason),
                                 status == 404 ? QString("Check projectId and platform; search_content returns both.") : QString()));
    };
    callbacks.onAbort = [finish] { finish(ToolResult::error("Cancelled")); };

    auto task = apiFor(platform).getProjectVersions(args, callbacks);
    if (!task) {
        reply(ToolResult::error("This platform has no version listing for that kind of project."));
        return;
    }
    // A failed request would otherwise open a "retry?" dialog and keep the agent waiting on the user
    if (auto* netJob = dynamic_cast<NetJob*>(task.get())) {
        netJob->setAskRetry(false);
    }
    *keepAlive = task;
    task->start();
}

// --- install_content ---

struct InstallRequest {
    QString platform;
    QString projectId;
    QString versionId;
};

struct InstallPlanEntry {
    ModPlatform::IndexedPack::Ptr pack;
    ModPlatform::IndexedVersion version;
    QString reason;      // "requested" or "dependency"
    QString requiredBy;  // project id that pulled a dependency in
};

QJsonObject planJson(const InstallPlanEntry& entry)
{
    QJsonObject out{
        { "platform", QString::fromLatin1(ModPlatform::ProviderCapabilities::name(entry.pack->provider)) },
        { "projectId", entry.pack->addonId.toString() },
        { "name", entry.pack->name },
        { "versionId", entry.version.fileId.toString() },
        { "version", entry.version.version },
        { "fileName", entry.version.fileName },
        { "reason", entry.reason },
    };
    if (!entry.requiredBy.isEmpty()) {
        out["requiredBy"] = entry.requiredBy;
    }
    return out;
}

// Picks the file to install: the requested version id, or the newest release (newest of any type if none) that fits
std::optional<ModPlatform::IndexedVersion> chooseVersion(const QVector<ModPlatform::IndexedVersion>& versions, const QString& versionId)
{
    if (!versionId.isEmpty()) {
        for (const auto& version : versions) {
            if (version.fileId.toString() == versionId) {
                return version;
            }
        }
        return std::nullopt;
    }
    for (const auto& version : versions) {
        if (version.versionType.value() == ModPlatform::IndexedVersionTypeValue::Release) {
            return version;
        }
    }
    if (!versions.isEmpty()) {
        return versions.first();
    }
    return std::nullopt;
}

using ResolveDone = std::function<void(Result<InstallPlanEntry>)>;

// Fetches the project, then its versions filtered to the instance, and picks one
void resolveInstallItem(const InstallRequest& item, MinecraftInstance* instance, ModPlatform::ResourceType type, ResolveDone done)
{
    const auto& api = apiFor(item.platform);
    auto [projectJob, projectResponse] = api.getProjectTask(item.projectId, false, false);
    auto keepAlive = std::make_shared<NetJob::Ptr>(projectJob);
    auto release = [keepAlive] { keepAlive->reset(); };

    QObject::connect(projectJob.get(), &Task::failed, projectJob.get(), [item, done, release](const QString& reason) {
        done(std::unexpected(QString("%1 project '%2': %3").arg(item.platform, item.projectId, reason)));
        release();
    });
    QObject::connect(projectJob.get(), &Task::succeeded, projectJob.get(), [item, instance, type, done, release, response = projectResponse] {
        auto pack = std::make_shared<ModPlatform::IndexedPack>(*response);
        pack->provider = item.platform == PLATFORM_CURSEFORGE ? ModPlatform::ResourceProvider::FLAME : ModPlatform::ResourceProvider::MODRINTH;
        pack->resourceType = type;
        release();

        auto* profile = instance->getPackProfile();
        ResourceAPI::VersionSearchArgs args{ .pack = pack, .mcVersions = {}, .loaders = {}, .resourceType = type };
        // An explicit version is taken as given; otherwise only files for this Minecraft version (and loader, for mods)
        if (item.versionId.isEmpty()) {
            args.mcVersions = std::vector<Version>{ Version(profile->getComponentVersion("net.minecraft")) };
            if (type == ModPlatform::ResourceType::Mod) {
                args.loaders = profile->getSupportedModLoaders();
            }
        }

        auto versionsKeepAlive = std::make_shared<Task::Ptr>();
        ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks;
        callbacks.onSucceed = [item, pack, done, versionsKeepAlive](QVector<ModPlatform::IndexedVersion>& versions) {
            auto chosen = chooseVersion(versions, item.versionId);
            if (!chosen) {
                done(std::unexpected(item.versionId.isEmpty()
                                         ? QString("%1 has no file for this instance's Minecraft version and loader").arg(pack->name)
                                         : QString("%1 has no version '%2'").arg(pack->name, item.versionId)));
            } else {
                done(InstallPlanEntry{ pack, *chosen, "requested", {} });
            }
            versionsKeepAlive->reset();
        };
        callbacks.onFail = [pack, done, versionsKeepAlive](const QString& reason, int) {
            done(std::unexpected(QString("Could not list versions of %1: %2").arg(pack->name, reason)));
            versionsKeepAlive->reset();
        };
        callbacks.onAbort = [done, versionsKeepAlive] {
            done(std::unexpected(QString("Cancelled")));
            versionsKeepAlive->reset();
        };

        auto task = apiFor(item.platform).getProjectVersions(args, callbacks);
        if (!task) {
            done(std::unexpected(QString("No version listing for %1").arg(pack->name)));
            return;
        }
        if (auto* netJob = dynamic_cast<NetJob*>(task.get())) {
            netJob->setAskRetry(false);
        }
        *versionsKeepAlive = task;
        task->start();
    });
    projectJob->start();
}

void startInstall(const CallContext& context,
                  MinecraftInstance* instance,
                  ResourceFolderModel* model,
                  const QList<InstallPlanEntry>& plan,
                  const QJsonArray& planJsonArray,
                  const QJsonArray& skipped,
                  const ToolReply& reply)
{
    auto* registry = jobs();
    if (!registry) {
        reply(ToolResult::error("Jobs are not available."));
        return;
    }
    const bool indexed = !APPLICATION->settings()->get("ModMetadataDisabled").toBool();
    auto tasks = makeShared<ConcurrentTask>(QObject::tr("Install content"), APPLICATION->settings()->get("NumberOfConcurrentDownloads").toInt());
    QStringList files;
    for (const auto& entry : plan) {
        tasks->addTask(makeShared<ResourceDownloadTask>(entry.pack, entry.version, model, indexed, entry.reason, entry.requiredBy));
        files.append(entry.version.fileName);
    }
    const auto instanceId = instance->id();
    const auto jobId = registry->start(context.token.id, context.token.name, QString("Install %1 file(s) into %2").arg(plan.size()).arg(instance->name()),
                                       tasks, [instanceId, files, model] {
                                           model->update();
                                           return QJsonObject{ { "instance", instanceId }, { "installed", QJsonArray::fromStringList(files) } };
                                       });
    auto result = ToolResult::ok(QJsonObject{ { "job", jobId }, { "instance", instanceId }, { "plan", planJsonArray }, { "skipped", skipped } },
                                 QString("Installing %1 file(s) as job %2; follow it with jobs action=wait.").arg(plan.size()).arg(jobId));
    result.jobId = jobId;
    reply(result);
}

void installContent(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    const auto kindName = arguments.value("kind").toString("mods");
    const auto kind = *contentKindFromName(kindName);
    const auto type = kind == ContentKind::Mods           ? ModPlatform::ResourceType::Mod
                      : kind == ContentKind::ResourcePacks ? ModPlatform::ResourceType::ResourcePack
                      : kind == ContentKind::TexturePacks  ? ModPlatform::ResourceType::TexturePack
                      : kind == ContentKind::ShaderPacks   ? ModPlatform::ResourceType::ShaderPack
                                                           : ModPlatform::ResourceType::DataPack;
    const bool withDependencies = arguments.value("dependencies").toString("required") == "required" && kind == ContentKind::Mods;
    const bool dryRun = arguments.value("dryRun").toBool(false);

    QList<InstallRequest> requests;
    for (const auto& value : arguments.value("items").toArray()) {
        const auto item = value.toObject();
        const auto platform = item.value("platform").toString();
        if (platform == PLATFORM_CURSEFORGE && !curseForgeAvailable()) {
            reply(ToolResult::error("CurseForge is not available in this build (no API key)."));
            return;
        }
        requests.append(InstallRequest{ platform, item.value("projectId").toString(), item.value("versionId").toString() });
    }
    if (instance->getPackProfile()->getComponentVersion("net.minecraft").isEmpty()) {
        TRY_OR_LOG(instance->getPackProfile()->reload(Net::Mode::Offline))
    }

    auto modelResult = contentModel(instance, kind);
    if (!modelResult) {
        reply(ToolResult::error(modelResult.error()));
        return;
    }
    auto* model = *modelResult;
    whenModelReady(model, [context, instance, model, kind, type, requests, withDependencies, dryRun, reply] {
        struct State {
            int pending = 0;
            QList<InstallPlanEntry> plan;
            QStringList errors;
        };
        auto state = std::make_shared<State>();
        state->pending = static_cast<int>(requests.size());

        auto finishResolution = [context, instance, model, kind, withDependencies, dryRun, reply, state] {
            if (!state->errors.isEmpty()) {
                reply(ToolResult::error(QString("Nothing was installed: %1").arg(state->errors.join("; ")),
                                        "Check projectId/platform with search_content and versions with content_versions."));
                return;
            }

            // Files already present at the same version are skipped; other versions of an installed project are replaced
            QJsonArray skipped;
            QList<InstallPlanEntry> plan;
            for (const auto& entry : state->plan) {
                bool same = false;
                for (int i = 0; i < model->size(); ++i) {
                    const auto metadata = model->at(i).metadata();
                    if (metadata && metadata->projectId.toString() == entry.pack->addonId.toString() &&
                        metadata->fileId.toString() == entry.version.fileId.toString()) {
                        same = true;
                    }
                }
                if (same) {
                    skipped.append(QJsonObject{ { "projectId", entry.pack->addonId.toString() }, { "name", entry.pack->name }, { "reason", "already installed" } });
                } else {
                    plan.append(entry);
                }
            }

            auto conclude = [context, instance, model, dryRun, reply, skipped](const QList<InstallPlanEntry>& finalPlan) {
                QJsonArray planArray;
                for (const auto& entry : finalPlan) {
                    planArray.append(planJson(entry));
                }
                if (dryRun || finalPlan.isEmpty()) {
                    reply(ToolResult::ok(QJsonObject{ { "dryRun", dryRun }, { "instance", instance->id() }, { "plan", planArray }, { "skipped", skipped } }));
                    return;
                }
                startInstall(context, instance, model, finalPlan, planArray, skipped, reply);
            };

            if (!withDependencies || kind != ContentKind::Mods || plan.isEmpty()) {
                conclude(plan);
                return;
            }

            QList<std::shared_ptr<GetModDependenciesTask::PackDependency>> selected;
            for (const auto& entry : plan) {
                selected.append(std::make_shared<GetModDependenciesTask::PackDependency>(entry.pack, entry.version));
            }
            auto dependencies = makeShared<GetModDependenciesTask>(instance, static_cast<ModFolderModel*>(model), selected);
            // A raw pointer: the task outlives this callback, and capturing the owning pointer would keep it alive forever
            runTask(dependencies, [dependencies = dependencies.get(), plan, conclude, reply](bool ok, const QString& error) {
                if (!ok) {
                    reply(ToolResult::error(QString("Resolving dependencies failed: %1. Nothing was installed.").arg(error),
                                            "Retry with dependencies=none to install only what you asked for."));
                    return;
                }
                auto finalPlan = plan;
                const auto extra = dependencies->getExtraInfo();
                for (const auto& dependency : dependencies->getDependecies()) {
                    const auto info = extra.value(dependency->pack->addonId.toString());
                    finalPlan.append(InstallPlanEntry{ dependency->pack, dependency->version, "dependency",
                                                       info.requiredByIds.isEmpty() ? QString() : info.requiredByIds.first() });
                }
                conclude(finalPlan);
            });
        };

        if (requests.isEmpty()) {
            finishResolution();
            return;
        }
        for (const auto& request : requests) {
            resolveInstallItem(request, instance, type, [state, finishResolution](Result<InstallPlanEntry> result) {
                if (result) {
                    state->plan.append(*result);
                } else {
                    state->errors.append(result.error());
                }
                if (--state->pending == 0) {
                    finishResolution();
                }
            });
        }
    });
}
}  // namespace

void registerPlatformTools(ToolRegistry& registry)
{
    using namespace Schema;

    const auto loaderSchema = string("Mod loader, e.g. fabric, forge, neoforge, quilt");
    const auto minecraftSchema = string("Minecraft version, e.g. 1.21.1");

    {
        Tool tool;
        tool.name = "search_content";
        tool.title = "Search Modrinth and CurseForge";
        tool.description =
            "Searches Modrinth and CurseForge for mods, modpacks, resource packs, shader packs or data packs, optionally filtered by "
            "Minecraft version and mod loader. Each result carries its platform and projectId, which content_versions and the install "
            "tools take. Both platforms are searched by default; a project listed on both appears once per platform. Pages hold about "
            "25 results per platform; use offset for more.";
        tool.inputSchema = object({
            { "query", string("Search words; empty browses by sort order") },
            { "kind", string("What to search for (default mod)", kindNames()) },
            { "platform", string("Where to search (default both)", { "both", PLATFORM_MODRINTH, PLATFORM_CURSEFORGE }) },
            { "minecraft", minecraftSchema },
            { "loader", loaderSchema },
            { "sort", string("Order (default relevance)", { "relevance", "downloads", "updated", "newest" }) },
            { "offset", integer("Results to skip, per platform", 0, 10000) },
        });
        tool.outputSchema = object({ { "results", array(object({}, {}, false)) },
                                     { "offset", integer({}) },
                                     { "count", integer({}) },
                                     { "notes", array(string({})) } },
                                   { "results", "offset", "count" }, false);
        tool.annotations.openWorld = true;
        tool.scope = Scope::Read;
        tool.alwaysLoad = true;
        tool.handler = searchContent;
        registry.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "content_versions";
        tool.title = "Versions of a project";
        tool.description =
            "Lists the downloadable versions (files) of one Modrinth or CurseForge project, newest first, optionally only those for a "
            "Minecraft version and loader. Each version lists its Minecraft versions, loaders, release type and dependencies "
            "(projectId + required/optional/incompatible).";
        tool.inputSchema = object(
            {
                { "platform", string("Where the project lives", { PLATFORM_MODRINTH, PLATFORM_CURSEFORGE }) },
                { "projectId", string("Project id (Modrinth also accepts the slug)") },
                { "kind", string("Project kind (default mod)", kindNames()) },
                { "minecraft", minecraftSchema },
                { "loader", loaderSchema },
                { "limit", integer("Maximum versions returned (default 20)", 1, MAX_VERSION_LIMIT) },
            },
            { "platform", "projectId" });
        tool.outputSchema = object({ { "platform", string({}) },
                                     { "projectId", string({}) },
                                     { "count", integer("Matching versions, including any beyond the limit") },
                                     { "versions", array(object({}, {}, false)) } },
                                   { "platform", "projectId", "count", "versions" }, false);
        tool.annotations.openWorld = true;
        tool.scope = Scope::Read;
        tool.handler = contentVersions;
        registry.add(std::move(tool));
    }
    {
        Tool tool;
        tool.name = "install_content";
        tool.title = "Install mods and packs";
        tool.description =
            "Installs Modrinth or CurseForge projects into an instance: mods, resource packs, texture packs, shader packs or data "
            "packs. Without versionId the newest release for the instance's Minecraft version (and loader, for mods) is chosen. For "
            "mods, required dependencies are added unless dependencies=none. Files already installed at the same version are "
            "skipped; another version of an installed project replaces it. dryRun=true returns the plan without installing. "
            "Otherwise it returns a job id at once; follow it with jobs action=wait.";
        tool.inputSchema = object(
            {
                { "instance", string("Instance id") },
                { "kind", string("Which folder to install into (default mods)", contentKindNames()) },
                { "items", [] {
                     auto items = array(object(
                                            {
                                                { "platform", string("Where the project lives", { PLATFORM_MODRINTH, PLATFORM_CURSEFORGE }) },
                                                { "projectId", string("Project id from search_content") },
                                                { "versionId", string("A specific versionId from content_versions (optional)") },
                                            },
                                            { "platform", "projectId" }));
                     items["minItems"] = 1;
                     items["maxItems"] = 200;
                     return items;
                 }() },
                { "dependencies", string("Mods: add required dependencies (default required)", { "required", "none" }) },
                { "dryRun", boolean("Only return what would be installed (default false)") },
            },
            { "instance", "items" });
        tool.outputSchema = object({ { "instance", string({}) },
                                     { "job", string("Job id when installing") },
                                     { "dryRun", boolean({}) },
                                     { "plan", array(object({}, {}, false)) },
                                     { "skipped", array(object({}, {}, false)) } },
                                   { "instance", "plan" }, false);
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.annotations.idempotent = true;
        tool.annotations.openWorld = true;
        tool.scope = Scope::Content;
        tool.effect = [](const QJsonObject& arguments) { return arguments.value("dryRun").toBool(false) ? Effect::Read : Effect::Write; };
        tool.describeAction = [](const QJsonObject& arguments) {
            QStringList names;
            for (const auto& item : arguments.value("items").toArray()) {
                names.append(QString("%1:%2").arg(item.toObject().value("platform").toString(), item.toObject().value("projectId").toString()));
            }
            QString target = arguments.value("instance").toString();
            if (auto* instance = APPLICATION->instances()->getInstanceById(target)) {
                target = instance->name();
            }
            return QString("Install into '%1' (with required dependencies): %2").arg(target, names.join(", "));
        };
        tool.handler = installContent;
        registry.add(std::move(tool));
    }
}

}  // namespace Mcp
