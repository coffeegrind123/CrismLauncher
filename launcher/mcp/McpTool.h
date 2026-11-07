// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

#include "mcp/McpTokens.h"

namespace Mcp {

// Who is calling and how to talk back while a call runs. Tools never see HTTP.
struct CallContext {
    TokenInfo token;
    QString clientName;
    QString protocolVersion;

    // Sends notifications/progress when the client asked for it; harmless otherwise
    std::function<void(double progress, std::optional<double> total, const QString& message)> progress;

    // Set when the client went away or cancelled; long tools check it and stop
    std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);
};

struct ToolResult {
    QJsonValue structured;     // structuredContent; must match the tool's outputSchema
    QString text;              // text content; when empty the structured value is rendered as JSON
    QJsonArray extraContent;   // images, resource links
    bool isError = false;

    static ToolResult ok(const QJsonValue& structured, const QString& text = {});
    // `hint` tells the model how to recover ("call instances to list valid ids")
    static ToolResult error(const QString& message, const QString& hint = {});
};

using ToolReply = std::function<void(const ToolResult&)>;
using ToolHandler = std::function<void(const CallContext&, const QJsonObject& arguments, const ToolReply&)>;

struct ToolAnnotations {
    bool readOnly = true;
    bool destructive = false;
    bool idempotent = true;
    bool openWorld = false;  // reaches outside the launcher (mod platforms, the web)
};

struct Tool {
    QString name;
    QString title;
    QString description;
    QJsonObject inputSchema;
    QJsonObject outputSchema;
    ToolAnnotations annotations;
    Scope scope = Scope::Read;
    // Claude Code keeps these loaded when tool search is on, so the entry points are always visible
    bool alwaysLoad = false;
    ToolHandler handler;

    QJsonObject describe() const;
};

// The fixed tool list. The spec requires tools/list not to vary per connection, so tools are registered once at
// startup, sorted by name for deterministic order, and never added or removed afterwards.
class ToolRegistry {
   public:
    void add(Tool tool);
    const QList<Tool>& tools() const { return m_tools; }
    const Tool* find(const QString& name) const;
    // Closest registered names, for "did you mean" in errors
    QStringList suggest(const QString& name) const;

   private:
    QList<Tool> m_tools;
};

// Shared by error messages: the nearest strings by edit distance, best first
QStringList closestMatches(const QString& needle, const QStringList& haystack, int maxResults = 3);

}  // namespace Mcp
