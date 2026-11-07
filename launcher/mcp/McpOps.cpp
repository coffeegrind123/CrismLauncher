// SPDX-License-Identifier: GPL-3.0-only
#include "McpOps.h"

#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <memory>

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
    reply(ToolResult::error(QString("Unknown op '%1'.").arg(name),
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

QJsonObject resultEntry(const QString& op, const ToolResult& result)
{
    QJsonObject entry{ { "op", op }, { "ok", !result.isError } };
    if (result.isError) {
        entry["error"] = result.text;
    } else {
        entry["result"] = result.structured;
    }
    return entry;
}

void runBatch(const ToolRegistry* ops, const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    const auto steps = arguments.value("ops").toArray();
    const bool continueOnError = arguments.value("continueOnError").toBool(false);

    // Every op must exist and be allowed before anything runs, so a typo in step 9 doesn't leave steps 1-8 applied
    for (qsizetype i = 0; i < steps.size(); ++i) {
        const auto name = steps[i].toObject().value("op").toString();
        const auto* op = ops->find(name);
        if (!op) {
            const auto close = ops->suggest(name);
            reply(ToolResult::error(QString("Step %1: unknown op '%2'. Nothing was run.").arg(i).arg(name),
                                    close.isEmpty() ? QString("Use search_ops to find ops.") : QString("Did you mean: %1?").arg(close.join(", "))));
            return;
        }
        if (!context.token.scopes.testFlag(op->scope)) {
            reply(ToolResult::error(
                QString("Step %1: this token lacks the '%2' scope that %3 needs. Nothing was run.").arg(i).arg(scopeName(op->scope), name)));
            return;
        }
    }

    struct State {
        QJsonArray steps;
        QJsonArray results;
        qsizetype next = 0;
        int failures = 0;
    };
    auto state = std::make_shared<State>();
    state->steps = steps;

    auto finish = [state, reply, continueOnError] {
        QJsonObject result{ { "results", state->results }, { "completed", state->results.size() }, { "total", state->steps.size() } };
        if (state->results.size() < state->steps.size()) {
            result["stoppedAt"] = state->results.size() - 1;
        }
        if (state->failures > 0) {
            result["failures"] = state->failures;
        }
        auto out = ToolResult::ok(result);
        // Ops that already ran are not undone; the agent sees exactly which ones ran
        out.isError = state->failures > 0 && !continueOnError;
        reply(out);
    };

    // Each pending op's callback holds the only strong reference to `step`, so the chain frees itself when it ends
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakStep = step;
    *step = [ops, context, continueOnError, state, finish, weakStep] {
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

        auto onResult = [continueOnError, state, finish, self, name](const ToolResult& result) {
            state->results.append(resultEntry(name, result));
            if (result.isError) {
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
            onResult(ToolResult::error(QString("Step %1: %2").arg(index).arg(resolved.error())));
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
}  // namespace

Result<QJsonValue> resolveReferences(const QJsonValue& value, const QJsonArray& results, int current)
{
    static const QRegularExpression s_reference(R"(^\$(\d+)(?:\.(.+))?$)");

    if (value.isString()) {
        const auto text = value.toString();
        if (text.startsWith("$$")) {
            return QJsonValue(text.mid(1));
        }
        const auto match = s_reference.match(text);
        if (!match.hasMatch()) {
            return value;
        }
        const int index = match.captured(1).toInt();
        if (index >= current) {
            return std::unexpected(QString("'%1' refers to step %2, which has not run yet").arg(text).arg(index));
        }
        const auto entry = results.at(index).toObject();
        if (!entry.value("ok").toBool()) {
            return std::unexpected(QString("'%1' refers to step %2, which failed").arg(text).arg(index));
        }
        auto resolved = lookupPath(entry.value("result"), match.captured(2));
        if (!resolved) {
            return std::unexpected(QString("'%1': %2").arg(text, resolved.error()));
        }
        return *resolved;
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
            "arguments, full adds the description and JSON schemas; ask for full before calling an op you haven't used.";
        tool.inputSchema = object({
            { "query", string("Words that must all appear in the op's name or description; empty lists everything") },
            { "area", string("Only ops of this area, e.g. 'components'") },
            { "detail", string("How much to return (default signatures)", { "names", "signatures", "full" }) },
            { "limit", integer("Maximum ops returned (default 50)", 1, 500) },
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
            "Runs one operation from the catalog (see search_ops) with its arguments, and returns its result. Each op needs a scope; "
            "write ops may ask the user to confirm in the launcher first.";
        tool.inputSchema = object(
            {
                { "op", string("Op name, e.g. 'components.list'") },
                { "arguments", withDescription(object({}, {}, false), "The op's arguments, per its inputSchema") },
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
        tool.description =
            "Runs catalog ops in order. Every op name and scope is checked before the first one runs. Stops at the first failure "
            "unless continueOnError is true. Ops that ran are not undone, and the result lists each step's outcome. A string "
            "argument \"$N.path\" is replaced by that path of step N's result (\"$0.instances.2.id\"); \"$N\" is the whole result; "
            "\"$$x\" is the literal \"$x\".";
        tool.inputSchema = object(
            {
                { "ops", [] {
                     auto steps = array(object({ { "op", string("Op name") }, { "arguments", object({}, {}, false) } }, { "op" }));
                     steps["minItems"] = 1;
                     steps["maxItems"] = MAX_BATCH_OPS;
                     return steps;
                 }() },
                { "continueOnError", boolean("Keep going after a failed step (default false)") },
            },
            { "ops" });
        tool.outputSchema = object({ { "results", array(object({}, {}, false)) },
                                     { "completed", integer({}) },
                                     { "total", integer({}) },
                                     { "stoppedAt", integer({}) },
                                     { "failures", integer({}) } },
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
