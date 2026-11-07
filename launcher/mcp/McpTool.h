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

    // Asks the human to allow `action` (one sentence, e.g. "Delete instance 'Foo'"). Unset means nobody can answer, which
    // counts as a refusal.
    std::function<void(const QString& action, std::function<void(bool allowed, const QString& reason)>)> confirm;
};

// What a call does, which decides whether the token's policy asks the human first
enum class Effect { Read, Write, Destructive };

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

    // Per-call effect when it depends on the arguments (settings: list reads, set writes); by default from annotations
    std::function<Effect(const QJsonObject& arguments)> effect;
    // The sentence the human sees when asked to confirm; by default the title and the arguments
    std::function<QString(const QJsonObject& arguments)> describeAction;
    // call and batch run ops that confirm themselves; asking for the wrapper too would ask twice
    bool delegatesConfirmation = false;

    Effect effectFor(const QJsonObject& arguments) const;

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

bool needsConfirmation(Effect effect, ConfirmPolicy policy);

// Runs a tool (or a catalog op) the way tools/call does: scope check, argument validation against inputSchema, the
// token's confirmation policy, and an exception guard. `reply` is called exactly once unless the handler itself never replies.
void invokeTool(const Tool& tool, const CallContext& context, const QJsonObject& arguments, const ToolReply& reply);

// Shared by error messages: the nearest strings by edit distance, best first
QStringList closestMatches(const QString& needle, const QStringList& haystack, int maxResults = 3);

}  // namespace Mcp
