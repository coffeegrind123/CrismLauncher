// SPDX-License-Identifier: GPL-3.0-only
#include "McpTool.h"

#include <algorithm>
#include <vector>

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
