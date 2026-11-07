// SPDX-License-Identifier: GPL-3.0-only
#include "McpSettingsTool.h"

#include <QJsonArray>
#include <QMetaType>
#include <QRegularExpression>

#include "Application.h"
#include "mcp/McpCommon.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"
#include "settings/Setting.h"
#include "settings/SettingsObject.h"

namespace Mcp {

namespace {
constexpr auto LAUNCHER_TARGET = "launcher";
constexpr auto REDACTED = "<redacted>";
constexpr int DEFAULT_LIST_LIMIT = 300;

// Values never shown to an agent
const QStringList& secretKeys()
{
    static const QStringList s_keys{ "ProxyPass", "ProxyUser", "FlameKeyOverride", "ModrinthToken", "MSAClientIDOverride", "TechnicClientID" };
    return s_keys;
}

// Settings that can run a program, inject code into the game, redirect downloads or network traffic, or move the
// launcher's data. An agent changing one of these could take over the machine, so only the human may.
const QStringList& sensitiveKeys()
{
    static const QStringList s_keys{
        // run programs or inject code
        "PreLaunchCommand", "PostExitCommand", "WrapperCommand", "PreLoadCommand", "JvmArgs", "Env", "JavaPath", "CustomGLFWPath",
        "CustomOpenALPath", "CustomSDLPath", "JProfilerPath", "JVisualVMPath", "AuthlibInjectorPath",
        // where code and data are downloaded from or uploaded to
        "MetaURLOverride", "ResourceURLOverride", "LegacyFMLLibsURLOverride", "DownloadMirrorURL", "PastebinURL", "ManagedPackURL",
        "UserAgentOverride", "ProxyType", "ProxyAddr", "ProxyPort",
        // where the launcher keeps things
        "InstanceDir", "AdditionalInstanceDirs", "CentralModsDir", "IconsDir", "JavaDir", "SkinsDir", "DownloadsDir", "TemplateDir",
        "GlobalDataPacksPath", "FTBAppInstancesPath"
    };
    return s_keys;
}

enum class Protection { None, Secret, Sensitive, ReadOnly };

Protection protectionFor(const QString& key)
{
    static const QRegularExpression s_secretPattern("(token|password|passwd|secret|apikey|api_key|clientid)", QRegularExpression::CaseInsensitiveOption);
    if (secretKeys().contains(key) || s_secretPattern.match(key).hasMatch()) {
        return Protection::Secret;
    }
    // The endpoint's own switches, and opaque platform bookmarks
    if (key.startsWith("Mcp") || key.endsWith("Bookmark")) {
        return Protection::ReadOnly;
    }
    if (sensitiveKeys().contains(key)) {
        return Protection::Sensitive;
    }
    return Protection::None;
}

QString protectionName(Protection protection)
{
    switch (protection) {
        case Protection::None:
            return {};
        case Protection::Secret:
            return "secret";
        case Protection::Sensitive:
            return "sensitive";
        case Protection::ReadOnly:
            return "readonly";
    }
    return {};
}

QString typeName(const QVariant& value)
{
    switch (value.metaType().id()) {
        case QMetaType::Bool:
            return "boolean";
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            return "integer";
        case QMetaType::Double:
        case QMetaType::Float:
            return "number";
        case QMetaType::QStringList:
            return "array";
        case QMetaType::QString:
        case QMetaType::UnknownType:
            return "string";
        default:
            return "other";
    }
}

QJsonValue toJson(const QVariant& value)
{
    if (typeName(value) == "other") {
        return QJsonValue::Null;
    }
    return QJsonValue::fromVariant(value);
}

// Converts an agent-supplied JSON value to the setting's type, or explains why it can't
Result<QVariant> fromJson(const QJsonValue& value, const QVariant& defaultValue)
{
    const auto type = typeName(defaultValue);
    if (type == "boolean" && value.isBool()) {
        return QVariant(value.toBool());
    }
    if (type == "integer" && value.isDouble() && value.toDouble() == static_cast<double>(value.toInteger())) {
        return QVariant(static_cast<int>(value.toInteger()));
    }
    if (type == "number" && value.isDouble()) {
        return QVariant(value.toDouble());
    }
    if (type == "string" && value.isString()) {
        return QVariant(value.toString());
    }
    if (type == "array" && value.isArray()) {
        QStringList items;
        for (const auto& item : value.toArray()) {
            if (!item.isString()) {
                return std::unexpected(QString("expected an array of strings"));
            }
            items.append(item.toString());
        }
        return QVariant(items);
    }
    if (type == "other") {
        return std::unexpected(QString("this setting holds data that can't be set as JSON"));
    }
    return std::unexpected(QString("expected %1").arg(type));
}

QJsonObject describe(const std::shared_ptr<Setting>& setting)
{
    const auto key = setting->id();
    const auto protection = protectionFor(key);
    const auto value = setting->get();
    const auto defaultValue = setting->defValue();

    QJsonObject out{ { "key", key }, { "type", typeName(defaultValue.isValid() ? defaultValue : value) } };
    if (protection == Protection::Secret) {
        out["value"] = value.toString().isEmpty() ? QString() : QString(REDACTED);
        out["isSet"] = !value.toString().isEmpty();
    } else {
        out["value"] = toJson(value);
        out["default"] = toJson(defaultValue);
    }
    if (protection != Protection::None) {
        out["protection"] = protectionName(protection);
    }
    return out;
}

Result<SettingsObject*> targetSettings(const QString& target)
{
    if (target.isEmpty() || target == LAUNCHER_TARGET) {
        return APPLICATION->settings();
    }
    TRY_INTO(auto* instance, findInstance(target))
    return instance->settings();
}

void settingsTool(const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto action = arguments.value("action").toString();
    const auto target = arguments.value("target").toString(LAUNCHER_TARGET);
    auto settings = targetSettings(target);
    if (!settings) {
        reply(ToolResult::error(settings.error()));
        return;
    }
    auto* object = *settings;
    const auto ids = object->settingIds();

    if (action == "list") {
        const auto query = arguments.value("query").toString();
        const int limit = arguments.value("limit").toInt(DEFAULT_LIST_LIMIT);
        QJsonArray out;
        int matched = 0;
        for (const auto& id : ids) {
            if (!query.isEmpty() && !id.contains(query, Qt::CaseInsensitive)) {
                continue;
            }
            ++matched;
            if (out.size() < limit) {
                out.append(describe(object->getSetting(id)));
            }
        }
        reply(ToolResult::ok(QJsonObject{ { "target", target }, { "count", matched }, { "settings", out } }));
        return;
    }

    if (action == "get") {
        QJsonArray out;
        QStringList unknown;
        for (const auto& key : arguments.value("keys").toArray()) {
            const auto setting = object->getSetting(key.toString());
            if (!setting) {
                unknown.append(key.toString());
                continue;
            }
            out.append(describe(setting));
        }
        if (!unknown.isEmpty()) {
            QStringList hints;
            for (const auto& key : unknown) {
                if (const auto close = closestMatches(key, ids); !close.isEmpty()) {
                    hints.append(QString("%1 -> %2").arg(key, close.join(" / ")));
                }
            }
            reply(ToolResult::error(QString("Unknown setting(s): %1").arg(unknown.join(", ")),
                                    hints.isEmpty() ? QString("Use action=list with a query to find keys.") : "Did you mean: " + hints.join("; ")));
            return;
        }
        reply(ToolResult::ok(QJsonObject{ { "target", target }, { "settings", out } }));
        return;
    }

    // action == "set"
    if (!context.token.scopes.testFlag(Scope::Settings)) {
        reply(ToolResult::error("Changing settings needs the 'settings' scope, which this token does not have.",
                                "The user can grant it in the launcher: Settings > AI agents (MCP) > edit the token."));
        return;
    }
    const auto values = arguments.value("values").toObject();
    if (values.isEmpty()) {
        reply(ToolResult::error("action=set needs a non-empty `values` object (key: new value)."));
        return;
    }

    // Check everything first so a bad key doesn't leave half the values applied
    QList<std::pair<std::shared_ptr<Setting>, QVariant>> changes;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto setting = object->getSetting(it.key());
        if (!setting) {
            const auto close = closestMatches(it.key(), ids);
            reply(ToolResult::error(QString("Unknown setting '%1'. Nothing was changed.").arg(it.key()),
                                    close.isEmpty() ? QString() : QString("Did you mean: %1?").arg(close.join(", "))));
            return;
        }
        switch (protectionFor(setting->id())) {
            case Protection::Secret:
            case Protection::Sensitive:
                reply(ToolResult::error(QString("'%1' is protected: it can run programs, redirect downloads or traffic, move data, or "
                                                "holds a secret. Nothing was changed.")
                                            .arg(setting->id()),
                                        "Ask the user to change it in the launcher's settings."));
                return;
            case Protection::ReadOnly:
                reply(ToolResult::error(QString("'%1' cannot be changed by agents. Nothing was changed.").arg(setting->id())));
                return;
            case Protection::None:
                break;
        }
        const auto defaultValue = setting->defValue().isValid() ? setting->defValue() : setting->get();
        auto converted = fromJson(it.value(), defaultValue);
        if (!converted) {
            reply(ToolResult::error(QString("'%1': %2. Nothing was changed.").arg(setting->id(), converted.error())));
            return;
        }
        changes.emplace_back(setting, *converted);
    }

    QJsonArray applied;
    for (const auto& [setting, value] : changes) {
        const auto before = setting->get();
        setting->set(value);
        applied.append(QJsonObject{ { "key", setting->id() }, { "old", toJson(before) }, { "new", toJson(setting->get()) } });
    }
    reply(ToolResult::ok(QJsonObject{ { "target", target }, { "changed", applied } }));
}
}  // namespace

void registerSettingsTool(ToolRegistry& registry)
{
    using namespace Schema;

    Tool tool;
    tool.name = "settings";
    tool.title = "Launcher and instance settings";
    tool.description =
        "Reads and changes settings of the launcher (target \"launcher\", the default) or of one instance (target = instance id). "
        "action=list finds keys (filter with query), get reads keys, set writes values {key: value}, all checked before any is "
        "applied. Values keep the setting's type. An instance setting that mirrors a launcher setting (memory, Java, window size, "
        "...) only applies while its Override* switch is true, so set both. Secrets are redacted. Settings marked sensitive "
        "(commands, JVM arguments, executable paths, download and proxy URLs, data folders) can only be changed by the user.";
    tool.inputSchema = object(
        {
            { "action", string("What to do", { "list", "get", "set" }) },
            { "target", string("\"launcher\" (default) or an instance id") },
            { "query", string("list: substring of the key") },
            { "limit", integer("list: maximum settings returned (default 300)", 1, 2000) },
            { "keys", withDescription(array(string({})), "get: the keys to read") },
            { "values", withDescription(object({}, {}, false), "set: key to new value") },
        },
        { "action" });
    tool.outputSchema = object({ { "target", string({}) },
                                 { "count", integer({}) },
                                 { "settings", array(object({}, {}, false)) },
                                 { "changed", array(object({}, {}, false)) } },
                               { "target" }, false);
    tool.annotations.readOnly = false;
    tool.annotations.destructive = false;
    tool.annotations.idempotent = true;
    // Reading needs only `read`; set checks the `settings` scope itself
    tool.scope = Scope::Read;
    tool.handler = settingsTool;
    tool.effect = [](const QJsonObject& arguments) { return arguments.value("action").toString() == "set" ? Effect::Write : Effect::Read; };
    tool.describeAction = [](const QJsonObject& arguments) {
        QStringList changes;
        const auto values = arguments.value("values").toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            changes.append(QString("%1 = %2").arg(it.key(), it.value().toVariant().toString()));
        }
        return QString("Change settings of %1: %2").arg(arguments.value("target").toString(LAUNCHER_TARGET), changes.join(", "));
    };
    registry.add(std::move(tool));
}

}  // namespace Mcp
