// SPDX-License-Identifier: GPL-3.0-only
#include "McpResources.h"

#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>

#include "mcp/McpTool.h"

namespace Mcp {

namespace {
constexpr auto MARKDOWN = "text/markdown";
constexpr auto JSON = "application/json";
constexpr auto PLAIN = "text/plain";

// "launcher://instances/{id}/summary" -> ^launcher://instances/([^/]+)/summary$
QRegularExpression templatePattern(const QString& uriTemplate)
{
    static const QRegularExpression s_variable(R"(\{[^}]+\})");
    QString pattern;
    qsizetype last = 0;
    auto it = s_variable.globalMatch(uriTemplate);
    while (it.hasNext()) {
        const auto match = it.next();
        pattern += QRegularExpression::escape(uriTemplate.mid(last, match.capturedStart() - last));
        pattern += "([^/]+)";
        last = match.capturedEnd();
    }
    pattern += QRegularExpression::escape(uriTemplate.mid(last));
    return QRegularExpression("^" + pattern + "$");
}

QString toText(const QJsonValue& value)
{
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));
    }
    return value.toVariant().toString();
}

// Runs a registered tool or op as a read-only caller, for resources that are views of tool results
void viaTool(const ToolRegistry* registry, const QString& name, const QJsonObject& arguments, const QString& uri, const QString& mimeType,
             const ResourceReply& reply, std::function<QString(const ToolResult&)> render = {})
{
    const auto* tool = registry->find(name);
    if (!tool) {
        reply(std::unexpected(QString("internal: no %1").arg(name)));
        return;
    }
    CallContext context;
    context.token.name = "resource";
    context.token.scopes = Scope::Read;
    context.token.policy = ConfirmPolicy::NeverAsk;
    context.progress = [](double, std::optional<double>, const QString&) {};
    invokeTool(*tool, context, arguments, [uri, mimeType, reply, render](const ToolResult& result) {
        if (result.isError) {
            reply(std::unexpected(result.text));
            return;
        }
        reply(ResourceContent{ uri, mimeType, render ? render(result) : toText(result.structured) });
    });
}

const QHash<QString, QString>& guides()
{
    static const QHash<QString, QString> s_guides{
        { "modpack-building", QStringLiteral(R"(# Building a modpack

1. Pick the Minecraft version and loader first: most mods target one loader. Fabric/Quilt suit performance and
   client mods; NeoForge (1.20.2+) or Forge suit large content mods. Check that the key mods exist for that
   version with `content_versions`.
2. Create the instance: `create_instance` source=vanilla with `minecraft` and `loader`.
3. Search with `search_content` filtered by `minecraft` and `loader`, then install in one call:
   `install_content` with every project, `dryRun=true` first to see the plan and the dependencies it adds.
4. Fabric needs Fabric API (`fabric-api` on Modrinth) for nearly every mod; Quilt uses QFAPI/QSL; mods that need
   Architectury, Cloth Config or similar list them as required dependencies and `install_content` adds them.
5. Avoid duplicates that do the same job (two minimaps, two performance renderers such as Sodium and OptiFine).
6. Launch once (`launch`), then read `logs source=game minLevel=error` to catch missing dependencies or crashes.
)") },
        { "crash-triage", QStringLiteral(R"(# Diagnosing a crash

1. `logs source=crash` gives the newest crash report; `logs source=game minLevel=error` the console of the last run.
2. Look for: "Mod X requires Y" (missing dependency: install Y with `install_content`), "Incompatible mod set" or
   mixin errors naming a mod (an incompatible version: `content_versions` for a matching one, or disable it with
   `call content.disable`), "Unsupported class file major version" (wrong Java: see the java-versions guide),
   "OutOfMemoryError" (raise `MaxMemAlloc` with the settings tool, target = the instance, and `OverrideMemory` = true).
3. To find which mod breaks the game, disable half of the suspects, launch, and repeat on the half that still crashes.
4. Change one thing at a time and relaunch, reading the log each time.
)") },
        { "java-versions", QStringLiteral(R"(# Java and Minecraft versions

| Minecraft | Java |
|---|---|
| 1.20.5 and newer | 21 |
| 1.18 - 1.20.4 | 17 |
| 1.17 | 16 (17 works) |
| 1.16.5 and older | 8 |

The launcher picks a matching Java automatically when `AutomaticJava` is on. `call java.list_installs` shows what is
installed. An instance can be pinned to one with the settings tool (target = the instance): `OverrideJavaLocation`
= true and `JavaPath`, but `JavaPath` is a sensitive setting only the user may change.
)") },
        { "performance", QStringLiteral(R"(# Performance

- Fabric/Quilt: Sodium (rendering), Lithium (game logic), FerriteCore (memory), ImmediatelyFast, Entity Culling.
  NeoForge: Embeddium or Sodium (NeoForge builds), ModernFix, FerriteCore.
- Memory: 4-6 GB (`MaxMemAlloc` 4096-6144) suits most packs; more is not faster and lengthens GC pauses.
- Shaders need Iris (Fabric/Quilt/NeoForge) or OptiFine; install shader packs with `install_content kind=shaderpacks`.
)") },
    };
    return s_guides;
}
}  // namespace

void ResourceCatalog::add(Entry entry, Reader reader)
{
    entry.isTemplate = entry.uri.contains('{');
    m_entries.emplace_back(entry, std::move(reader));
}

QJsonArray ResourceCatalog::listJson() const
{
    QJsonArray out;
    for (const auto& [entry, reader] : m_entries) {
        if (!entry.isTemplate) {
            out.append(QJsonObject{ { "uri", entry.uri },
                                    { "name", entry.name },
                                    { "title", entry.title },
                                    { "description", entry.description },
                                    { "mimeType", entry.mimeType } });
        }
    }
    return out;
}

QJsonArray ResourceCatalog::templatesJson() const
{
    QJsonArray out;
    for (const auto& [entry, reader] : m_entries) {
        if (entry.isTemplate) {
            out.append(QJsonObject{ { "uriTemplate", entry.uri },
                                    { "name", entry.name },
                                    { "title", entry.title },
                                    { "description", entry.description },
                                    { "mimeType", entry.mimeType } });
        }
    }
    return out;
}

void ResourceCatalog::read(const QString& uri, const ResourceReply& reply) const
{
    for (const auto& [entry, reader] : m_entries) {
        if (!entry.isTemplate) {
            if (entry.uri == uri) {
                reader(uri, {}, reply);
                return;
            }
            continue;
        }
        const auto match = templatePattern(entry.uri).match(uri);
        if (match.hasMatch()) {
            auto captures = match.capturedTexts();
            captures.removeFirst();
            for (auto& capture : captures) {
                capture = QUrl::fromPercentEncoding(capture.toUtf8());
            }
            reader(uri, captures, reply);
            return;
        }
    }
    reply(std::unexpected(QString("Unknown resource: %1").arg(uri)));
}

void PromptCatalog::add(Prompt prompt)
{
    m_prompts.append(std::move(prompt));
}

QJsonArray PromptCatalog::listJson() const
{
    QJsonArray out;
    for (const auto& prompt : m_prompts) {
        QJsonArray arguments;
        for (const auto& argument : prompt.arguments) {
            arguments.append(QJsonObject{ { "name", argument.name }, { "description", argument.description }, { "required", argument.required } });
        }
        out.append(QJsonObject{ { "name", prompt.name }, { "title", prompt.title }, { "description", prompt.description }, { "arguments", arguments } });
    }
    return out;
}

Result<QJsonObject> PromptCatalog::get(const QString& name, const QJsonObject& arguments) const
{
    for (const auto& prompt : m_prompts) {
        if (prompt.name != name) {
            continue;
        }
        for (const auto& argument : prompt.arguments) {
            if (argument.required && arguments.value(argument.name).toString().isEmpty()) {
                return std::unexpected(QString("Prompt %1 needs the argument '%2'").arg(name, argument.name));
            }
        }
        const QJsonObject message{ { "role", "user" }, { "content", QJsonObject{ { "type", "text" }, { "text", prompt.render(arguments) } } } };
        return QJsonObject{ { "description", prompt.description }, { "messages", QJsonArray{ message } } };
    }
    return std::unexpected(QString("Unknown prompt: %1").arg(name));
}

void registerLauncherResources(ResourceCatalog& catalog, const ToolRegistry* tools, const ToolRegistry* ops)
{
    catalog.add({ "launcher://api/index.md", "api-index", "How this server works", "The tools, the op catalog, ids, scopes, jobs, confirmations.",
                  MARKDOWN },
                [tools, ops](const QString& uri, const QStringList&, const ResourceReply& reply) {
                    QString text = "# Launcher MCP server\n\n## Tools\n\n";
                    for (const auto& tool : tools->tools()) {
                        text += QString("- `%1`: %2\n").arg(tool.name, tool.title.isEmpty() ? tool.description.section(". ", 0, 0) : tool.title);
                    }
                    text += "\n## Op catalog (run with `call` or `batch`)\n\n";
                    for (const auto& op : ops->tools()) {
                        text += QString("- `%1` (%2%3): %4\n")
                                    .arg(op.name, scopeName(op.scope), op.annotations.readOnly ? QString(", read-only") : QString(),
                                         op.description.section(". ", 0, 0));
                    }
                    text += "\n## Rules\n\n"
                            "- Instances are addressed by id; projects by platform + projectId.\n"
                            "- Slow work returns a job id; follow it with `jobs action=wait`.\n"
                            "- Each token has scopes and a confirmation policy; refused or declined calls say why.\n"
                            "- Errors with isError=true carry a hint on how to recover.\n";
                    reply(ResourceContent{ uri, MARKDOWN, text });
                });

    catalog.add({ "launcher://api/ops.schema.json", "ops-schema", "Op catalog schemas", "Every op's description, scope and input JSON Schema.", JSON },
                [ops](const QString& uri, const QStringList&, const ResourceReply& reply) {
                    QJsonObject out;
                    for (const auto& op : ops->tools()) {
                        out[op.name] = QJsonObject{ { "description", op.description },
                                                    { "scope", scopeName(op.scope) },
                                                    { "readOnly", op.annotations.readOnly },
                                                    { "inputSchema", op.inputSchema } };
                    }
                    reply(ResourceContent{ uri, JSON, toText(out) });
                });

    catalog.add({ "launcher://instances/{id}/summary", "instance-summary", "Instance summary",
                  "What instance_get returns for one instance.", JSON },
                [tools](const QString& uri, const QStringList& captures, const ResourceReply& reply) {
                    viaTool(tools, "instance_get", QJsonObject{ { "instance", captures.value(0) } }, uri, JSON, reply);
                });

    catalog.add({ "launcher://instances/{id}/mods", "instance-mods", "Instance mods", "Every mod of one instance with its origin.", JSON },
                [ops](const QString& uri, const QStringList& captures, const ResourceReply& reply) {
                    viaTool(ops, "content.list", QJsonObject{ { "instance", captures.value(0) }, { "kind", "mods" }, { "limit", 5000 } }, uri, JSON,
                            reply);
                });

    catalog.add({ "launcher://instances/{id}/logs/latest", "instance-latest-log", "Latest game log", "The game's logs/latest.log (last 2000 lines).",
                  PLAIN },
                [tools](const QString& uri, const QStringList& captures, const ResourceReply& reply) {
                    viaTool(tools, "logs", QJsonObject{ { "instance", captures.value(0) }, { "source", "latest" }, { "lines", 2000 } }, uri, PLAIN,
                            reply, [](const ToolResult& result) { return result.structured.toObject().value("text").toString(); });
                });

    catalog.add({ "launcher://guide/{topic}", "guide", "Guides", "modpack-building, crash-triage, java-versions, performance.", MARKDOWN },
                [](const QString& uri, const QStringList& captures, const ResourceReply& reply) {
                    const auto topic = captures.value(0);
                    if (!guides().contains(topic)) {
                        reply(std::unexpected(QString("No guide '%1'. Topics: %2").arg(topic, guides().keys().join(", "))));
                        return;
                    }
                    reply(ResourceContent{ uri, MARKDOWN, guides().value(topic) });
                });
    for (const auto& topic : guides().keys()) {
        catalog.add({ "launcher://guide/" + topic, "guide-" + topic, topic, "Guide: " + topic, MARKDOWN },
                    [topic](const QString& uri, const QStringList&, const ResourceReply& reply) {
                        reply(ResourceContent{ uri, MARKDOWN, guides().value(topic) });
                    });
    }
}

void registerLauncherPrompts(PromptCatalog& catalog)
{
    catalog.add({ "build_modpack",
                  "Build a modpack",
                  "Create a new instance and fill it with mods around a theme.",
                  { { "theme", "What the pack is about, e.g. 'tech and automation'", true },
                    { "minecraft", "Minecraft version (optional)", false },
                    { "loader", "Mod loader (optional)", false } },
                  [](const QJsonObject& a) {
                      return QString("Build a Minecraft modpack around: %1.%2%3 Read launcher://guide/modpack-building first. Create a new "
                                     "instance, find fitting mods with search_content, install them with install_content (dry run first, "
                                     "then for real), launch once and check the log for errors. Report what you installed and why.")
                          .arg(a.value("theme").toString(),
                               a.value("minecraft").toString().isEmpty() ? QString() : QString(" Minecraft %1.").arg(a.value("minecraft").toString()),
                               a.value("loader").toString().isEmpty() ? QString() : QString(" Use %1.").arg(a.value("loader").toString()));
                  } });

    catalog.add({ "fix_crash",
                  "Fix a crashing instance",
                  "Find out why an instance crashes and fix it.",
                  { { "instance", "Instance id", true } },
                  [](const QJsonObject& a) {
                      return QString("The instance %1 crashes. Read launcher://guide/crash-triage, then the crash report and the last log "
                                     "(logs source=crash, logs source=game). Find the cause, fix it with as small a change as possible, "
                                     "relaunch to confirm, and tell me what was wrong.")
                          .arg(a.value("instance").toString());
                  } });

    catalog.add({ "update_instance",
                  "Update an instance's mods",
                  "Check for and apply mod updates safely.",
                  { { "instance", "Instance id", true } },
                  [](const QJsonObject& a) {
                      return QString("Update the mods of instance %1: back up its worlds first (call worlds.backup for each world in "
                                     "worlds.list), run content.check_updates, show me the list, then content.update. Launch once and "
                                     "check the log.")
                          .arg(a.value("instance").toString());
                  } });

    catalog.add({ "optimize",
                  "Make an instance run faster",
                  "Add performance mods and tune memory for one instance.",
                  { { "instance", "Instance id", true } },
                  [](const QJsonObject& a) {
                      return QString("Make instance %1 run faster. Read launcher://guide/performance, check what it already has "
                                     "(instance_get, content.list), add compatible performance mods with install_content, and set a "
                                     "sensible memory limit with the settings tool. Explain each change.")
                          .arg(a.value("instance").toString());
                  } });
}

}  // namespace Mcp
