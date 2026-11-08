// SPDX-License-Identifier: GPL-3.0-only
#include "McpExportOps.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QStandardPaths>

#include <memory>

#include "Application.h"
#include "FileIgnoreProxy.h"
#include "FileSystem.h"
#include "InstanceCopyPrefs.h"
#include "InstanceCopyTask.h"
#include "InstanceList.h"
#include "MMCZip.h"
#include "archive/ExportToZipTask.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "mcp/McpLauncherTools.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/mod/ResourceFolderModel.h"
#include "modplatform/flame/FlamePackExportTask.h"
#include "modplatform/modrinth/ModrinthPackExportTask.h"
#include "settings/SettingsObject.h"

// Defined in ui/dialogs/ExportInstanceDialog.cpp: copies the instance's icon into its folder so exports carry it
void SaveIcon(BaseInstance* instance);

namespace Mcp {

namespace {
constexpr auto IGNORE_FILE = ".packignore";

QString instanceLabel(const QJsonObject& arguments)
{
    const auto id = arguments.value("instance").toString();
    auto* instance = APPLICATION->instances()->getInstanceById(id);
    return instance ? instance->name() : id;
}

// The same exclusions the export dialogs start from, including the user's saved .packignore choices
std::shared_ptr<FileIgnoreProxy> exportFilter(MinecraftInstance* instance, bool platformPack)
{
    auto proxy = std::make_shared<FileIgnoreProxy>(instance->instanceRoot(), nullptr);
    const auto prefix = QDir(instance->instanceRoot()).relativeFilePath(instance->gameRoot());
    for (const auto* path : { "logs", "crash-reports", ".cache", ".fabric", ".quilt" }) {
        proxy->ignoreFilesWithPath().insert(FS::PathCombine(prefix, path));
    }
    proxy->ignoreFilesWithName().append({ ".DS_Store", "thumbs.db", "Thumbs.db" });
    if (platformPack) {
        proxy->ignoreFilesWithSuffix().append(".pw.toml");
        const QDir root(instance->instanceRoot());
        for (auto* model : instance->resourceLists()) {
            if (model && model->indexDir().exists() && model->dir() != model->indexDir()) {
                proxy->ignoreFilesWithPath().insert(root.relativeFilePath(model->indexDir().absolutePath()));
            }
        }
    }
    proxy->loadBlockedPathsFromFile(FS::PathCombine(instance->instanceRoot(), IGNORE_FILE));
    return proxy;
}

QString exportFolder()
{
    auto folder = APPLICATION->settings()->get("DownloadsDir").toString();
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    return folder;
}

void exportOp(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    auto* registry = jobs();
    if (!registry) {
        reply(ToolResult::error("Jobs are not available."));
        return;
    }
    const auto format = arguments.value("format").toString("zip");
    const auto extension = format == "mrpack" ? QString(".mrpack") : QString(".zip");
    auto fileName = arguments.value("fileName").toString(instance->name() + extension);
    if (!fileName.endsWith(extension)) {
        fileName += extension;
    }
    // Only a name: exports always land in the downloads folder, so an agent cannot overwrite files elsewhere
    fileName = FS::RemoveInvalidFilenameChars(fileName);
    const auto output = FS::PathCombine(exportFolder(), fileName);

    const auto name = arguments.value("name").toString(instance->name());
    const auto version = arguments.value("version").toString(instance->settings()->get("ExportVersion").toString());
    const bool optional = arguments.value("optionalFiles").toBool(true);
    auto filter = exportFilter(instance, format != "zip");
    auto filterFunction = [filter](const QFileInfo& file) { return filter->filterFile(file); };

    Task::Ptr task;
    if (format == "zip") {
        SaveIcon(instance);
        QFileInfoList files;
        if (!MMCZip::collectFileListRecursively(instance->instanceRoot(), nullptr, &files, filterFunction)) {
            reply(ToolResult::error("Could not list the instance's files."));
            return;
        }
        task = makeShared<MMCZip::ExportToZipTask>(output, instance->instanceRoot(), files, "", true);
    } else if (format == "mrpack") {
        task = Task::Ptr(new ModrinthPackExportTask(name, version, arguments.value("summary").toString(), optional, instance, output,
                                                    filterFunction));
    } else {
        FlamePackExportOptions options;
        options.name = name;
        options.version = version;
        options.author = arguments.value("author").toString(instance->settings()->get("ExportAuthor").toString());
        options.optionalFiles = optional;
        options.instance = instance;
        options.output = output;
        options.filter = filterFunction;
        options.recommendedRAM = arguments.value("recommendedRamMB").toInt(instance->settings()->get("ExportRecommendedRAM").toInt());
        task = Task::Ptr(new FlamePackExportTask(std::move(options)));
    }

    const auto id = registry->start(context.token.id, context.token.name, QString("Export '%1' as %2").arg(instance->name(), format), task,
                                    [output] { return QJsonObject{ { "file", output } }; });
    auto result = ToolResult::ok(QJsonObject{ { "job", id }, { "file", output } },
                                 QString("Exporting to %1 as job %2; follow it with jobs action=wait.").arg(output, id));
    result.jobId = id;
    reply(result);
}

void copyOp(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return;
    }
    auto* instance = *found;
    auto* registry = jobs();
    if (!registry) {
        reply(ToolResult::error("Jobs are not available."));
        return;
    }

    InstanceCopyPrefs prefs;
    prefs.enableCopySaves(arguments.value("copyWorlds").toBool(true));
    prefs.enableKeepPlaytime(arguments.value("keepPlaytime").toBool(false));
    prefs.enableCopyGameOptions(true);
    prefs.enableCopyResourcePacks(true);
    prefs.enableCopyShaderPacks(true);
    prefs.enableCopyServers(true);
    prefs.enableCopyMods(true);
    prefs.enableCopyScreenshots(arguments.value("copyScreenshots").toBool(false));

    auto* copy = new InstanceCopyTask(instance, prefs);
    copy->setName(arguments.value("name").toString(QString("%1 (copy)").arg(instance->name())));
    copy->setGroup(arguments.value("group").toString(APPLICATION->instances()->getInstanceGroup(instance->id())));
    copy->setIcon(instance->iconKey());

    Task::Ptr wrapped(APPLICATION->instances()->wrapInstanceTask(copy));
    auto newId = std::make_shared<QString>();
    QObject::connect(APPLICATION->instances(), &InstanceList::instanceSelectRequest, wrapped.get(), [newId](const QString& id) {
        if (newId->isEmpty()) {
            *newId = id;
        }
    });
    const auto id = registry->start(context.token.id, context.token.name, QString("Copy instance '%1'").arg(instance->name()), wrapped, [newId] {
        if (auto* created = APPLICATION->instances()->getInstanceById(*newId)) {
            return QJsonObject{ { "instance", instanceSummary(created) } };
        }
        return QJsonObject{ { "instanceId", *newId } };
    });
    auto result = ToolResult::ok(QJsonObject{ { "job", id } }, QString("Copying as job %1; its result holds the new instance.").arg(id));
    result.jobId = id;
    reply(result);
}
}  // namespace

void registerExportOps(ToolRegistry& ops)
{
    using namespace Schema;

    {
        Tool tool;
        tool.name = "instance.export";
        tool.description =
            "Exports an instance as a job: format=zip (a full Prism/MultiMC instance archive), mrpack (a Modrinth modpack) or "
            "curseforge (a CurseForge modpack). Mod-platform files are referenced rather than bundled in mrpack/curseforge. The file "
            "goes to the user's downloads folder under fileName (default: the instance name). Logs, crash reports and caches are "
            "left out, as are paths the user excluded when exporting before.";
        tool.inputSchema = object(
            {
                { "instance", string("Instance id") },
                { "format", string("Archive type (default zip)", { "zip", "mrpack", "curseforge" }) },
                { "fileName", string("File name only, no folders") },
                { "name", string("Pack name (mrpack/curseforge; default the instance name)") },
                { "version", string("Pack version (mrpack/curseforge)") },
                { "summary", string("mrpack: description") },
                { "author", string("curseforge: author") },
                { "optionalFiles", boolean("Mark disabled mods as optional (default true)") },
                { "recommendedRamMB", integer("curseforge: recommended memory", 512, 65536) },
            },
            { "instance" });
        tool.scope = Scope::Instances;
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.describeAction = [](const QJsonObject& a) {
            return QString("Export '%1' as %2 to the downloads folder").arg(instanceLabel(a), a.value("format").toString("zip"));
        };
        tool.handler = exportOp;
        ops.add(tool);
    }

    {
        Tool tool;
        tool.name = "instance.copy";
        tool.description =
            "Copies an instance as a job: mods, packs, options and servers, plus worlds (copyWorlds, default true) and screenshots "
            "(copyScreenshots, default false). The job's result holds the new instance.";
        tool.inputSchema = object(
            {
                { "instance", string("Instance id") },
                { "name", string("Name of the copy (default '<name> (copy)')") },
                { "group", string("Group of the copy (default the same group)") },
                { "copyWorlds", boolean("Copy worlds (default true)") },
                { "copyScreenshots", boolean("Copy screenshots (default false)") },
                { "keepPlaytime", boolean("Keep the play time (default false)") },
            },
            { "instance" });
        tool.scope = Scope::Instances;
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.annotations.idempotent = false;
        tool.describeAction = [](const QJsonObject& a) { return QString("Copy instance '%1'").arg(instanceLabel(a)); };
        tool.handler = copyOp;
        ops.add(tool);
    }
}

}  // namespace Mcp
