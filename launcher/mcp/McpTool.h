// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

#include "mcp/McpTokens.h"

namespace Mcp {

// The answer to a confirmation question
struct ConfirmVerdict {
    enum class Kind { Allowed, Denied, NeedsInput };
    Kind kind = Kind::Denied;
    QString reason;
    // NeedsInput: the InputRequiredResult (inputRequests + requestState) that asks the user through the agent's client
    QJsonObject inputRequired;

    static ConfirmVerdict allowed() { return { Kind::Allowed, {}, {} }; }
    static ConfirmVerdict denied(const QString& reason) { return { Kind::Denied, reason, {} }; }
    static ConfirmVerdict needsInput(const QJsonObject& inputRequired) { return { Kind::NeedsInput, {}, inputRequired }; }
};
using ConfirmAnswer = std::function<void(const ConfirmVerdict&)>;

// Who is calling and how to talk back while a call runs. Tools never see HTTP.
struct CallContext {
    TokenInfo token;
    QString clientName;
    QString protocolVersion;

    // Sends notifications/progress when the client asked for it; harmless otherwise
    std::function<void(double progress, std::optional<double> total, const QString& message)> progress;

    // Set when the client went away or cancelled; long tools check it and stop
    std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);

    // Asks the human to allow `actions` (one sentence each, e.g. "Delete instance 'Foo'"), all at once. Answered in the
    // launcher window, or through the agent's client when it supports elicitation. Unset means nobody can answer, which
    // counts as a refusal.
    std::function<void(const QStringList& actions, const ConfirmAnswer& answer)> confirm;
    // Always the launcher window, never the agent's chat: execute asks here, because an in-chat answer arrives as a retry
    // of the whole call, which would run the script's earlier ops a second time. Unset when there is no window.
    std::function<void(const QStringList& actions, const ConfirmAnswer& answer)> confirmInLauncher;
};

// What a call does, which decides whether the token's policy asks the human first
enum class Effect { Read, Write, Destructive };

// Stable error codes, part of the public contract: agents branch on them, so add new ones instead of renaming
//   invalid_argument  the arguments are wrong; fix them and call again
//   unknown_op        no such tool or op; see the "did you mean" list or search_ops
//   not_found         an id, name, file or version that does not exist; list them again
//   scope_missing     the token lacks a scope; only the user can grant it
//   denied            the user declined, or nobody could be asked
//   protected         the launcher never lets agents change this
//   busy              in use (a running game, agents paused); retry once it is free
//   conflict          the current state does not allow it (already exists, not running, required)
//   unavailable       this build, platform or window cannot do it
//   failed            the work itself failed (download, disk, platform API)
//   cancelled         stopped by the agent, the user or a disconnect
//   timeout           a time limit ran out; the work may still have happened
//   internal          a bug in the launcher
//   script_error      execute: the script threw, or an op it called failed and it did not catch that
//   out_of_memory     execute: the script went over its memory limit
enum class ErrorCode {
    InvalidArgument,
    UnknownOp,
    NotFound,
    ScopeMissing,
    Denied,
    Protected,
    Busy,
    Conflict,
    Unavailable,
    Failed,
    Cancelled,
    Timeout,
    Internal,
    ScriptError,
    OutOfMemory,
};
QString errorCodeName(ErrorCode code);

struct ToolResult {
    QJsonValue structured;     // structuredContent; must match the tool's outputSchema
    QString text;              // text content; when empty the structured value is rendered as JSON
    QJsonArray extraContent;   // images, resource links
    bool isError = false;
    ErrorCode code = ErrorCode::Internal;  // meaningful only when isError
    // The call started this job; clients of the tasks extension get a task for it instead of this result
    QString jobId;
    // Set instead of a result when the user must answer first (MRTR InputRequiredResult)
    std::optional<QJsonObject> inputRequired;

    static ToolResult needsInput(const QJsonObject& inputRequired);

    static ToolResult ok(const QJsonValue& structured, const QString& text = {});
    // `hint` tells the model how to recover ("call instances to list valid ids")
    static ToolResult error(ErrorCode code, const QString& message, const QString& hint = {});
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
// The sentence the user is asked to confirm for this call
QString describeAction(const Tool& tool, const QJsonObject& arguments);

// Runs a tool (or a catalog op) the way tools/call does: scope check, argument validation against inputSchema, the
// token's confirmation policy, and an exception guard. `reply` is called exactly once unless the handler itself never replies.
void invokeTool(const Tool& tool, const CallContext& context, const QJsonObject& arguments, const ToolReply& reply);

// Shared by error messages: the nearest strings by edit distance, best first
QStringList closestMatches(const QString& needle, const QStringList& haystack, int maxResults = 3);

}  // namespace Mcp
