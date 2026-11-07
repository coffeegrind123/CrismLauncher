// SPDX-License-Identifier: GPL-3.0-only
#include "McpJavaOps.h"

#include <QHash>
#include <QJsonArray>

#include <memory>

#include "Application.h"
#include "BuildConfig.h"
#include "FileSystem.h"
#include "java/JavaMetadata.h"
#include "java/download/ArchiveDownloadTask.h"
#include "java/download/ManifestDownloadTask.h"
#include "java/download/SymlinkTask.h"
#include "mcp/McpCommon.h"
#include "mcp/McpJobs.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "meta/Index.h"
#include "meta/Version.h"
#include "meta/VersionList.h"
#include "tasks/SequentialTask.h"
#include "ui/java/VersionList.h"

namespace Mcp {

namespace {
// The vendors the Install Java dialog offers, by the names tools use
const QHash<QString, QString>& vendorUids()
{
    static const QHash<QString, QString> s_uids{
        { "mojang", "net.minecraft.java" },
        { "adoptium", "net.adoptium.java" },
        { "azul", "com.azul.java" },
        { "ibm", "com.ibm.java" },
    };
    return s_uids;
}

void withList(const QString& uid, const ToolReply& reply, std::function<void(const Meta::VersionList::Ptr&)> ready)
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
    runTask(list->getLoadTask(), [list, reply, ready](bool ok, const QString& error) {
        if (!ok) {
            reply(ToolResult::error(QString("Could not load the Java versions: %1").arg(error)));
            return;
        }
        ready(list);
    });
}

void javaAvailable(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto vendor = arguments.value("vendor").toString("mojang");
    withList(vendorUids().value(vendor), reply, [vendor, reply](const Meta::VersionList::Ptr& list) {
        QJsonArray majors;
        for (const auto& version : list->versions()) {
            if (version->version().startsWith("java")) {
                majors.append(version->version().mid(4).toInt());
            }
        }
        reply(ToolResult::ok(QJsonObject{ { "vendor", vendor }, { "majors", majors } }));
    });
}

void javaDownload(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    if (!BuildConfig.JAVA_DOWNLOADER_ENABLED) {
        reply(ToolResult::error("This build of the launcher cannot download Java."));
        return;
    }
    const auto vendor = arguments.value("vendor").toString("mojang");
    const auto major = arguments.value("major").toInt();
    const auto uid = vendorUids().value(vendor);

    withList(uid, reply, [context, vendor, major, uid, reply](const Meta::VersionList::Ptr& list) {
        auto version = list->getVersion(QString("java%1").arg(major));
        if (!version || !list->hasVersion(QString("java%1").arg(major))) {
            reply(ToolResult::error(QString("%1 has no Java %2.").arg(vendor).arg(major), "java.available lists the majors."));
            return;
        }
        // The runtimes for this OS and architecture, newest first, as the Install Java dialog lists them
        auto runtimes = std::make_shared<Java::VersionList>(version);
        runTask(runtimes->getLoadTask(), [context, vendor, major, runtimes, reply](bool ok, const QString& error) {
            if (!ok) {
                reply(ToolResult::error(QString("Could not load Java %1 builds: %2").arg(major).arg(error)));
                return;
            }
            runtimes->sortVersions();
            if (runtimes->count() == 0) {
                reply(ToolResult::error(QString("%1 has no Java %2 build for this system.").arg(vendor).arg(major)));
                return;
            }
            auto meta = std::dynamic_pointer_cast<Java::Metadata>(runtimes->at(0));
            if (!meta) {
                reply(ToolResult::error("Unexpected Java metadata."));
                return;
            }
            const auto finalPath = FS::PathCombine(APPLICATION->javaPath(), meta->m_name);
            Task::Ptr task;
            switch (meta->downloadType) {
                case Java::DownloadType::Manifest:
                    task = makeShared<Java::ManifestDownloadTask>(meta->url, finalPath, meta->checksumType, meta->checksumHash);
                    break;
                case Java::DownloadType::Archive:
                    task = makeShared<Java::ArchiveDownloadTask>(meta->url, finalPath, meta->checksumType, meta->checksumHash);
                    break;
                case Java::DownloadType::Unknown:
                    reply(ToolResult::error("Could not determine how to download that Java build."));
                    return;
            }
#if defined(Q_OS_MACOS)
            auto sequence = makeShared<SequentialTask>(QObject::tr("Install Java"));
            sequence->addTask(task);
            sequence->addTask(makeShared<Java::SymlinkTask>(finalPath));
            task = sequence;
#endif
            // A failed or cancelled download leaves a partial folder behind; remove it as the dialog does
            QObject::connect(task.get(), &Task::failed, task.get(), [finalPath] { FS::deletePath(finalPath); });
            QObject::connect(task.get(), &Task::aborted, task.get(), [finalPath] { FS::deletePath(finalPath); });

            auto* registry = jobs();
            if (!registry) {
                reply(ToolResult::error("Jobs are not available."));
                return;
            }
            const auto id = registry->start(context.token.id, context.token.name, QString("Download %1 Java %2").arg(vendor).arg(major), task,
                                            [finalPath] {
                                                // Rescan so java.list_installs and automatic selection see the new runtime
                                                runTask(APPLICATION->javalist()->getLoadTask(true), [](bool, const QString&) {});
                                                return QJsonObject{ { "path", finalPath } };
                                            });
            reply(ToolResult::ok(QJsonObject{ { "job", id }, { "build", meta->m_name }, { "path", finalPath } },
                                 QString("Downloading %1 as job %2; follow it with jobs action=wait.").arg(meta->m_name, id)));
        });
    });
}
}  // namespace

void registerJavaOps(ToolRegistry& ops)
{
    using namespace Schema;
    const auto vendorArg = string("Java vendor (default mojang)", { "mojang", "adoptium", "azul", "ibm" });

    {
        Tool tool;
        tool.name = "java.available";
        tool.description = "Lists the Java major versions a vendor offers for download through the launcher.";
        tool.inputSchema = object({ { "vendor", vendorArg } });
        tool.scope = Scope::Read;
        tool.annotations.openWorld = true;
        tool.handler = javaAvailable;
        ops.add(tool);
    }

    {
        Tool tool;
        tool.name = "java.download";
        tool.description =
            "Downloads and installs the newest build of a Java major version for this system into the launcher's Java folder, as a "
            "job, like Settings > Java > Install. Afterwards java.list_installs includes it, and automatic Java selection can use it.";
        tool.inputSchema = object({ { "vendor", vendorArg }, { "major", integer("Java major version, e.g. 21", 8, 99) } }, { "major" });
        tool.scope = Scope::Settings;
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.annotations.openWorld = true;
        tool.describeAction = [](const QJsonObject& a) {
            return QString("Download and install %1 Java %2").arg(a.value("vendor").toString("mojang")).arg(a.value("major").toInt());
        };
        tool.handler = javaDownload;
        ops.add(tool);
    }
}

}  // namespace Mcp
