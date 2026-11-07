// SPDX-License-Identifier: GPL-3.0-only
#include "McpFileOps.h"

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QMainWindow>
#include <QPixmap>
#include <QStringDecoder>

#include <algorithm>

#include "Application.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "mcp/McpCommon.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"

namespace Mcp {

Result<QString> resolveInsideRoot(const QString& rootPath, const QString& relative)
{
    const auto root = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.startsWith('~') || relative.contains(QChar(0))) {
        return std::unexpected(QString("'%1' must be a path relative to the instance folder").arg(relative));
    }
    const auto path = QDir::cleanPath(root + '/' + relative);
    if (path != root && !path.startsWith(root + '/')) {
        return std::unexpected(QString("'%1' is outside the instance folder").arg(relative));
    }

    // The deepest existing ancestor must also be inside once symlinks are followed
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    auto probe = path;
    while (!QFileInfo::exists(probe) && probe != root) {
        probe = QFileInfo(probe).absolutePath();
    }
    const auto canonical = QFileInfo(probe).canonicalFilePath();
    if (canonical != canonicalRoot && !canonical.startsWith(canonicalRoot + '/')) {
        return std::unexpected(QString("'%1' leads outside the instance folder through a link").arg(relative));
    }
    return path;
}

namespace {
constexpr qint64 DEFAULT_READ_BYTES = 256 * 1024;
constexpr qint64 MAX_READ_BYTES = 4 * 1024 * 1024;
constexpr qint64 MAX_WRITE_BYTES = 8 * 1024 * 1024;
constexpr int MAX_LIST_ENTRIES = 2000;
constexpr int MAX_IMAGE_EDGE = 1600;
constexpr auto SCREENSHOTS_FOLDER = "screenshots";

// Files that configure how the instance launches; changing them would bypass the settings and components protections
bool isProtectedFile(const QString& relative)
{
    const auto clean = QDir::cleanPath(relative);
    return clean == "instance.cfg" || clean == "mmc-pack.json" || clean == "patches" || clean.startsWith("patches/");
}

Result<QString> resolveInside(MinecraftInstance* instance, const QString& relative)
{
    return resolveInsideRoot(instance->instanceRoot(), relative);
}

QString relativeTo(MinecraftInstance* instance, const QString& path)
{
    return QDir(instance->instanceRoot()).relativeFilePath(path);
}

MinecraftInstance* instanceFrom(const QJsonObject& arguments, const ToolReply& reply)
{
    auto found = findInstance(arguments.value("instance").toString());
    if (!found) {
        reply(ToolResult::error(found.error()));
        return nullptr;
    }
    return *found;
}

QJsonObject imageContent(const QImage& image)
{
    QImage scaled = image;
    if (image.width() > MAX_IMAGE_EDGE || image.height() > MAX_IMAGE_EDGE) {
        scaled = image.scaled(MAX_IMAGE_EDGE, MAX_IMAGE_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    scaled.save(&buffer, "PNG");
    return QJsonObject{ { "type", "image" }, { "data", QString::fromLatin1(bytes.toBase64()) }, { "mimeType", "image/png" } };
}

// --- files ---

void filesList(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply);
    if (!instance) {
        return;
    }
    auto path = resolveInside(instance, arguments.value("path").toString("."));
    if (!path) {
        reply(ToolResult::error(path.error()));
        return;
    }
    if (!QFileInfo(*path).isDir()) {
        reply(ToolResult::error(QString("'%1' is not a folder.").arg(arguments.value("path").toString())));
        return;
    }
    const bool recursive = arguments.value("recursive").toBool(false);
    QDirIterator it(*path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    QJsonArray entries;
    bool truncated = false;
    while (it.hasNext()) {
        it.next();
        if (entries.size() >= MAX_LIST_ENTRIES) {
            truncated = true;
            break;
        }
        const auto info = it.fileInfo();
        QJsonObject entry{ { "path", relativeTo(instance, info.absoluteFilePath()) }, { "type", info.isDir() ? "dir" : "file" } };
        if (info.isFile()) {
            entry["sizeBytes"] = info.size();
        }
        if (info.isSymLink()) {
            entry["symlink"] = true;
        }
        entry["modified"] = info.lastModified().toUTC().toString(Qt::ISODate);
        entries.append(entry);
    }
    QJsonObject result{ { "instance", instance->id() }, { "root", instance->instanceRoot() }, { "entries", entries } };
    if (truncated) {
        result["truncated"] = true;
    }
    reply(ToolResult::ok(result));
}

void filesRead(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply);
    if (!instance) {
        return;
    }
    const auto relative = arguments.value("path").toString();
    auto path = resolveInside(instance, relative);
    if (!path) {
        reply(ToolResult::error(path.error()));
        return;
    }
    QFile file(*path);
    if (!QFileInfo(*path).isFile() || !file.open(QIODevice::ReadOnly)) {
        reply(ToolResult::error(QString("Cannot read '%1'%2").arg(relative, QFileInfo::exists(*path) ? QString(": not a readable file") : QString(": it does not exist")),
                                "files.list shows what is there."));
        return;
    }
    const qint64 offset = arguments.value("offset").toInteger(0);
    const qint64 length = std::min<qint64>(arguments.value("maxBytes").toInteger(DEFAULT_READ_BYTES), MAX_READ_BYTES);
    file.seek(offset);
    const auto data = file.read(length);

    QJsonObject result{ { "instance", instance->id() },
                        { "path", relative },
                        { "sizeBytes", file.size() },
                        { "offset", offset },
                        { "returnedBytes", data.size() },
                        { "eof", offset + data.size() >= file.size() } };
    // Valid UTF-8 comes back as text, anything else as base64
    auto decoder = QStringDecoder(QStringDecoder::Utf8);
    const QString text = decoder(data);
    if (!decoder.hasError() && !data.contains('\0')) {
        result["encoding"] = "text";
        result["content"] = text;
    } else {
        result["encoding"] = "base64";
        result["content"] = QString::fromLatin1(data.toBase64());
    }
    reply(ToolResult::ok(result));
}

void filesWrite(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply);
    if (!instance) {
        return;
    }
    const auto relative = arguments.value("path").toString();
    if (isProtectedFile(relative)) {
        reply(ToolResult::error(QString("'%1' is protected: it controls how the instance launches.").arg(relative),
                                "Use the settings tool or the components ops instead."));
        return;
    }
    auto path = resolveInside(instance, relative);
    if (!path) {
        reply(ToolResult::error(path.error()));
        return;
    }
    QByteArray data;
    if (arguments.value("encoding").toString("text") == "base64") {
        auto decoded = QByteArray::fromBase64Encoding(arguments.value("content").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            reply(ToolResult::error("content is not valid base64."));
            return;
        }
        data = *decoded;
    } else {
        data = arguments.value("content").toString().toUtf8();
    }
    if (data.size() > MAX_WRITE_BYTES) {
        reply(ToolResult::error(QString("Too large: %1 bytes (limit %2).").arg(data.size()).arg(MAX_WRITE_BYTES)));
        return;
    }
    const bool existed = QFileInfo::exists(*path);
    if (QFileInfo(*path).isDir()) {
        reply(ToolResult::error(QString("'%1' is a folder.").arg(relative)));
        return;
    }
    if (auto res = FS::write(*path, data); !res) {
        reply(ToolResult::error(res.error()));
        return;
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "path", relative }, { "bytes", data.size() }, { "created", !existed } }));
}

void filesDelete(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply);
    if (!instance) {
        return;
    }
    const auto relative = arguments.value("path").toString();
    if (isProtectedFile(relative) || QDir::cleanPath(relative) == ".") {
        reply(ToolResult::error(QString("'%1' cannot be deleted by agents.").arg(relative), "instance.delete removes a whole instance."));
        return;
    }
    auto path = resolveInside(instance, relative);
    if (!path) {
        reply(ToolResult::error(path.error()));
        return;
    }
    if (!QFileInfo::exists(*path)) {
        reply(ToolResult::error(QString("'%1' does not exist.").arg(relative)));
        return;
    }
    if (instance->isRunning()) {
        reply(ToolResult::error(QString("'%1' is running; deleting its files now could crash the game.").arg(instance->name())));
        return;
    }
    QString trashed;
    if (FS::trash(*path, &trashed)) {
        reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "path", relative }, { "trashed", true } }));
        return;
    }
    if (!arguments.value("permanent").toBool(false)) {
        reply(ToolResult::error("Moving it to the trash failed (or there is none).", "Pass permanent=true to delete it for good."));
        return;
    }
    if (!FS::deletePath(*path)) {
        reply(ToolResult::error(QString("Deleting '%1' failed.").arg(relative)));
        return;
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "path", relative }, { "trashed", false } }));
}

// --- screenshots ---

QDir screenshotsDir(MinecraftInstance* instance)
{
    return QDir(QDir(instance->gameRoot()).filePath(SCREENSHOTS_FOLDER));
}

void screenshotsList(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply);
    if (!instance) {
        return;
    }
    QJsonArray out;
    for (const auto& info : screenshotsDir(instance).entryInfoList({ "*.png", "*.jpg" }, QDir::Files, QDir::Time)) {
        out.append(QJsonObject{ { "file", info.fileName() },
                                { "sizeBytes", info.size() },
                                { "taken", info.lastModified().toUTC().toString(Qt::ISODate) } });
    }
    reply(ToolResult::ok(QJsonObject{ { "instance", instance->id() }, { "screenshots", out } }));
}

void screenshotsGet(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply);
    if (!instance) {
        return;
    }
    const auto name = arguments.value("file").toString();
    const auto dir = screenshotsDir(instance);
    if (name.contains('/') || name.contains('\\') || !dir.entryList(QDir::Files).contains(name)) {
        reply(ToolResult::error(QString("No screenshot '%1'.").arg(name), "screenshots.list gives the names."));
        return;
    }
    const QImage image(dir.filePath(name));
    if (image.isNull()) {
        reply(ToolResult::error(QString("'%1' is not a readable image.").arg(name)));
        return;
    }
    auto result = ToolResult::ok(QJsonObject{ { "file", name }, { "width", image.width() }, { "height", image.height() } });
    result.extraContent.append(imageContent(image));
    reply(result);
}

void screenshotsDelete(const CallContext&, const QJsonObject& arguments, const ToolReply& reply)
{
    auto* instance = instanceFrom(arguments, reply);
    if (!instance) {
        return;
    }
    const auto name = arguments.value("file").toString();
    const auto dir = screenshotsDir(instance);
    if (name.contains('/') || name.contains('\\') || !dir.entryList(QDir::Files).contains(name)) {
        reply(ToolResult::error(QString("No screenshot '%1'.").arg(name), "screenshots.list gives the names."));
        return;
    }
    if (!FS::trash(dir.filePath(name)) && !QFile::remove(dir.filePath(name))) {
        reply(ToolResult::error(QString("Could not delete '%1'.").arg(name)));
        return;
    }
    reply(ToolResult::ok(QJsonObject{ { "deleted", name } }));
}

// --- the launcher window ---

void windowScreenshot(const CallContext&, const QJsonObject&, const ToolReply& reply)
{
    QWidget* window = nullptr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMainWindow*>(widget) && widget->isVisible()) {
            window = widget;
            if (widget->isActiveWindow()) {
                break;
            }
        }
    }
    if (!window) {
        reply(ToolResult::error("No launcher window is open."));
        return;
    }
    const auto image = window->grab().toImage();
    auto result = ToolResult::ok(QJsonObject{ { "window", window->windowTitle() }, { "width", image.width() }, { "height", image.height() } });
    result.extraContent.append(imageContent(image));
    reply(result);
}

Tool fileOp(const QString& name, Scope scope, Effect effect, const QString& description, const QJsonObject& input,
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

QString instanceLabel(const QJsonObject& arguments)
{
    const auto id = arguments.value("instance").toString();
    auto* instance = APPLICATION->instances()->getInstanceById(id);
    return instance ? instance->name() : id;
}
}  // namespace

void registerFileOps(ToolRegistry& ops)
{
    using namespace Schema;
    const auto instanceArg = string("Instance id");
    const auto pathArg = string("Path relative to the instance folder, e.g. minecraft/config/sodium-options.json");

    ops.add(fileOp("files.list", Scope::Files, Effect::Read,
                   "Lists a folder inside an instance (default: the instance folder; the game's files are under minecraft/ or "
                   ".minecraft/). recursive=true walks subfolders (up to 2000 entries).",
                   object({ { "instance", instanceArg }, { "path", pathArg }, { "recursive", boolean("Include subfolders") } }, { "instance" }),
                   {}, filesList));

    ops.add(fileOp("files.read", Scope::Files, Effect::Read,
                   "Reads a file inside an instance: UTF-8 text as text, anything else as base64. Reads at most maxBytes (default 256 "
                   "KiB, max 4 MiB) from offset; eof says whether that was the end.",
                   object({ { "instance", instanceArg },
                            { "path", pathArg },
                            { "offset", integer("Byte offset (default 0)", 0, std::nullopt) },
                            { "maxBytes", integer("Bytes to read", 1, MAX_READ_BYTES) } },
                          { "instance", "path" }),
                   {}, filesRead));

    auto write = fileOp("files.write", Scope::Files, Effect::Write,
                        "Writes a whole file inside an instance (config files, options.txt, ...), creating folders as needed. "
                        "instance.cfg, mmc-pack.json and patches/ are refused: use settings and components ops. Max 8 MiB.",
                        object({ { "instance", instanceArg },
                                 { "path", pathArg },
                                 { "content", string("The file's new content") },
                                 { "encoding", string("How content is encoded (default text)", { "text", "base64" }) } },
                               { "instance", "path", "content" }),
                        [](const QJsonObject& a) { return QString("Write %1 in instance '%2'").arg(a.value("path").toString(), instanceLabel(a)); },
                        filesWrite);
    // Replacing a file that exists loses its old content; creating one does not
    write.effect = [](const QJsonObject& a) {
        auto found = findInstance(a.value("instance").toString());
        if (!found) {
            return Effect::Write;
        }
        auto path = resolveInside(*found, a.value("path").toString());
        return path && QFileInfo::exists(*path) ? Effect::Destructive : Effect::Write;
    };
    ops.add(write);

    ops.add(fileOp("files.delete", Scope::Files, Effect::Destructive,
                   "Deletes a file or folder inside an instance, to the system trash where there is one. Refused while the game "
                   "runs; permanent=true deletes for good when trashing fails.",
                   object({ { "instance", instanceArg }, { "path", pathArg }, { "permanent", boolean("Delete for good if trashing fails") } },
                          { "instance", "path" }),
                   [](const QJsonObject& a) { return QString("Delete %1 in instance '%2'").arg(a.value("path").toString(), instanceLabel(a)); },
                   filesDelete));

    ops.add(fileOp("screenshots.list", Scope::Read, Effect::Read, "Lists an instance's in-game screenshots, newest first.",
                   object({ { "instance", instanceArg } }, { "instance" }), {}, screenshotsList));

    ops.add(fileOp("screenshots.get", Scope::Read, Effect::Read,
                   "Returns one in-game screenshot as an image (scaled to at most 1600 px on its longer side).",
                   object({ { "instance", instanceArg }, { "file", string("File name from screenshots.list") } }, { "instance", "file" }), {},
                   screenshotsGet));

    ops.add(fileOp("screenshots.delete", Scope::Content, Effect::Destructive, "Deletes an in-game screenshot (to the trash where there is one).",
                   object({ { "instance", instanceArg }, { "file", string("File name from screenshots.list") } }, { "instance", "file" }),
                   [](const QJsonObject& a) { return QString("Delete screenshot %1 of '%2'").arg(a.value("file").toString(), instanceLabel(a)); },
                   screenshotsDelete));

    ops.add(fileOp("window.screenshot", Scope::Read, Effect::Read,
                   "Returns an image of the launcher's main window as the user sees it, to check the result of a change.", object({}), {},
                   windowScreenshot));
}

}  // namespace Mcp
