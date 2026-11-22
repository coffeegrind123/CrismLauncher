// SPDX-License-Identifier: GPL-3.0-only
#include "McpOps.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <memory>

#include "mcp/McpReplay.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"

namespace Mcp {

namespace {
constexpr int DEFAULT_SEARCH_LIMIT = 50;

QString area(const QString& opName)
{
    return opName.section('.', 0, 0);
}

QString joinStrings(const QJsonArray& values)
{
    QStringList out;
    for (const auto& value : values) {
        out.append(value.toString());
    }
    return out.join('|');
}

QString firstSentence(const QString& text)
{
    const auto end = text.indexOf(". ");
    return end < 0 ? text : text.left(end + 1);
}

// "instance: string (required), limit?: integer"
QString signature(const QJsonObject& schema)
{
    const auto properties = schema.value("properties").toObject();
    QStringList required;
    for (const auto& name : schema.value("required").toArray()) {
        required.append(name.toString());
    }
    QStringList parts;
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        const auto type = it.value().toObject().value("type");
        QString typeText = type.isArray() ? joinStrings(type.toArray()) : type.toString();
        if (const auto values = it.value().toObject().value("enum").toArray(); !values.isEmpty()) {
            typeText = joinStrings(values);
        }
        const QString optional = required.contains(it.key()) ? QString() : QStringLiteral("?");
        parts.append(QString("%1%2: %3").arg(it.key(), optional, typeText));
    }
    return parts.join(", ");
}

QJsonObject describeOp(const Tool& op, const QString& detail)
{
    QJsonObject out{ { "name", op.name } };
    if (detail == "names") {
        return out;
    }
    out["scope"] = scopeName(op.scope);
    out["readOnly"] = op.annotations.readOnly;
    if (!op.annotations.readOnly) {
        out["destructive"] = op.annotations.destructive;
    }
    if (detail == "signatures") {
        out["summary"] = firstSentence(op.description);
        out["arguments"] = signature(op.inputSchema);
        return out;
    }
    out["description"] = op.description;
    out["inputSchema"] = op.inputSchema;
    if (!op.outputSchema.isEmpty()) {
        out["outputSchema"] = op.outputSchema;
    }
    return out;
}

void searchOps(const ToolRegistry* ops, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto terms = arguments.value("query").toString().toLower().split(' ', Qt::SkipEmptyParts);
    const auto wantedArea = arguments.value("area").toString();
    const auto detail = arguments.value("detail").toString("signatures");
    const int limit = arguments.value("limit").toInt(DEFAULT_SEARCH_LIMIT);

    QJsonArray matches;
    QStringList areas;
    int total = 0;
    for (const auto& op : ops->tools()) {
        if (!areas.contains(area(op.name))) {
            areas.append(area(op.name));
        }
        if (!wantedArea.isEmpty() && area(op.name) != wantedArea) {
            continue;
        }
        const auto haystack = (op.name + ' ' + op.title + ' ' + op.description).toLower();
        if (!std::all_of(terms.begin(), terms.end(), [&haystack](const QString& term) { return haystack.contains(term); })) {
            continue;
        }
        ++total;
        if (matches.size() < limit) {
            matches.append(describeOp(op, detail));
        }
    }

    QJsonObject result{ { "ops", matches }, { "count", total }, { "areas", QJsonArray::fromStringList(areas) } };
    if (total == 0 && !terms.isEmpty()) {
        QStringList names;
        for (const auto& op : ops->tools()) {
            names.append(op.name);
        }
        result["didYouMean"] = QJsonArray::fromStringList(closestMatches(terms.join(' '), names));
    }
    reply(ToolResult::ok(result));
}

const Tool* findOp(const ToolRegistry* ops, const QString& name, const ToolReply& reply)
{
    if (const auto* op = ops->find(name)) {
        return op;
    }
    const auto close = ops->suggest(name);
    reply(ToolResult::error(ErrorCode::UnknownOp, QString("Unknown op '%1'.").arg(name),
                            close.isEmpty() ? QString("Use search_ops to find ops.") : QString("Did you mean: %1?").arg(close.join(", "))));
    return nullptr;
}

void callOp(const ToolRegistry* ops, const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto* op = findOp(ops, arguments.value("op").toString(), reply);
    if (!op) {
        return;
    }
    invokeTool(*op, context, arguments.value("arguments").toObject(), reply);
}

QJsonObject resultEntry(qsizetype index, const QString& op, const ToolResult& result, qint64 elapsedMs)
{
    QJsonObject entry{ { "index", index }, { "op", op }, { "ok", !result.isError }, { "seconds", static_cast<double>(elapsedMs) / 1000.0 } };
    if (result.isError) {
        const auto error = result.structured.toObject();
        entry["error"] = error.value("error").toString(result.text);
        entry["code"] = errorCodeName(result.code);
        if (const auto hint = error.value("hint").toString(); !hint.isEmpty()) {
            entry["hint"] = hint;
        }
    } else {
        entry["result"] = result.structured;
    }
    return entry;
}

struct BatchOptions {
    bool continueOnError = false;
    QString label;
};

void executeBatch(const ToolRegistry* ops, const CallContext& context, const QJsonArray& steps, const BatchOptions& options, const ReplayCache::Finish& finish);

void startBatch(const ToolRegistry* ops, const CallContext& context, const QJsonObject& arguments, const ReplayCache::Finish& reply);

void runBatch(const ToolRegistry* ops, const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    replayCache().run(context.token.id, "batch", arguments.value("operation_id").toString(), arguments, reply,
                      [ops, context, arguments](const ReplayCache::Finish& finish) { startBatch(ops, context, arguments, finish); });
}

void startBatch(const ToolRegistry* ops, const CallContext& context, const QJsonObject& arguments, const ReplayCache::Finish& finish)
{
    const auto steps = arguments.value("ops").toArray();
    const BatchOptions options{ arguments.value("continueOnError").toBool(false), arguments.value("label").toString() };
    // Refusals before the first step: nothing was applied
    auto reply = [finish](const ToolResult& result) { finish(result, false); };

    // Every op must exist and be allowed before anything runs, so a typo in step 9 doesn't leave steps 1-8 applied
    for (qsizetype i = 0; i < steps.size(); ++i) {
        const auto name = steps[i].toObject().value("op").toString();
        const auto* op = ops->find(name);
        if (!op) {
            const auto close = ops->suggest(name);
            reply(ToolResult::error(ErrorCode::UnknownOp, QString("Step %1: unknown op '%2'. Nothing was run.").arg(i).arg(name),
                                    close.isEmpty() ? QString("Use search_ops to find ops.") : QString("Did you mean: %1?").arg(close.join(", "))));
            return;
        }
        if (!context.token.scopes.testFlag(op->scope)) {
            reply(ToolResult::error(ErrorCode::ScopeMissing,
                QString("Step %1: this token lacks the '%2' scope that %3 needs. Nothing was run.").arg(i).arg(scopeName(op->scope), name)));
            return;
        }
    }

    // One question for every step that needs the user's approval, before any step runs: asking step by step would
    // leave the earlier steps applied whenever the user is asked through the agent's client (which retries the call)
    QStringList actions;
    for (const auto& value : steps) {
        const auto step = value.toObject();
        const auto* op = ops->find(step.value("op").toString());
        const auto stepArguments = step.value("arguments").toObject();
        if (!op->delegatesConfirmation && needsConfirmation(op->effectFor(stepArguments), context.token.policy)) {
            actions.append(describeAction(*op, stepArguments));
        }
    }
    if (actions.isEmpty()) {
        executeBatch(ops, context, steps, options, finish);
        return;
    }
    if (!context.confirm) {
        reply(ToolResult::error(ErrorCode::Denied, "Some steps need the user's confirmation, and nobody can be asked here. Nothing was run."));
        return;
    }
    context.confirm(actions, [ops, context, steps, options, reply, finish](const ConfirmVerdict& verdict) {
        switch (verdict.kind) {
            case ConfirmVerdict::Kind::NeedsInput:
                reply(ToolResult::needsInput(verdict.inputRequired));
                return;
            case ConfirmVerdict::Kind::Denied:
                reply(ToolResult::error(ErrorCode::Denied, QString("The user did not allow this batch%1 Nothing was run.").arg(verdict.reason.isEmpty() ? QString(".") : ": " + verdict.reason + "."),
                                        "Do not retry the same changes unless the user asks for them."));
                return;
            case ConfirmVerdict::Kind::Allowed:
                break;
        }
        // Approved as a whole: the steps must not ask again one by one
        auto approved = context;
        approved.token.policy = ConfirmPolicy::NeverAsk;
        executeBatch(ops, approved, steps, options, finish);
    });
}

void executeBatch(const ToolRegistry* ops, const CallContext& context, const QJsonArray& steps, const BatchOptions& options, const ReplayCache::Finish& done)
{
    struct State {
        QJsonArray steps;
        QJsonArray results;
        qsizetype next = 0;
        int failures = 0;
        ToolResult firstFailure;
    };
    auto state = std::make_shared<State>();
    state->steps = steps;
    const bool continueOnError = options.continueOnError;

    auto finish = [state, done, options] {
        QJsonObject result{ { "results", state->results }, { "completed", state->results.size() }, { "total", state->steps.size() } };
        if (!options.label.isEmpty()) {
            result["label"] = options.label;
        }
        if (state->results.size() < state->steps.size()) {
            result["stoppedAt"] = state->results.size() - 1;
        }
        if (state->failures > 0) {
            result["failures"] = state->failures;
        }
        auto out = ToolResult::ok(result);
        // Ops that already ran are not undone; the agent sees exactly which ones ran
        out.isError = state->failures > 0 && !options.continueOnError;
        if (out.isError) {
            const auto failed = state->results.last().toObject();
            out.code = state->firstFailure.code;
            // The text leads with the failure, so a client that only shows text still knows what stopped the batch
            out.text = QString("%1: step %2 (%3) failed: %4\n%5")
                           .arg(errorCodeName(out.code))
                           .arg(failed.value("index").toInt())
                           .arg(failed.value("op").toString(), failed.value("error").toString(),
                                QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        }
        done(out, !state->results.isEmpty());
    };

    // Each pending op's callback holds the only strong reference to `step`, so the chain frees itself when it ends
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakStep = step;
    *step = [ops, context, continueOnError, options, state, finish, weakStep] {
        auto self = weakStep.lock();
        if (!self) {
            return;
        }
        if (state->next >= state->steps.size() || *context.cancelled) {
            finish();
            return;
        }
        const auto index = state->next++;
        const auto entry = state->steps[index].toObject();
        const auto name = entry.value("op").toString();
        const auto* op = ops->find(name);
        if (context.progress) {
            const auto message = QString("step %1: %2").arg(index).arg(name);
            context.progress(static_cast<double>(index), static_cast<double>(state->steps.size()),
                             options.label.isEmpty() ? message : options.label + ": " + message);
        }

        auto timer = std::make_shared<QElapsedTimer>();
        timer->start();
        auto onResult = [continueOnError, state, finish, self, name, index, timer](const ToolResult& result) {
            state->results.append(resultEntry(index, name, result, timer->elapsed()));
            if (result.isError) {
                if (state->failures == 0) {
                    state->firstFailure = result;
                }
                ++state->failures;
                if (!continueOnError) {
                    finish();
                    return;
                }
            }
            (*self)();
        };

        auto resolved = resolveReferences(entry.value("arguments").toObject(), state->results, static_cast<int>(index));
        if (!resolved) {
            onResult(ToolResult::error(ErrorCode::InvalidArgument, QString("Step %1: %2").arg(index).arg(resolved.error())));
            return;
        }
        invokeTool(*op, context, resolved->toObject(), onResult);
    };
    (*step)();
}

Result<QJsonValue> lookupPath(QJsonValue value, const QString& path)
{
    if (path.isEmpty()) {
        return value;
    }
    for (const auto& key : path.split('.')) {
        if (value.isArray()) {
            bool ok = false;
            const auto index = key.toInt(&ok);
            const auto array = value.toArray();
            if (!ok || index < 0 || index >= array.size()) {
                return std::unexpected(QString("index '%1' is out of range (size %2)").arg(key).arg(array.size()));
            }
            value = array[index];
        } else if (value.isObject()) {
            const auto object = value.toObject();
            if (!object.contains(key)) {
                return std::unexpected(QString("no field '%1' (fields: %2)").arg(key, object.keys().join(", ")));
            }
            value = object[key];
        } else {
            return std::unexpected(QString("cannot read '%1' of a %2").arg(key, value.isNull() ? "null" : "plain value"));
        }
    }
    return value;
}

Result<QJsonValue> resolveOne(const QString& text, int index, const QString& path, const QJsonArray& results, int current)
{
    if (index >= current) {
        return std::unexpected(QString("'%1' refers to step %2, which has not run yet").arg(text).arg(index));
    }
    const auto entry = results.at(index).toObject();
    if (!entry.value("ok").toBool()) {
        return std::unexpected(QString("'%1' refers to step %2, which failed").arg(text).arg(index));
    }
    auto resolved = lookupPath(entry.value("result"), path);
    if (!resolved) {
        return std::unexpected(QString("'%1': %2").arg(text, resolved.error()));
    }
    return *resolved;
}
}  // namespace

Result<QJsonValue> resolveReferences(const QJsonValue& value, const QJsonArray& results, int current)
{
    static const QRegularExpression s_reference(R"(^\$(\d+)(?:\.(.+))?$)");
    static const QRegularExpression s_inline(R"(\$\{(\d+)(?:\.([^}]+))?\})");

    if (value.isString()) {
        const auto text = value.toString();
        if (text.startsWith("$$")) {
            return QJsonValue(text.mid(1));
        }
        if (const auto match = s_reference.match(text); match.hasMatch()) {
            return resolveOne(text, match.captured(1).toInt(), match.captured(2), results, current);
        }
        // "${N.path}" inside a longer string: replaced by the value's text (JSON for anything but a string)
        QString out;
        qsizetype from = 0;
        auto it = s_inline.globalMatch(text);
        if (!it.hasNext()) {
            return value;
        }
        while (it.hasNext()) {
            const auto match = it.next();
            TRY_INTO(const auto resolved, resolveOne(match.captured(0), match.captured(1).toInt(), match.captured(2), results, current))
            out += text.mid(from, match.capturedStart() - from);
            if (resolved.isString()) {
                out += resolved.toString();
            } else if (resolved.isObject() || resolved.isArray()) {
                out += QString::fromUtf8(resolved.isObject() ? QJsonDocument(resolved.toObject()).toJson(QJsonDocument::Compact)
                                                             : QJsonDocument(resolved.toArray()).toJson(QJsonDocument::Compact));
            } else if (resolved.isDouble()) {
                out += QString::number(resolved.toDouble(), 'g', QLocale::FloatingPointShortest);
            } else if (resolved.isBool()) {
                out += resolved.toBool() ? QStringLiteral("true") : QStringLiteral("false");
            } else {
                out += QStringLiteral("null");
            }
            from = match.capturedEnd();
        }
        out += text.mid(from);
        return QJsonValue(out);
    }
    if (value.isArray()) {
        QJsonArray out;
        for (const auto& item : value.toArray()) {
            TRY_INTO(const auto resolved, resolveReferences(item, results, current))
            out.append(resolved);
        }
        return QJsonValue(out);
    }
    if (value.isObject()) {
        QJsonObject out;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            TRY_INTO(const auto resolved, resolveReferences(it.value(), results, current))
            out.insert(it.key(), resolved);
        }
        return QJsonValue(out);
    }
    return value;
}

void registerOpTools(ToolRegistry& tools, const ToolRegistry* ops)
{
    using namespace Schema;

    {
        Tool tool;
        tool.name = "search_ops";
        tool.title = "Find launcher operations";
        tool.description =
            "Searches the catalog of launcher operations run through `call` and `batch`: instance management (rename, copy, groups, "
            "components and loaders), content (list, enable, remove, update mods and packs), worlds, accounts, Java, and more. "
            "Search with a few words or browse one area. detail=names lists names only, signatures (default) adds a summary and "
            "arguments, full adds the description and the input and output JSON schemas; ask for full before calling an op you "
            "haven't used, or before reading its result through a batch $N reference. The same ops are launcher.<area>.<op>() "
            "inside execute. Returns {ops: [{name, scope, readOnly, destructive?, summary?, arguments?, description?, "
            "inputSchema?, outputSchema?}], count, areas, didYouMean?}.";
        tool.inputSchema = object({
            { "query", string("Words that must all appear in the op's name or description; empty lists everything. "
                              "Example: \"update mods\"") },
            { "area", string("Only ops of this area (the part before the dot; `areas` in any result lists them). "
                             "Example: \"components\"") },
            { "detail", string("How much to return per op (default signatures). Example: \"full\"", { "names", "signatures", "full" }) },
            { "limit", integer("Maximum ops returned (default 50); `count` still reports every match. Example: 10", 1, 500) },
        });
        tool.outputSchema = object({ { "ops", array(object({}, {}, false)) },
                                     { "count", integer({}) },
                                     { "areas", array(string({})) },
                                     { "didYouMean", array(string({})) } },
                                   { "ops", "count", "areas" }, false);
        tool.scope = Scope::Read;
        tool.alwaysLoad = true;
        tool.handler = [ops](const CallContext&, const QJsonObject& arguments, const ToolReply& reply) { searchOps(ops, arguments, reply); };
        tools.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "call";
        tool.title = "Run a launcher operation";
        tool.description =
            "Runs one operation from the catalog (see search_ops) with its arguments, with the same validation as a direct tool, "
            "and returns the op's own result (its outputSchema in search_ops detail=full). For several ops use batch; for "
            "loops or conditions use execute. Each op needs a scope; write ops may ask the user to confirm first. Errors carry "
            "a code and a hint.";
        tool.inputSchema = object(
            {
                { "op", string("Catalog op name from search_ops. Example: \"components.list\"") },
                { "arguments", withDescription(object({}, {}, false),
                                               "The op's arguments, per its inputSchema (empty object when it takes none). "
                                               "Example: {\"instance\": \"1.21.1 Fabric\"}") },
            },
            { "op" });
        tool.annotations.readOnly = false;
        tool.annotations.destructive = true;
        tool.annotations.idempotent = false;
        tool.scope = Scope::Read;
        tool.delegatesConfirmation = true;
        tool.handler = [ops](const CallContext& context, const QJsonObject& arguments, const ToolReply& reply) {
            callOp(ops, context, arguments, reply);
        };
        tools.add(std::move(tool));
    }

    {
        Tool tool;
        tool.name = "batch";
        tool.title = "Run several launcher operations";
        tool.description = QString(
            "Runs a known sequence of catalog ops (search_ops finds them) in one call, in order, passing results forward; use "
            "execute instead when the next step depends on loops or conditions. Every op name and scope is checked, and every "
            "confirmation the steps need is asked once, before the first step runs. Stops at the first failure unless "
            "continueOnError is true. Not transactional: ops that ran stay applied (files and downloads cannot be rolled back), "
            "and the result lists each step's outcome so you know exactly what ran. At most %1 steps. A string argument "
            "\"$N.path\" becomes that field of step N's result, keeping its type (\"$0.instances.2.id\"; \"$N\" is the whole "
            "result); \"${N.path}\" inside a longer string is replaced by its text; \"$$x\" is the literal \"$x\". "
            "Returns {results: [{index, op, ok, result | error, code, hint, seconds}], completed, total, stoppedAt?, failures?, "
            "label?, replayed?}.")
                               .arg(MAX_BATCH_OPS);
        tool.inputSchema = object(
            {
                { "ops", [] {
                     auto steps = array(
                         object({ { "op", string("Catalog op name from search_ops. Example: \"content.list\"") },
                                  { "arguments", withDescription(object({}, {}, false),
                                                                 "The op's arguments per its inputSchema; strings may hold $N "
                                                                 "references. Example: {\"instance\": \"$0.id\", \"kind\": \"mods\"}") } },
                                { "op" }),
                         QString("Steps to run in order, 1 to %1. Example: [{\"op\": \"groups.list\"}, {\"op\": \"instance.set_group\", "
                                 "\"arguments\": {\"instance\": \"abc\", \"group\": \"$0.groups.0\"}}]")
                             .arg(MAX_BATCH_OPS));
                     steps["minItems"] = 1;
                     steps["maxItems"] = MAX_BATCH_OPS;
                     return steps;
                 }() },
                { "continueOnError", boolean("Keep going after a failed step and report every outcome (default false: stop at the "
                                             "first failure). Example: true") },
                { "label", string("A short name for this batch, shown in progress messages and echoed in the result. "
                                  "Example: \"Add performance mods\"") },
                { "operation_id", [] {
                     auto id = string("Idempotency key: resending the same call with the same operation_id within an hour returns "
                                      "the first run's result (replayed: true) instead of applying the steps twice. Use a new id "
                                      "per operation. Example: \"add-sodium-2026-11-22\"");
                     id["maxLength"] = MAX_OPERATION_ID_LENGTH;
                     return id;
                 }() },
            },
            { "ops" });
        tool.outputSchema = object({ { "results", array(object({}, {}, false)) },
                                     { "completed", integer({}) },
                                     { "total", integer({}) },
                                     { "stoppedAt", integer({}) },
                                     { "failures", integer({}) },
                                     { "label", string({}) },
                                     { "replayed", boolean({}) } },
                                   { "results", "completed", "total" }, false);
        tool.annotations.readOnly = false;
        tool.annotations.destructive = true;
        tool.annotations.idempotent = false;
        tool.scope = Scope::Read;
        tool.delegatesConfirmation = true;
        tool.handler = [ops](const CallContext& context, const QJsonObject& arguments, const ToolReply& reply) {
            runBatch(ops, context, arguments, reply);
        };
        tools.add(std::move(tool));
    }
}

}  // namespace Mcp
