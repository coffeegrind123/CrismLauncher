// SPDX-License-Identifier: GPL-3.0-only
#include "McpPortingOps.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <memory>

#include "Application.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "Version.h"
#include "archive/ArchiveReader.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/mod/ResourceFolderModel.h"
#include "modplatform/ModIndex.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "net/NetJob.h"
#include "net/RawHeaderProxy.h"
#include "net/Request.h"

namespace Mcp {

namespace {
constexpr quint32 CLASS_MAGIC = 0xCAFEBABE;
constexpr int TOP_PACKAGES = 40;
constexpr int LISTED_MIXINS = 80;
constexpr auto PRIMER_URL = "https://github.com/neoforged/.github/blob/main/primers/%1/index.md";
constexpr auto CACHE_FOLDER = "cache/mcp-porting";

// The NeoForge primers, oldest first (neoforged/.github, primers/). Newer ones appear upstream; the guide says to check.
const QStringList& knownPrimers()
{
    static const QStringList s_primers{ "1.20",   "1.20.5", "1.20.6", "1.21",    "1.21.1",  "1.21.2", "1.21.4", "1.21.5",
                                        "1.21.6", "1.21.7", "1.21.8", "1.21.9",  "1.21.10", "1.21.11", "26.1",  "26.2",
                                        "26.3" };
    return s_primers;
}

// Packages whose API changed the most between versions: code touching them is where a port costs time
const QStringList& renderPackages()
{
    static const QStringList s_packages{ "com/mojang/blaze3d", "com/mojang/renderpearl", "net/minecraft/client/renderer",
                                         "net/minecraft/client/gui/render" };
    return s_packages;
}

// net/minecraft/client/renderer/chunk/SectionRenderDispatcher -> net/minecraft/client/renderer/chunk
QString packageOf(const QString& className)
{
    const auto slash = className.lastIndexOf('/');
    return slash < 0 ? QString() : className.left(slash);
}

bool isGameClass(const QString& name)
{
    return name.startsWith("net/minecraft/") || name.startsWith("com/mojang/") || name.startsWith("net/neoforged/") ||
           name.startsWith("net/minecraftforge/") || name.startsWith("net/fabricmc/") || name.startsWith("org/quiltmc/");
}

QString firstMatch(const QString& text, const QRegularExpression& pattern)
{
    const auto match = pattern.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}
}  // namespace

Result<ClassReferences> readClassReferences(const QByteArray& classFile)
{
    const auto* data = reinterpret_cast<const uchar*>(classFile.constData());
    const qsizetype size = classFile.size();
    if (size < 10 || qFromBigEndian<quint32>(data) != CLASS_MAGIC) {
        return std::unexpected(QString("not a class file"));
    }

    ClassReferences out;
    out.majorVersion = qFromBigEndian<quint16>(data + 6);
    const int count = qFromBigEndian<quint16>(data + 8);

    QHash<int, QString> utf8;
    QList<int> classNameIndexes;
    qsizetype at = 10;
    auto need = [&](qsizetype bytes) { return at + bytes <= size; };

    for (int index = 1; index < count; ++index) {
        if (!need(1)) {
            return std::unexpected(QString("truncated constant pool"));
        }
        const int tag = data[at++];
        switch (tag) {
            case 1: {  // Utf8 (modified UTF-8; plain UTF-8 decoding is right for class names)
                if (!need(2)) {
                    return std::unexpected(QString("truncated constant pool"));
                }
                const int length = qFromBigEndian<quint16>(data + at);
                at += 2;
                if (!need(length)) {
                    return std::unexpected(QString("truncated constant pool"));
                }
                utf8.insert(index, QString::fromUtf8(reinterpret_cast<const char*>(data + at), length));
                at += length;
                break;
            }
            case 7:  // Class
                if (!need(2)) {
                    return std::unexpected(QString("truncated constant pool"));
                }
                classNameIndexes.append(qFromBigEndian<quint16>(data + at));
                at += 2;
                break;
            case 8:   // String
            case 16:  // MethodType
            case 19:  // Module
            case 20:  // Package
                at += 2;
                break;
            case 15:  // MethodHandle
                at += 3;
                break;
            case 3:   // Integer
            case 4:   // Float
            case 9:   // Fieldref
            case 10:  // Methodref
            case 11:  // InterfaceMethodref
            case 12:  // NameAndType
            case 17:  // Dynamic
            case 18:  // InvokeDynamic
                at += 4;
                break;
            case 5:  // Long and Double take two pool slots
            case 6:
                at += 8;
                ++index;
                break;
            default:
                return std::unexpected(QString("unknown constant pool tag %1").arg(tag));
        }
    }

    for (const auto nameIndex : classNameIndexes) {
        auto name = utf8.value(nameIndex);
        // Array classes: "[[Lnet/minecraft/core/BlockPos;" -> net/minecraft/core/BlockPos
        if (name.startsWith('[')) {
            const auto start = name.indexOf('L');
            if (start < 0) {
                continue;
            }
            name = name.mid(start + 1);
            name.chop(1);
        }
        out.classes.insert(name);
    }
    // Types used only in field and method descriptors are not Class constants; catch them in the descriptors
    static const QRegularExpression s_descriptorType(R"(L((?:net/minecraft|com/mojang|net/neoforged|net/fabricmc)/[\w/$]+);)");
    for (const auto& text : std::as_const(utf8)) {
        if (!text.contains(';')) {
            continue;
        }
        auto it = s_descriptorType.globalMatch(text);
        while (it.hasNext()) {
            out.classes.insert(it.next().captured(1));
        }
    }
    return out;
}

int javaRelease(int classMajorVersion)
{
    return classMajorVersion >= 45 ? classMajorVersion - 44 : 0;
}

QStringList primersBetween(const QString& fromVersion, const QString& toVersion)
{
    const Version from(fromVersion);
    const Version to(toVersion);
    const auto& low = from < to ? from : to;
    const auto& high = from < to ? to : from;
    QStringList out;
    for (const auto& primer : knownPrimers()) {
        const Version version(primer);
        if (low < version && (version < high || version == high)) {
            out.append(QString(PRIMER_URL).arg(primer));
        }
    }
    return out;
}

Result<QJsonObject> analyzeJar(const QString& path)
{
    MMCZip::ArchiveReader zip(path);

    QJsonObject metadata;
    QJsonArray mixinConfigs;
    QStringList mixinClasses;
    QHash<QString, int> packageUse;  // package -> number of the mod's classes that use it
    QHash<QString, int> assetKinds;
    int classCount = 0;
    int renderClasses = 0;
    int maxMajor = 0;

    auto parsed = zip.parse([&](MMCZip::ArchiveReader::File* file) -> Result<> {
        const auto name = file->filename();
        if (name.endsWith(".class")) {
            TRY_INTO(const auto bytes, file->readAll())
            auto references = readClassReferences(bytes);
            if (!references) {
                return {};  // a damaged or unusual class should not stop the report
            }
            ++classCount;
            maxMajor = std::max(maxMajor, references->majorVersion);
            QSet<QString> packages;
            bool touchesRender = false;
            for (const auto& reference : std::as_const(references->classes)) {
                if (!isGameClass(reference)) {
                    continue;
                }
                packages.insert(packageOf(reference));
                touchesRender = touchesRender || std::any_of(renderPackages().begin(), renderPackages().end(),
                                                             [&reference](const QString& prefix) { return reference.startsWith(prefix); });
            }
            for (const auto& package : std::as_const(packages)) {
                packageUse[package]++;
            }
            renderClasses += touchesRender ? 1 : 0;
            return {};
        }
        if (name == "META-INF/neoforge.mods.toml" || name == "META-INF/mods.toml") {
            TRY_INTO(const auto bytes, file->readAll())
            const auto text = QString::fromUtf8(bytes);
            metadata["loader"] = name.contains("neoforge") ? "neoforge" : "forge";
            metadata["modId"] = firstMatch(text, QRegularExpression(R"re(modId\s*=\s*"([^"]+)")re"));
            metadata["version"] = firstMatch(text, QRegularExpression(R"re(\n\s*version\s*=\s*"([^"]+)")re"));
            metadata["license"] = firstMatch(text, QRegularExpression(R"re(license\s*=\s*"([^"]*)")re"));
            metadata["minecraft"] = firstMatch(text, QRegularExpression(R"re(modId\s*=\s*"minecraft"[\s\S]*?versionRange\s*=\s*"([^"]*)")re"));
            return {};
        }
        if (name == "fabric.mod.json" || name == "quilt.mod.json") {
            TRY_INTO(const auto bytes, file->readAll())
            const auto json = QJsonDocument::fromJson(bytes).object();
            metadata["loader"] = name.startsWith("quilt") ? "quilt" : "fabric";
            metadata["modId"] = json.value("id").toString();
            metadata["version"] = json.value("version").toString();
            const auto license = json.value("license");
            metadata["license"] = license.isArray() ? license.toArray().first().toString() : license.toString();
            metadata["minecraft"] = json.value("depends").toObject().value("minecraft").toVariant().toString();
            return {};
        }
        if (name.endsWith("mixins.json")) {
            TRY_INTO(const auto bytes, file->readAll())
            const auto json = QJsonDocument::fromJson(bytes).object();
            mixinConfigs.append(name);
            for (const auto* side : { "mixins", "client", "server" }) {
                for (const auto& value : json.value(side).toArray()) {
                    mixinClasses.append(value.toString());
                }
            }
            return {};
        }
        if (name.contains("/shaders/") || name.endsWith(".glsl") || name.endsWith(".spv")) {
            assetKinds[QFileInfo(name).suffix()]++;
        }
        return file->skip();
    });
    if (!parsed) {
        return std::unexpected(QString("Could not read %1: %2").arg(QFileInfo(path).fileName(), parsed.error()));
    }

    QList<std::pair<QString, int>> ranked;
    for (auto it = packageUse.cbegin(); it != packageUse.cend(); ++it) {
        ranked.emplace_back(it.key(), it.value());
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
    QJsonArray packages;
    for (qsizetype i = 0; i < ranked.size() && i < TOP_PACKAGES; ++i) {
        packages.append(QJsonObject{ { "package", QString(ranked[i].first).replace('/', '.') }, { "classes", ranked[i].second } });
    }
    QJsonObject shaders;
    for (auto it = assetKinds.cbegin(); it != assetKinds.cend(); ++it) {
        shaders[it.key()] = it.value();
    }

    const double renderShare = classCount > 0 ? static_cast<double>(renderClasses) / classCount : 0;
    QString effort = "small: mostly logic and data, which ports with the primers' renames";
    if (renderShare > 0.4 || mixinClasses.size() > 30) {
        effort = "large: much of the mod touches rendering or patches the game with mixins; expect rewrites, not renames";
    } else if (renderShare > 0.15 || mixinClasses.size() > 10) {
        effort = "medium: a fair share touches rendering or mixins, whose targets change between versions";
    }

    QJsonObject result{
        { "file", QFileInfo(path).fileName() },
        { "metadata", metadata },
        { "javaRelease", javaRelease(maxMajor) },
        { "classes", classCount },
        { "classesTouchingRendering", renderClasses },
        { "mixins", QJsonObject{ { "configs", mixinConfigs },
                                 { "count", static_cast<qint64>(mixinClasses.size()) },
                                 { "classes", QJsonArray::fromStringList(mixinClasses.mid(0, LISTED_MIXINS)) } } },
        { "packagesUsed", packages },
        { "shaderAssets", shaders },
        { "effort", effort },
    };
    return result;
}

namespace {
QString instanceLabel(const QJsonObject& arguments)
{
    const auto id = arguments.value("instance").toString();
    auto* instance = APPLICATION->instances()->getInstanceById(id);
    return instance ? instance->name() : id;
}

// Fills in what only the mod platform knows, then replies
void finishAnalysis(QJsonObject report, const QString& sourceUrl, const QString& targetMinecraft, const ToolReply& reply)
{
    if (!sourceUrl.isEmpty()) {
        report["sourceUrl"] = sourceUrl;
    }
    report["sourcePublished"] = !sourceUrl.isEmpty();
    // A Minecraft range such as "[26.3]" or ">=1.21.1": its first version number is a fair starting point
    static const QRegularExpression s_firstVersion(R"((\d+\.\d+(?:\.\d+)?))");
    const auto from = firstMatch(report.value("metadata").toObject().value("minecraft").toString(), s_firstVersion);
    if (!targetMinecraft.isEmpty() && !from.isEmpty()) {
        report["primers"] = QJsonArray::fromStringList(primersBetween(from, targetMinecraft));
        report["direction"] = Version(targetMinecraft) < Version(from) ? "backport" : "forward port";
    }
    report["terms"] =
        "Personal use only: the port stays private (a private repository used only for CI builds, nothing published), the jar "
        "goes only into the user's own instance, and the author's official build replaces it when one exists.";
    report["guide"] = "launcher://guide/mod-porting";
    reply(ToolResult::ok(report));
}

void analyzeOp(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto target = arguments.value("targetMinecraft").toString();

    // A jar already in an instance
    if (arguments.contains("instance")) {
        auto found = findInstance(arguments.value("instance").toString());
        if (!found) {
            reply(ToolResult::error(found.error()));
            return;
        }
        auto* model = contentModel(*found, *contentKindFromName(arguments.value("kind").toString("mods")));
        const auto path = model->dir().absoluteFilePath(arguments.value("file").toString());
        if (arguments.value("file").toString().contains('/') || !QFileInfo(path).isFile()) {
            reply(ToolResult::error("No such file in that folder.", "content.list gives the file names."));
            return;
        }
        auto report = analyzeJar(path);
        if (!report) {
            reply(ToolResult::error(report.error()));
            return;
        }
        finishAnalysis(*report, {}, target, reply);
        return;
    }

    // A file from Modrinth or CurseForge: download it into the cache, then analyse
    const auto platform = arguments.value("platform").toString();
    const auto projectId = arguments.value("projectId").toString();
    const auto versionId = arguments.value("versionId").toString();
    if (platform.isEmpty() || projectId.isEmpty()) {
        reply(ToolResult::error("Pass instance + file, or platform + projectId (+ versionId)."));
        return;
    }
    const bool curseForge = platform == "curseforge";
    const ResourceAPI& api = curseForge ? static_cast<const ResourceAPI&>(FlameAPI::get()) : static_cast<const ResourceAPI&>(ModrinthAPI::get());
    auto pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = projectId;
    pack->provider = curseForge ? ModPlatform::ResourceProvider::FLAME : ModPlatform::ResourceProvider::MODRINTH;

    // Project first (it carries the source link), then the file
    auto [projectJob, projectResponse] = api.getProjectTask(projectId, true, false);
    runTask(projectJob, [projectResponse = projectResponse, pack, versionId, target, reply, curseForge](bool ok, const QString& error) {
        if (!ok) {
            reply(ToolResult::error(QString("Could not load the project: %1").arg(error)));
            return;
        }
        const auto sourceUrl = projectResponse->extraData.sourceUrl;
        pack->name = projectResponse->name;

        auto keep = std::make_shared<Task::Ptr>();
        ResourceAPI::Callback<QVector<ModPlatform::IndexedVersion>> callbacks;
        callbacks.onSucceed = [pack, versionId, sourceUrl, target, reply, keep](QVector<ModPlatform::IndexedVersion>& versions) {
            keep->reset();
            const auto chosen = std::find_if(versions.begin(), versions.end(), [&versionId](const ModPlatform::IndexedVersion& v) {
                return versionId.isEmpty() || v.fileId.toString() == versionId;
            });
            if (chosen == versions.end() || chosen->downloadUrl.isEmpty()) {
                reply(ToolResult::error("No downloadable file for that project/version.", "content_versions lists the versionIds."));
                return;
            }
            const auto folder = FS::PathCombine(APPLICATION->dataRoot(), CACHE_FOLDER);
            FS::ensureFolderPathExists(folder);
            const auto path = FS::PathCombine(folder, chosen->fileName);
            auto download = makeShared<NetJob>(QString("Porting analysis download"), APPLICATION->network());
            download->setAskRetry(false);
            download->addNetAction(Net::Request::makeFile(QUrl(chosen->downloadUrl), path));
            runTask(download, [path, sourceUrl, target, reply](bool downloaded, const QString& reason) {
                if (!downloaded) {
                    reply(ToolResult::error(QString("Download failed: %1").arg(reason)));
                    return;
                }
                auto report = analyzeJar(path);
                if (!report) {
                    reply(ToolResult::error(report.error()));
                    return;
                }
                report->insert("cachedJar", path);
                finishAnalysis(*report, sourceUrl, target, reply);
            });
        };
        callbacks.onFail = [reply, keep](const QString& reason, int) {
            keep->reset();
            reply(ToolResult::error(QString("Could not list versions: %1").arg(reason)));
        };
        callbacks.onAbort = [reply, keep] {
            keep->reset();
            reply(ToolResult::error("Cancelled"));
        };
        const auto& versionApi = curseForge ? static_cast<const ResourceAPI&>(FlameAPI::get()) : static_cast<const ResourceAPI&>(ModrinthAPI::get());
        auto task = versionApi.getProjectVersions({ .pack = pack, .mcVersions = {}, .loaders = {}, .resourceType = ModPlatform::ResourceType::Mod },
                                                  callbacks);
        if (!task) {
            reply(ToolResult::error("This platform has no version listing for that project."));
            return;
        }
        if (auto* netJob = dynamic_cast<NetJob*>(task.get())) {
            netJob->setAskRetry(false);
        }
        *keep = task;
        task->start();
    });
}

void installUrlOp(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    const QUrl url(arguments.value("url").toString(), QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http")) {
        reply(ToolResult::error("url must be an http(s) link to the file."));
        return;
    }
    auto fileName = arguments.value("fileName").toString(url.fileName());
    fileName = FS::RemoveInvalidFilenameChars(fileName);
    if (fileName.isEmpty()) {
        reply(ToolResult::error("Could not tell the file name from the URL; pass fileName."));
        return;
    }
    auto* model = contentModel(instance, *contentKindFromName(arguments.value("kind").toString("mods")));
    const auto path = model->dir().absoluteFilePath(fileName);
    auto* registry = jobs();
    if (!registry) {
        reply(ToolResult::error("Jobs are not available."));
        return;
    }

    auto download = makeShared<NetJob>(QString("Install from URL"), APPLICATION->network());
    download->setAskRetry(false);
    auto request = Net::Request::makeFile(url, path);
    // A bearer token for private release assets (e.g. a GitHub token with read access); never logged or returned
    if (const auto token = arguments.value("authorization").toString(); !token.isEmpty()) {
        request->addHeaderProxy(std::make_unique<Net::RawHeaderProxy>(QList<Net::HeaderPair>{ { "Authorization", token.toUtf8() },
                                                                                                { "Accept", "application/octet-stream" } }));
    }
    download->addNetAction(request);
    const auto id = registry->start(context.token.id, context.token.name, QString("Download %1 into '%2'").arg(fileName, instance->name()), download,
                                    [model, path] {
                                        model->update();
                                        return QJsonObject{ { "file", QFileInfo(path).fileName() }, { "path", path } };
                                    });
    auto result = ToolResult::ok(QJsonObject{ { "job", id }, { "file", fileName } },
                                 QString("Downloading as job %1; follow it with jobs action=wait.").arg(id));
    result.jobId = id;
    reply(result);
}
}  // namespace

void registerPortingOps(ToolRegistry& ops)
{
    using namespace Schema;

    {
        Tool tool;
        tool.name = "porting.analyze";
        tool.description =
            "Measures how hard a mod is to port to another Minecraft version or loader, for the user's own play only (read "
            "launcher://guide/mod-porting first; its terms apply). Give a mod by platform + projectId (+ versionId) or by "
            "instance + file. Reports its loader, Minecraft range, licence, Java release, whether source is published, mixin "
            "configs and targets, the game packages its classes use (counted), how many classes touch rendering, shader assets, "
            "a rough effort rating, and with targetMinecraft the NeoForge porting primers between the two versions.";
        tool.inputSchema = object({
            { "platform", string("Where the mod lives", { "modrinth", "curseforge" }) },
            { "projectId", string("Project id") },
            { "versionId", string("A specific file (default the newest)") },
            { "instance", string("Or: an instance id holding the jar") },
            { "kind", string("With instance: which folder (default mods)", contentKindNames()) },
            { "file", string("With instance: the jar's file name") },
            { "targetMinecraft", string("The version to port to, e.g. 1.21.1") },
        });
        tool.scope = Scope::Read;
        tool.annotations.openWorld = true;
        tool.handler = analyzeOp;
        ops.add(tool);
    }

    {
        Tool tool;
        tool.name = "content.install_url";
        tool.description =
            "Downloads a file from an http(s) URL into an instance's mods (or other content) folder, as a job: for example a jar "
            "built by a private CI run. `authorization` (e.g. \"Bearer <token>\") is sent with the request and never stored "
            "or returned. Asks the user first under the default policy, since the file can be any code.";
        tool.inputSchema = object(
            {
                { "instance", string("Instance id") },
                { "url", string("http(s) URL of the file") },
                { "kind", string("Which folder (default mods)", contentKindNames()) },
                { "fileName", string("File name to save as (default from the URL)") },
                { "authorization", string("Authorization header value for private downloads") },
            },
            { "instance", "url" });
        tool.scope = Scope::Content;
        tool.annotations.readOnly = false;
        tool.annotations.destructive = true;
        tool.annotations.openWorld = true;
        tool.describeAction = [](const QJsonObject& a) {
            return QString("Download into '%1' (%2): %3")
                .arg(instanceLabel(a), a.value("kind").toString("mods"), QUrl(a.value("url").toString()).toDisplayString(QUrl::RemoveQuery));
        };
        tool.handler = installUrlOp;
        ops.add(tool);
    }
}

}  // namespace Mcp
