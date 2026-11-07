// SPDX-License-Identifier: GPL-3.0-only
#include "McpPlatformTools.h"

#include <QJsonArray>
#include <QHash>
#include <QTimer>

#include <memory>
#include <utility>

#include "Application.h"
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
        NetJob::Ptr held = *keepAlive;
        keepAlive->reset();
        done(std::move(result));
        QTimer::singleShot(0, [held] { (void)held; });
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
        QTimer::singleShot(0, [held = std::exchange(*keepAlive, Task::Ptr())] { (void)held; });
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
    *keepAlive = task;
    task->start();
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
}

}  // namespace Mcp
