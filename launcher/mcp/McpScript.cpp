// SPDX-License-Identifier: GPL-3.0-only
#include "McpScript.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include <quickjs.h>

#include "mcp/McpReplay.h"
#include "mcp/McpSchema.h"
#include "mcp/McpTool.h"

namespace Mcp {

namespace {

constexpr int WATCHDOG_INTERVAL_MS = 200;
constexpr auto SCRIPT_FILE_NAME = "execute.js";

// Tools a script cannot call: the catalog wrappers (a script calls ops directly) and execute itself
const QStringList& metaTools()
{
    static const QStringList s_names{ "batch", "call", "execute", "search_ops" };
    return s_names;
}

// Builds the `launcher` and `console` globals from the op and tool names, then removes the host functions it wraps, so a
// script only ever sees the frozen API. LauncherError carries an op's error code, so scripts can branch on it:
//   try { await launcher.content.remove(...) } catch (e) { if (e.code === "not_found") ... }
constexpr auto PRELUDE = R"js(
(function (ops, tools) {
    "use strict";
    const hostOp = globalThis.__launcher_op, hostLog = globalThis.__launcher_log, hostSleep = globalThis.__launcher_sleep;
    delete globalThis.__launcher_op;
    delete globalThis.__launcher_log;
    delete globalThis.__launcher_sleep;

    class LauncherError extends Error {
        constructor(e) {
            super(e.message);
            this.name = "LauncherError";
            this.code = e.code;
            this.op = e.op;
            if (e.hint) {
                this.hint = e.hint;
            }
        }
    }

    const call = async (name, args) => {
        if (args !== undefined && (args === null || typeof args !== "object" || Array.isArray(args))) {
            throw new TypeError(name + ": the arguments must be an object, e.g. {instance: \"abc\"}");
        }
        try {
            return await hostOp(String(name), JSON.stringify(args === undefined ? {} : args));
        } catch (e) {
            if (e && e.__launcher) {
                throw new LauncherError(e);
            }
            throw e;
        }
    };

    const launcher = { call, sleep: (ms) => hostSleep(Number(ms) || 0) };
    for (const name of tools) {
        if (!(name in launcher)) {
            launcher[name] = (args) => call(name, args);
        }
    }
    for (const name of ops) {
        const dot = name.indexOf(".");
        const area = name.slice(0, dot), op = name.slice(dot + 1);
        if (typeof launcher[area] === "function") {
            continue;
        }
        (launcher[area] ??= {})[op] = (args) => call(name, args);
    }
    for (const value of Object.values(launcher)) {
        if (value && typeof value === "object") {
            Object.freeze(value);
        }
    }
    Object.freeze(launcher);

    const text = (values) => values.map((v) => {
        if (typeof v === "string") {
            return v;
        }
        if (v instanceof Error) {
            return String(v);
        }
        try {
            const json = JSON.stringify(v);
            return json === undefined ? String(v) : json;
        } catch (e) {
            return String(v);
        }
    }).join(" ");
    const console = Object.freeze({
        log: (...v) => hostLog("log", text(v)),
        info: (...v) => hostLog("info", text(v)),
        debug: (...v) => hostLog("debug", text(v)),
        warn: (...v) => hostLog("warn", text(v)),
        error: (...v) => hostLog("error", text(v)),
    });
    Object.defineProperty(globalThis, "launcher", { value: launcher });
    Object.defineProperty(globalThis, "console", { value: console });
    Object.defineProperty(globalThis, "LauncherError", { value: LauncherError });
})
)js";

std::optional<ErrorCode> errorCodeFromName(const QString& name)
{
    static const QList<ErrorCode> s_all{ ErrorCode::InvalidArgument, ErrorCode::UnknownOp, ErrorCode::NotFound,    ErrorCode::ScopeMissing,
                                         ErrorCode::Denied,          ErrorCode::Protected, ErrorCode::Busy,        ErrorCode::Conflict,
                                         ErrorCode::Unavailable,     ErrorCode::Failed,    ErrorCode::Cancelled,   ErrorCode::Timeout,
                                         ErrorCode::Internal,        ErrorCode::ScriptError, ErrorCode::OutOfMemory };
    for (const auto code : s_all) {
        if (errorCodeName(code) == name) {
            return code;
        }
    }
    return std::nullopt;
}

QString compactJson(const QJsonValue& value)
{
    // QJsonDocument only takes objects and arrays; a one-element array carries any value, then the brackets come off
    const auto text = QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(text.mid(1, text.size() - 2));
}

struct ScriptOptions {
    QString code;
    QString label;
    bool readOnly = false;
    int timeoutMs = DEFAULT_SCRIPT_TIMEOUT_SECONDS * 1000;
};

// One execute call: a QuickJS runtime that lives until the script's promise settles or a limit stops it. Ops run
// asynchronously on the launcher's event loop; each resolves (or rejects) the promise its call returned, and the job
// queue is drained again. Deleted with deleteLater() once it has replied, so late op replies find it gone (QPointer).
class ScriptRun : public QObject {
   public:
    enum class Stop { None, Timeout, Slice, Cancelled };

    ScriptRun(const ToolRegistry* tools, const ToolRegistry* ops, const CallContext& context, ScriptOptions options, ReplayCache::Finish finish)
        : m_tools(tools), m_ops(ops), m_outer(context), m_options(std::move(options)), m_finish(std::move(finish))
    {
        // Ops the script calls get their own cancellation flag: set when the caller cancels or the script stops, so a
        // long op (a download, jobs action=wait) ends with it
        m_inner = context;
        m_inner.cancelled = std::make_shared<bool>(false);
        m_inner.confirm = context.confirmInLauncher;
        m_inner.progress = [](double, std::optional<double>, const QString&) {};
    }

    ~ScriptRun() override { release(); }

    void start()
    {
        m_clock.start();
        m_runtime = JS_NewRuntime();
        if (!m_runtime) {
            fail(ErrorCode::Internal, "Could not start the JavaScript engine.");
            return;
        }
        JS_SetMemoryLimit(m_runtime, MAX_SCRIPT_MEMORY_BYTES);
        JS_SetMaxStackSize(m_runtime, MAX_SCRIPT_STACK_BYTES);
        JS_SetInterruptHandler(m_runtime, &ScriptRun::interrupt, this);
        m_context = JS_NewContext(m_runtime);
        if (!m_context) {
            fail(ErrorCode::Internal, "Could not start the JavaScript engine.");
            return;
        }
        JS_SetContextOpaque(m_context, this);

        auto* watchdog = new QTimer(this);
        watchdog->setInterval(WATCHDOG_INTERVAL_MS);
        connect(watchdog, &QTimer::timeout, this, &ScriptRun::checkWatchdog);
        watchdog->start();

        enterJs();
        // The synchronous part before the first await counts as a slice too
        m_slice.start();
        const auto global = JS_GetGlobalObject(m_context);
        JS_SetPropertyStr(m_context, global, "__launcher_op", JS_NewCFunction2(m_context, &ScriptRun::hostOp, "op", 2, JS_CFUNC_generic, 0));
        JS_SetPropertyStr(m_context, global, "__launcher_log", JS_NewCFunction2(m_context, &ScriptRun::hostLog, "log", 2, JS_CFUNC_generic, 0));
        JS_SetPropertyStr(m_context, global, "__launcher_sleep",
                          JS_NewCFunction2(m_context, &ScriptRun::hostSleep, "sleep", 1, JS_CFUNC_generic, 0));
        JS_FreeValue(m_context, global);

        QStringList opNames;
        for (const auto& op : m_ops->tools()) {
            opNames.append(op.name);
        }
        QStringList toolNames;
        for (const auto& tool : m_tools->tools()) {
            if (!metaTools().contains(tool.name)) {
                toolNames.append(tool.name);
            }
        }
        const auto prelude = QString("%1(%2, %3);")
                                 .arg(QString::fromUtf8(PRELUDE), compactJson(QJsonArray::fromStringList(opNames)),
                                      compactJson(QJsonArray::fromStringList(toolNames)))
                                 .toUtf8();
        const auto preludeResult = JS_Eval(m_context, prelude.constData(), static_cast<size_t>(prelude.size()), "prelude.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(preludeResult)) {
            failWithException(JS_GetException(m_context));
            return;
        }
        if (m_stop != Stop::None) {
            failStopped();
            return;
        }
        JS_FreeValue(m_context, preludeResult);

        // Same line as the code's first line, so the line numbers in errors are the script's own
        const auto source = QString("(async () => {%1\n})()").arg(m_options.code).toUtf8();
        m_promise = JS_Eval(m_context, source.constData(), static_cast<size_t>(source.size()), SCRIPT_FILE_NAME, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(m_promise)) {
            m_promise = JS_UNDEFINED;
            const auto exception = JS_GetException(m_context);
            if (m_stop != Stop::None) {
                JS_FreeValue(m_context, exception);
                failStopped();
                return;
            }
            failWithException(exception);
            return;
        }
        drive();
    }

   private:
    enum class PendingKind { Op, Sleep };
    struct Pending {
        PendingKind kind = PendingKind::Op;
        JSValue resolve = JS_UNDEFINED;
        JSValue reject = JS_UNDEFINED;
    };

    // Called by QuickJS every few thousand instructions; non-zero stops the script with an uncatchable error
    static int interrupt(JSRuntime*, void* opaque) { return static_cast<ScriptRun*>(opaque)->shouldStop() ? 1 : 0; }

    bool shouldStop()
    {
        if (m_stop == Stop::None) {
            if (*m_outer.cancelled) {
                m_stop = Stop::Cancelled;
            } else if (m_clock.elapsed() > m_options.timeoutMs) {
                m_stop = Stop::Timeout;
            } else if (m_slice.isValid() && m_slice.elapsed() > MAX_SCRIPT_SLICE_MS) {
                m_stop = Stop::Slice;
            }
        }
        return m_stop != Stop::None;
    }

    static ScriptRun* self(JSContext* ctx) { return static_cast<ScriptRun*>(JS_GetContextOpaque(ctx)); }

    static QString toQString(JSContext* ctx, JSValueConst value)
    {
        size_t length = 0;
        const char* text = JS_ToCStringLen(ctx, &length, value);
        if (!text) {
            // A throwing toString(): drop that exception, it must not surface as the next call's
            if (JS_HasException(ctx)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            return {};
        }
        auto out = QString::fromUtf8(text, static_cast<qsizetype>(length));
        JS_FreeCString(ctx, text);
        return out;
    }

    // A promise the host settles later; the resolving functions are kept until then
    JSValue newPending(PendingKind kind, int* id)
    {
        JSValue resolving[2];
        const auto promise = JS_NewPromiseCapability(m_context, resolving);
        if (JS_IsException(promise)) {
            return promise;
        }
        *id = ++m_nextId;
        m_pending.insert(*id, Pending{ kind, resolving[0], resolving[1] });
        return promise;
    }

    // launcher.<op>(args) -> __launcher_op(name, argumentsJson): a promise settled by the op's reply
    static JSValue hostOp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 2) {
            return JS_ThrowTypeError(ctx, "op needs a name and arguments");
        }
        auto* run = self(ctx);
        int id = 0;
        const auto promise = run->newPending(PendingKind::Op, &id);
        if (JS_IsException(promise)) {
            return promise;
        }
        // Dispatched from the event loop, never from inside JavaScript: an op that replies at once then settles its
        // promise between jobs instead of re-entering the engine
        QTimer::singleShot(0, run, [run, id, name = toQString(ctx, argv[0]), arguments = toQString(ctx, argv[1])] {
            run->dispatch(id, name, arguments);
        });
        return promise;
    }

    static JSValue hostSleep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        double ms = 0;
        if (argc > 0 && JS_ToFloat64(ctx, &ms, argv[0]) < 0) {
            return JS_EXCEPTION;
        }
        auto* run = self(ctx);
        int id = 0;
        const auto promise = run->newPending(PendingKind::Sleep, &id);
        if (JS_IsException(promise)) {
            return promise;
        }
        const auto remaining = std::max<qint64>(0, run->m_options.timeoutMs - run->m_clock.elapsed());
        const auto delay = static_cast<int>(std::clamp<qint64>(static_cast<qint64>(ms), 0, remaining));
        QTimer::singleShot(delay, run, [run, id] { run->settle(id, true, JS_UNDEFINED); });
        return promise;
    }

    static JSValue hostLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        auto* run = self(ctx);
        const auto level = argc > 0 ? toQString(ctx, argv[0]) : QStringLiteral("log");
        const auto text = argc > 1 ? toQString(ctx, argv[1]) : QString();
        run->log(level, text);
        return JS_UNDEFINED;
    }

    void log(const QString& level, const QString& text)
    {
        const auto bytes = static_cast<int>(text.toUtf8().size());
        if (m_logs.size() >= MAX_SCRIPT_LOG_LINES || m_logBytes + bytes > MAX_SCRIPT_LOG_BYTES) {
            ++m_logsDropped;
            return;
        }
        m_logBytes += bytes;
        m_logs.append(level == "log" ? text : QString("[%1] %2").arg(level, text));
    }

    const Tool* callable(const QString& name) const
    {
        if (const auto* op = m_ops->find(name)) {
            return op;
        }
        if (metaTools().contains(name)) {
            return nullptr;
        }
        return m_tools->find(name);
    }

    void rejectOp(int id, const QString& op, ErrorCode code, const QString& message, const QString& hint = {})
    {
        recordOp(op, false, code, 0);
        settleError(id, op, code, message, hint);
    }

    void dispatch(int id, const QString& name, const QString& argumentsJson)
    {
        if (m_finished || !m_pending.contains(id)) {
            return;
        }
        const auto* tool = callable(name);
        if (!tool) {
            QStringList names;
            for (const auto& op : m_ops->tools()) {
                names.append(op.name);
            }
            for (const auto& entry : m_tools->tools()) {
                if (!metaTools().contains(entry.name)) {
                    names.append(entry.name);
                }
            }
            const auto close = closestMatches(name, names);
            rejectOp(id, name, ErrorCode::UnknownOp, QString("Unknown op '%1'.").arg(name),
                     close.isEmpty() ? QString("search_ops lists the ops; launcher.<area>.<op>() calls one.")
                                     : QString("Did you mean: %1?").arg(close.join(", ")));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(argumentsJson.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            rejectOp(id, name, ErrorCode::InvalidArgument, QString("%1: the arguments must be a JSON object.").arg(name));
            return;
        }
        const auto arguments = document.object();
        if (m_options.readOnly && tool->effectFor(arguments) != Effect::Read) {
            rejectOp(id, name, ErrorCode::Denied, QString("%1 changes things, and this script runs with read_only.").arg(name),
                     "Run it again without read_only to allow writes.");
            return;
        }
        if (m_opCount >= MAX_SCRIPT_OPS) {
            rejectOp(id, name, ErrorCode::Failed, QString("The script reached its limit of %1 op calls.").arg(MAX_SCRIPT_OPS),
                     "Split the work into several execute calls.");
            return;
        }

        ++m_opCount;
        if (m_outer.progress) {
            m_outer.progress(static_cast<double>(m_opCount), std::nullopt, name);
        }
        auto timer = std::make_shared<QElapsedTimer>();
        timer->start();
        const QPointer<ScriptRun> guard(this);
        invokeTool(*tool, m_inner, arguments, [guard, id, name, timer](const ToolResult& result) {
            if (guard) {
                guard->opReplied(id, name, result, timer->elapsed());
            }
        });
    }

    void opReplied(int id, const QString& name, const ToolResult& result, qint64 elapsedMs)
    {
        if (m_finished || !m_pending.contains(id)) {
            return;
        }
        if (result.inputRequired) {
            // Cannot happen (ops here ask in the launcher window), but an op must never wait on a chat answer mid-script
            recordOp(name, false, ErrorCode::Denied, elapsedMs);
            settleError(id, name, ErrorCode::Denied, QString("%1 needs the user's confirmation, which a script cannot ask in the chat.").arg(name),
                        "Run that op with call instead.");
            return;
        }
        recordOp(name, !result.isError, result.code, elapsedMs);
        if (result.isError) {
            const auto error = result.structured.toObject();
            settleError(id, name, result.code, error.value("error").toString(result.text), error.value("hint").toString());
            return;
        }
        enterJs();
        const auto json = compactJson(result.structured.isUndefined() ? QJsonValue() : result.structured).toUtf8();
        const auto value = JS_ParseJSON(m_context, json.constData(), static_cast<size_t>(json.size()), "result");
        if (JS_IsException(value)) {
            settle(id, false, JS_GetException(m_context));
            return;
        }
        settle(id, true, value);
    }

    void settleError(int id, const QString& op, ErrorCode code, const QString& message, const QString& hint)
    {
        enterJs();
        QJsonObject error{ { "__launcher", true }, { "code", errorCodeName(code) }, { "op", op }, { "message", message } };
        if (!hint.isEmpty()) {
            error["hint"] = hint;
        }
        const auto json = QJsonDocument(error).toJson(QJsonDocument::Compact);
        const auto value = JS_ParseJSON(m_context, json.constData(), static_cast<size_t>(json.size()), "error");
        settle(id, false, JS_IsException(value) ? JS_GetException(m_context) : value);
    }

    // Resolves or rejects pending promise `id` with `value` (ownership taken), then lets the script continue
    void settle(int id, bool ok, JSValue value)
    {
        const auto it = m_pending.find(id);
        if (m_finished || it == m_pending.end()) {
            if (m_context) {
                JS_FreeValue(m_context, value);
            }
            return;
        }
        const auto pending = *it;
        m_pending.erase(it);
        enterJs();
        const auto called = JS_Call(m_context, ok ? pending.resolve : pending.reject, JS_UNDEFINED, 1, &value);
        JS_FreeValue(m_context, called);
        JS_FreeValue(m_context, value);
        JS_FreeValue(m_context, pending.resolve);
        JS_FreeValue(m_context, pending.reject);
        drive();
    }

    void recordOp(const QString& op, bool ok, ErrorCode code, qint64 elapsedMs)
    {
        if (m_opLog.size() >= MAX_SCRIPT_OPS_LISTED) {
            ++m_opLogDropped;
            return;
        }
        QJsonObject entry{ { "op", op }, { "ok", ok }, { "ms", elapsedMs } };
        if (!ok) {
            entry["code"] = errorCodeName(code);
        }
        m_opLog.append(entry);
    }

    // QuickJS measures its stack limit from the stack top it last saw; event-loop callbacks enter at different depths
    void enterJs()
    {
        if (m_runtime) {
            JS_UpdateStackTop(m_runtime);
        }
    }

    // Runs every queued job, then looks at the script's promise
    void drive()
    {
        if (m_finished) {
            return;
        }
        enterJs();
        m_slice.start();
        for (;;) {
            JSContext* jobContext = nullptr;
            const int ran = JS_ExecutePendingJob(m_runtime, &jobContext);
            if (ran == 0) {
                break;
            }
            if (ran < 0) {
                const auto exception = JS_GetException(jobContext ? jobContext : m_context);
                if (m_stop != Stop::None) {
                    JS_FreeValue(m_context, exception);
                    failStopped();
                    return;
                }
                failWithException(exception);
                return;
            }
        }
        m_slice.invalidate();
        if (m_stop != Stop::None) {
            failStopped();
            return;
        }

        switch (JS_PromiseState(m_context, m_promise)) {
            case JS_PROMISE_FULFILLED:
                succeed(JS_PromiseResult(m_context, m_promise));
                return;
            case JS_PROMISE_REJECTED:
                failWithException(JS_PromiseResult(m_context, m_promise));
                return;
            case JS_PROMISE_PENDING:
                if (m_pending.isEmpty()) {
                    fail(ErrorCode::ScriptError, "The script is waiting on a promise that nothing will settle.",
                         "Await only launcher calls, launcher.sleep() and promises built from them; there are no timers or I/O.");
                }
                return;
            default:
                fail(ErrorCode::Internal, "The script did not produce a promise.");
                return;
        }
    }

    void checkWatchdog()
    {
        if (m_finished || !shouldStop()) {
            return;
        }
        // Not inside JavaScript (it would have been interrupted there): the script is waiting on ops
        failStopped();
    }

    void failStopped()
    {
        switch (m_stop) {
            case Stop::Cancelled:
                fail(ErrorCode::Cancelled, "The script was cancelled.");
                return;
            case Stop::Slice:
                fail(ErrorCode::Timeout,
                     QString("The script ran JavaScript for %1 s without awaiting anything, which freezes the launcher.").arg(MAX_SCRIPT_SLICE_MS / 1000),
                     "Await launcher calls inside long loops, or move heavy computation out of the script.");
                return;
            case Stop::Timeout:
            case Stop::None:
                fail(ErrorCode::Timeout, QString("The script did not finish within %1 s.").arg(m_options.timeoutMs / 1000),
                     "Ops it started may still be running or done (see ops); check the state before running it again, or raise "
                     "timeoutSeconds. Slow work belongs in a job: start it, return its id, then use jobs action=wait.");
                return;
        }
    }

    void failWithException(JSValue exception)
    {
        enterJs();
        QString message;
        QString stack;
        QString hint;
        QString op;
        auto code = ErrorCode::ScriptError;

        if (JS_IsObject(exception)) {
            const auto marker = JS_GetPropertyStr(m_context, exception, "name");
            const bool launcherError = toQString(m_context, marker) == "LauncherError";
            JS_FreeValue(m_context, marker);
            if (launcherError) {
                auto property = [this, exception](const char* name) {
                    const auto value = JS_GetPropertyStr(m_context, exception, name);
                    auto text = JS_IsUndefined(value) ? QString() : toQString(m_context, value);
                    JS_FreeValue(m_context, value);
                    return text;
                };
                code = errorCodeFromName(property("code")).value_or(ErrorCode::ScriptError);
                op = property("op");
                hint = property("hint");
                message = property("message");
            }
            const auto stackValue = JS_GetPropertyStr(m_context, exception, "stack");
            if (JS_IsString(stackValue)) {
                stack = toQString(m_context, stackValue);
            }
            JS_FreeValue(m_context, stackValue);
        }
        if (message.isEmpty()) {
            message = toQString(m_context, exception);
        }
        JS_FreeValue(m_context, exception);

        if (message.contains("out of memory")) {
            fail(ErrorCode::OutOfMemory, QString("The script went over its %1 MiB memory limit.").arg(static_cast<int>(MAX_SCRIPT_MEMORY_BYTES / (1024 * 1024))),
                 "Keep less in memory: summarise results inside the loop instead of collecting them.");
            return;
        }
        if (op.isEmpty()) {
            message = "Uncaught " + message;
            if (hint.isEmpty()) {
                hint = "The ops listed in `ops` already ran and stay applied; fix the script and run only what is left.";
            }
        } else {
            message = QString("Uncaught LauncherError from %1: %2").arg(op, message);
        }
        fail(code, message, hint, stack, op);
    }

    QJsonObject report() const
    {
        QJsonObject out{ { "logs", QJsonArray::fromStringList(m_logs) }, { "ops", m_opLog }, { "opCount", m_opCount }, { "ms", m_clock.elapsed() } };
        if (m_logsDropped > 0) {
            out["logsDropped"] = m_logsDropped;
        }
        if (m_opLogDropped > 0) {
            out["opsDropped"] = m_opLogDropped;
        }
        if (!m_options.label.isEmpty()) {
            out["label"] = m_options.label;
        }
        return out;
    }

    void succeed(JSValue value)
    {
        // {"v": value} so that undefined, numbers and strings serialise like objects do; undefined drops out as null
        const auto holder = JS_NewObject(m_context);
        JS_SetPropertyStr(m_context, holder, "v", value);
        const auto json = JS_JSONStringify(m_context, holder, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(m_context, holder);
        if (JS_IsException(json)) {
            failWithException(JS_GetException(m_context));
            return;
        }
        const auto text = toQString(m_context, json);
        JS_FreeValue(m_context, json);
        if (text.toUtf8().size() > MAX_SCRIPT_RESULT_BYTES) {
            fail(ErrorCode::ScriptError, QString("The returned value is over %1 KiB of JSON.").arg(MAX_SCRIPT_RESULT_BYTES / 1024),
                 "Return a summary, or the ids to look at, instead of whole results.");
            return;
        }
        auto out = report();
        out["value"] = QJsonDocument::fromJson(text.toUtf8()).object().value("v");
        if (out["value"].isUndefined()) {
            out["value"] = QJsonValue();
        }
        respond(ToolResult::ok(out));
    }

    void fail(ErrorCode code, const QString& message, const QString& hint = {}, const QString& stack = {}, const QString& op = {})
    {
        auto result = ToolResult::error(code, message, hint);
        auto structured = result.structured.toObject();
        const auto extra = report();
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            structured.insert(it.key(), it.value());
        }
        if (!stack.isEmpty()) {
            structured["stack"] = stack;
        }
        if (!op.isEmpty()) {
            structured["op"] = op;
        }
        result.structured = structured;
        // The text carries what a client without structuredContent needs: the error, then logs and the ops that ran
        QStringList lines{ result.text };
        if (!stack.isEmpty()) {
            lines.append(stack.trimmed());
        }
        lines.append(QString("%1 op call(s) ran before it stopped; they are not undone.").arg(m_opCount));
        if (!m_logs.isEmpty()) {
            lines.append("console:\n" + m_logs.join('\n'));
        }
        result.text = lines.join('\n');
        respond(result);
    }

    void respond(const ToolResult& result)
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        *m_inner.cancelled = true;
        release();
        m_finish(result, m_opCount > 0);
        deleteLater();
    }

    // Frees every value held for the engine, then the engine. Pending op replies arrive later and are ignored.
    void release()
    {
        if (m_context) {
            for (const auto& pending : std::as_const(m_pending)) {
                JS_FreeValue(m_context, pending.resolve);
                JS_FreeValue(m_context, pending.reject);
            }
            m_pending.clear();
            JS_FreeValue(m_context, m_promise);
            m_promise = JS_UNDEFINED;
            JS_FreeContext(m_context);
            m_context = nullptr;
        }
        if (m_runtime) {
            JS_FreeRuntime(m_runtime);
            m_runtime = nullptr;
        }
    }

    const ToolRegistry* m_tools;
    const ToolRegistry* m_ops;
    CallContext m_outer;
    CallContext m_inner;
    ScriptOptions m_options;
    ReplayCache::Finish m_finish;

    JSRuntime* m_runtime = nullptr;
    JSContext* m_context = nullptr;
    JSValue m_promise = JS_UNDEFINED;
    QHash<int, Pending> m_pending;
    int m_nextId = 0;

    QElapsedTimer m_clock;
    QElapsedTimer m_slice;
    Stop m_stop = Stop::None;
    bool m_finished = false;

    int m_opCount = 0;
    QJsonArray m_opLog;
    int m_opLogDropped = 0;
    QStringList m_logs;
    int m_logBytes = 0;
    int m_logsDropped = 0;
};

void runScript(const ToolRegistry* tools, const ToolRegistry* ops, const CallContext& context, const QJsonObject& arguments, const ToolReply& reply)
{
    ScriptOptions options;
    options.code = arguments.value("code").toString();
    options.label = arguments.value("label").toString();
    options.readOnly = arguments.value("read_only").toBool(false);
    options.timeoutMs = arguments.value("timeoutSeconds").toInt(DEFAULT_SCRIPT_TIMEOUT_SECONDS) * 1000;

    if (options.code.toUtf8().size() > MAX_SCRIPT_CODE_BYTES) {
        reply(ToolResult::error(ErrorCode::InvalidArgument, QString("code is over %1 KiB.").arg(MAX_SCRIPT_CODE_BYTES / 1024),
                                "Loop over data inside the script instead of writing it out."));
        return;
    }

    replayCache().run(context.token.id, "execute", arguments.value("operation_id").toString(), arguments, reply,
                      [tools, ops, context, options](const ReplayCache::Finish& finish) {
                          auto* run = new ScriptRun(tools, ops, context, options, finish);
                          run->start();
                      });
}

// A JSON Schema as a TypeScript type, for the subset the tools use
QString typeScript(const QJsonObject& schema, int depth)
{
    constexpr int MAX_DEPTH = 6;
    if (depth > MAX_DEPTH) {
        return "unknown";
    }
    if (const auto values = schema.value("enum").toArray(); !values.isEmpty()) {
        QStringList out;
        for (const auto& value : values) {
            out.append(compactJson(value));
        }
        return out.join(" | ");
    }
    const auto typeValue = schema.value("type");
    QStringList types;
    if (typeValue.isArray()) {
        for (const auto& type : typeValue.toArray()) {
            types.append(type.toString());
        }
    } else if (typeValue.isString()) {
        types.append(typeValue.toString());
    }
    if (types.isEmpty()) {
        return "unknown";
    }
    QStringList out;
    const QString indent(static_cast<qsizetype>(depth + 1) * 4, ' ');
    for (const auto& type : types) {
        if (type == "string") {
            out.append("string");
        } else if (type == "integer" || type == "number") {
            out.append("number");
        } else if (type == "boolean") {
            out.append("boolean");
        } else if (type == "null") {
            out.append("null");
        } else if (type == "array") {
            const auto items = typeScript(schema.value("items").toObject(), depth + 1);
            out.append(items.contains(' ') ? QString("Array<%1>").arg(items) : items + "[]");
        } else if (type == "object") {
            const auto properties = schema.value("properties").toObject();
            if (properties.isEmpty()) {
                out.append("Record<string, unknown>");
                continue;
            }
            QStringList required;
            for (const auto& name : schema.value("required").toArray()) {
                required.append(name.toString());
            }
            QString body = "{\n";
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                const auto property = it.value().toObject();
                if (const auto description = property.value("description").toString(); !description.isEmpty()) {
                    body += QString("%1/** %2 */\n").arg(indent, QString(description).replace("*/", "* /"));
                }
                body += QString("%1%2%3: %4;\n").arg(indent, it.key(), required.contains(it.key()) ? QString() : QString("?"),
                                                      typeScript(property, depth + 1));
            }
            if (schema.value("additionalProperties").toBool(true)) {
                body += indent + "[key: string]: unknown;\n";
            }
            body += QString(static_cast<qsizetype>(depth) * 4, ' ') + "}";
            out.append(body);
        } else {
            out.append("unknown");
        }
    }
    return out.join(" | ");
}

QString functionDeclaration(const Tool& tool, int depth)
{
    const QString indent(static_cast<qsizetype>(depth) * 4, ' ');
    auto summary = tool.description.section(". ", 0, 0);
    if (!summary.endsWith('.')) {
        summary += '.';
    }
    QString effect = tool.annotations.readOnly ? QString("read-only") : tool.annotations.destructive ? QString("destructive") : QString("writes");
    const auto name = tool.name.contains('.') ? tool.name.section('.', 1) : tool.name;
    const auto output = tool.outputSchema.isEmpty() ? QString("Record<string, unknown>") : typeScript(tool.outputSchema, depth);
    return QString("%1/** %2 (%3; scope %4) */\n%1function %5(args%6: %7): Promise<%8>;\n")
        .arg(indent, QString(summary).replace("*/", "* /"), effect, scopeName(tool.scope), name,
             tool.inputSchema.value("required").toArray().isEmpty() ? QString("?") : QString(), typeScript(tool.inputSchema, depth), output);
}
}  // namespace

QString scriptTypeDeclarations(const ToolRegistry& tools, const ToolRegistry& ops)
{
    QString out =
        "// The API a script run by the `execute` tool sees. Generated from the tools' and ops' JSON Schemas.\n"
        "// Every call returns a promise: `await` it. A failed call throws LauncherError.\n\n"
        "declare class LauncherError extends Error {\n"
        "    name: \"LauncherError\";\n"
        "    /** invalid_argument, unknown_op, not_found, scope_missing, denied, protected, busy, conflict, unavailable, failed, "
        "cancelled, timeout, internal */\n"
        "    code: string;\n"
        "    /** The op or tool that failed, e.g. \"content.remove\" */\n"
        "    op: string;\n"
        "    /** How to recover, when known */\n"
        "    hint?: string;\n"
        "}\n\n"
        "declare const console: { log(...values: unknown[]): void; info(...values: unknown[]): void; debug(...values: unknown[]): void; "
        "warn(...values: unknown[]): void; error(...values: unknown[]): void };\n\n"
        "declare namespace launcher {\n"
        "    /** Calls any op or tool by name: launcher.call(\"content.list\", {...}) is launcher.content.list({...}). */\n"
        "    function call(name: string, args?: Record<string, unknown>): Promise<any>;\n"
        "    /** Waits `ms` milliseconds (capped at the script's remaining time). */\n"
        "    function sleep(ms: number): Promise<void>;\n";
    for (const auto& tool : tools.tools()) {
        if (!metaTools().contains(tool.name)) {
            out += functionDeclaration(tool, 1);
        }
    }
    QString area;
    for (const auto& op : ops.tools()) {
        const auto opArea = op.name.section('.', 0, 0);
        if (opArea != area) {
            if (!area.isEmpty()) {
                out += "    }\n";
            }
            area = opArea;
            out += QString("    namespace %1 {\n").arg(area);
        }
        out += functionDeclaration(op, 2);
    }
    if (!area.isEmpty()) {
        out += "    }\n";
    }
    out += "}\n";
    return out;
}

void registerScriptTool(ToolRegistry& tools, const ToolRegistry* ops)
{
    using namespace Schema;

    Tool tool;
    tool.name = "execute";
    tool.title = "Run a JavaScript program against the launcher";
    tool.description = QString(
        "Runs JavaScript (QuickJS, ES2023) that drives the launcher through its whole API: every catalog op as "
        "`await launcher.<area>.<op>(args)` (search_ops lists them, e.g. launcher.content.list({instance, kind: \"mods\"})) and "
        "every direct tool as `await launcher.<tool>(args)` (launcher.instances(), launcher.instance_get({instance})). Use it for "
        "loops, conditions and computed arguments; use batch for a fixed sequence. `code` is the body of an async function: "
        "`return` gives the result (any JSON value), console.log output is captured. Each call returns the op's result object "
        "(its outputSchema) or throws LauncherError {code, op, message, hint}, which the script may catch; launcher.sleep(ms) "
        "waits. Ops go through the same scopes, validation and confirmations as direct calls (confirmations are asked in the "
        "launcher window). No file, network or process access of its own. Not transactional: ops that ran stay applied, and "
        "the result lists them in order. read_only=true refuses every op that changes something. Limits: timeoutSeconds "
        "(default %1, max %2), %3 s of JavaScript without an await, %4 MiB memory, %5 KiB code, %6 KiB result, %7 op calls, "
        "%8 log lines. Returns {value, logs, ops: [{op, ok, code?, ms}], opCount, ms, label?, replayed?}; an error adds "
        "{error, code, hint, op?, stack?} (code: script_error, timeout, out_of_memory, cancelled, or the op's own code).")
                           .arg(DEFAULT_SCRIPT_TIMEOUT_SECONDS)
                           .arg(MAX_SCRIPT_TIMEOUT_SECONDS)
                           .arg(MAX_SCRIPT_SLICE_MS / 1000)
                           .arg(static_cast<int>(MAX_SCRIPT_MEMORY_BYTES / (1024 * 1024)))
                           .arg(MAX_SCRIPT_CODE_BYTES / 1024)
                           .arg(MAX_SCRIPT_RESULT_BYTES / 1024)
                           .arg(MAX_SCRIPT_OPS)
                           .arg(MAX_SCRIPT_LOG_LINES);
    tool.inputSchema = object(
        {
            { "code", [] {
                 auto code = string(
                     "JavaScript, the body of an async function: await launcher calls, `return` the result. Example: "
                     "\"const {instances} = await launcher.instances(); const out = []; for (const i of instances) { const {mods} = "
                     "await launcher.content.list({instance: i.id, kind: 'mods'}); out.push({name: i.name, mods: mods.length}); } "
                     "return out;\"");
                 code["minLength"] = 1;
                 code["maxLength"] = MAX_SCRIPT_CODE_BYTES;
                 return code;
             }() },
            { "read_only", boolean("true refuses every op that changes something (they throw LauncherError code denied), so the "
                                   "script can only look. Default false. Example: true") },
            { "timeoutSeconds",
              integer(QString("Wall-clock limit for the whole script, waits for ops included (default %1). Example: 120").arg(DEFAULT_SCRIPT_TIMEOUT_SECONDS), 1,
                      MAX_SCRIPT_TIMEOUT_SECONDS) },
            { "label", string("A short name for this script, echoed in the result. Example: \"Disable client-only mods\"") },
            { "operation_id", [] {
                 auto id = string("Idempotency key: resending the same call with the same operation_id within an hour returns the "
                                  "first run's result (replayed: true) instead of running the script again. Use a new id per "
                                  "operation. Example: \"cleanup-2026-11-22\"");
                 id["maxLength"] = MAX_OPERATION_ID_LENGTH;
                 return id;
             }() },
        },
        { "code" });
    tool.outputSchema = object({ { "value", QJsonObject{ { "description", "What the script returned (null when nothing)" } } },
                                 { "logs", array(string({})) },
                                 { "logsDropped", integer({}) },
                                 { "ops", array(object({}, {}, false)) },
                                 { "opsDropped", integer({}) },
                                 { "opCount", integer({}) },
                                 { "ms", integer({}) },
                                 { "label", string({}) },
                                 { "replayed", boolean({}) } },
                               { "value", "logs", "ops", "opCount", "ms" }, false);
    tool.annotations.readOnly = false;
    tool.annotations.destructive = true;
    tool.annotations.idempotent = false;
    tool.scope = Scope::Read;
    tool.alwaysLoad = true;
    // Each op asks for itself, in the launcher window
    tool.delegatesConfirmation = true;
    tool.effect = [](const QJsonObject& arguments) { return arguments.value("read_only").toBool(false) ? Effect::Read : Effect::Destructive; };
    const auto* registry = &tools;
    tool.handler = [registry, ops](const CallContext& context, const QJsonObject& arguments, const ToolReply& reply) {
        runScript(registry, ops, context, arguments, reply);
    };
    tools.add(std::move(tool));
}

}  // namespace Mcp
