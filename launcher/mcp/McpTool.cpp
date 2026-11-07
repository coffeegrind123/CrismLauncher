// SPDX-License-Identifier: GPL-3.0-only
#include "McpTool.h"

#include <QJsonDocument>

#include <algorithm>
#include <exception>
#include <vector>

#include "mcp/McpSchema.h"

namespace Mcp {

ToolResult ToolResult::ok(const QJsonValue& structured, const QString& text)
{
    ToolResult result;
    result.structured = structured;
    result.text = text;
    return result;
}

ToolResult ToolResult::error(const QString& message, const QString& hint)
{
    ToolResult result;
    result.isError = true;
    QJsonObject error{ { "error", message } };
    if (!hint.isEmpty()) {
        error["hint"] = hint;
    }
    result.structured = error;
    result.text = hint.isEmpty() ? message : message + "\n" + hint;
    return result;
}

QJsonObject Tool::describe() const
{
    QJsonObject out{
        { "name", name },
        { "description", description },
        { "inputSchema", inputSchema },
    };
    if (!title.isEmpty()) {
        out["title"] = title;
    }
    if (!outputSchema.isEmpty()) {
        out["outputSchema"] = outputSchema;
    }
    QJsonObject hints{
        { "readOnlyHint", annotations.readOnly },
        { "openWorldHint", annotations.openWorld },
    };
    if (!title.isEmpty()) {
        hints["title"] = title;
    }
    // The spec only defines these for tools that modify things
    if (!annotations.readOnly) {
        hints["destructiveHint"] = annotations.destructive;
        hints["idempotentHint"] = annotations.idempotent;
    }
    out["annotations"] = hints;
    if (alwaysLoad) {
        out["_meta"] = QJsonObject{ { "anthropic/alwaysLoad", true } };
    }
    return out;
}

void ToolRegistry::add(Tool tool)
{
    Q_ASSERT(!find(tool.name));
    m_tools.append(std::move(tool));
    std::sort(m_tools.begin(), m_tools.end(), [](const Tool& a, const Tool& b) { return a.name < b.name; });
}

const Tool* ToolRegistry::find(const QString& name) const
{
    for (const auto& tool : m_tools) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

QStringList ToolRegistry::suggest(const QString& name) const
{
    QStringList names;
    for (const auto& tool : m_tools) {
        names.append(tool.name);
    }
    return closestMatches(name, names);
}

Effect Tool::effectFor(const QJsonObject& arguments) const
{
    if (effect) {
        return effect(arguments);
    }
    if (annotations.readOnly) {
        return Effect::Read;
    }
    return annotations.destructive ? Effect::Destructive : Effect::Write;
}

bool needsConfirmation(Effect effect, ConfirmPolicy policy)
{
    switch (policy) {
        case ConfirmPolicy::NeverAsk:
            return false;
        case ConfirmPolicy::AskAllWrites:
            return effect != Effect::Read;
        case ConfirmPolicy::AskDestructive:
            return effect == Effect::Destructive;
    }
    return true;
}

namespace {
QString defaultDescription(const Tool& tool, const QJsonObject& arguments)
{
    auto text = QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    constexpr int maxChars = 300;
    if (text.size() > maxChars) {
        text = text.left(maxChars) + QChar(0x2026);
    }
    return QString("%1 %2").arg(tool.title.isEmpty() ? tool.name : tool.title, text);
}
}  // namespace

void invokeTool(const Tool& tool, const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    if (!context.token.scopes.testFlag(tool.scope)) {
        reply(ToolResult::error(QString("This token does not have the '%1' scope, which %2 needs.").arg(scopeName(tool.scope), tool.name),
                                "The user can grant it in the launcher: Settings > AI agents (MCP) > edit the token."));
        return;
    }
    if (auto valid = Schema::validate(arguments, tool.inputSchema); !valid) {
        reply(ToolResult::error(QString("Invalid arguments for %1: %2").arg(tool.name, valid.error()),
                                "Check the inputSchema and call again."));
        return;
    }

    auto run = [tool, context, arguments, reply] {
        try {
            tool.handler(context, arguments, reply);
        } catch (const std::exception& e) {
            reply(ToolResult::error(QString("Internal error in %1: %2").arg(tool.name, QString::fromUtf8(e.what()))));
        }
    };

    if (tool.delegatesConfirmation || !needsConfirmation(tool.effectFor(arguments), context.token.policy)) {
        run();
        return;
    }
    if (!context.confirm) {
        reply(ToolResult::error(QString("%1 needs the user's confirmation, and nobody can be asked here.").arg(tool.name)));
        return;
    }
    const auto action = tool.describeAction ? tool.describeAction(arguments) : defaultDescription(tool, arguments);
    context.confirm(action, [run, reply, name = tool.name](bool allowed, const QString& reason) {
        if (!allowed) {
            reply(ToolResult::error(QString("The user did not allow %1%2").arg(name, reason.isEmpty() ? QString(".") : ": " + reason),
                                    "Do not retry the same change unless the user asks for it."));
            return;
        }
        run();
    });
}

namespace {
int editDistance(const QString& a, const QString& b)
{
    QList<int> row(b.size() + 1);
    for (qsizetype j = 0; j <= b.size(); ++j) {
        row[j] = static_cast<int>(j);
    }
    for (qsizetype i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = static_cast<int>(i);
        for (qsizetype j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            const int cost = a[i - 1].toLower() == b[j - 1].toLower() ? 0 : 1;
            row[j] = std::min({ row[j] + 1, row[j - 1] + 1, diagonal + cost });
            diagonal = above;
        }
    }
    return row[b.size()];
}
}  // namespace

QStringList closestMatches(const QString& needle, const QStringList& haystack, int maxResults)
{
    std::vector<std::pair<int, QString>> scored;
    const int limit = std::max(2, static_cast<int>(needle.size()) / 3);
    for (const auto& candidate : haystack) {
        int distance = editDistance(needle, candidate);
        // A substring hit ("mods" in "list_mods") counts as close whatever the length difference
        if (candidate.contains(needle, Qt::CaseInsensitive) || needle.contains(candidate, Qt::CaseInsensitive)) {
            distance = std::min(distance, 1);
        }
        if (distance <= limit) {
            scored.emplace_back(distance, candidate);
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QStringList out;
    for (const auto& [distance, candidate] : scored) {
        if (out.size() >= maxResults) {
            break;
        }
        out.append(candidate);
    }
    return out;
}

}  // namespace Mcp
