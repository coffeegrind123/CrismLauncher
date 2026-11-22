// SPDX-License-Identifier: GPL-3.0-only
#include "McpReplay.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>

namespace Mcp {

namespace {
constexpr auto OPERATION_ID = "operation_id";

QString key(const QString& tokenId, const QString& tool, const QString& operationId)
{
    return tokenId + '\n' + tool + '\n' + operationId;
}
}  // namespace

QByteArray argumentsDigest(const QJsonObject& arguments)
{
    auto copy = arguments;
    copy.remove(OPERATION_ID);
    return QCryptographicHash::hash(QJsonDocument(copy).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}

void ReplayCache::prune(qint64 now)
{
    // A run that never finished (its handler never replied) expires too, or its id would stay busy forever
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (now - it->created > REPLAY_TTL_MS) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    // Still too many: drop the oldest finished ones
    while (m_entries.size() > MAX_REPLAY_ENTRIES) {
        auto oldest = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (!it->running && (oldest == m_entries.end() || it->created < oldest->created)) {
                oldest = it;
            }
        }
        if (oldest == m_entries.end()) {
            break;
        }
        m_entries.erase(oldest);
    }
}

void ReplayCache::run(const QString& tokenId,
                      const QString& tool,
                      const QString& operationId,
                      const QJsonObject& arguments,
                      const ToolReply& reply,
                      const std::function<void(const Finish&)>& work)
{
    if (operationId.isEmpty()) {
        work([reply](const ToolResult& result, bool) { reply(result); });
        return;
    }

    const auto now = QDateTime::currentMSecsSinceEpoch();
    prune(now);
    const auto entryKey = key(tokenId, tool, operationId);
    const auto digest = argumentsDigest(arguments);

    if (const auto it = m_entries.constFind(entryKey); it != m_entries.constEnd()) {
        if (it->argumentsDigest != digest) {
            reply(ToolResult::error(ErrorCode::Conflict,
                                    QString("operation_id '%1' was already used for different arguments. Nothing was run.").arg(operationId),
                                    "Use a new operation_id for a new operation."));
            return;
        }
        if (it->running) {
            reply(ToolResult::error(ErrorCode::Busy, QString("operation_id '%1' is still running. Nothing was run again.").arg(operationId),
                                    "Wait, then resend the same call to get its result."));
            return;
        }
        auto replayed = it->result;
        if (replayed.structured.isObject()) {
            auto object = replayed.structured.toObject();
            object["replayed"] = true;
            replayed.structured = object;
        }
        replayed.text = "(replayed: this operation_id already ran; nothing was applied again)\n" + replayed.text;
        reply(replayed);
        return;
    }

    m_entries.insert(entryKey, Entry{ digest, now, true, {} });
    work([this, entryKey, reply](const ToolResult& result, bool applied) {
        auto it = m_entries.find(entryKey);
        if (it != m_entries.end()) {
            if (applied && !result.inputRequired) {
                it->running = false;
                it->result = result;
                it->created = QDateTime::currentMSecsSinceEpoch();
            } else {
                m_entries.erase(it);
            }
        }
        reply(result);
    });
}

void ReplayCache::clear()
{
    m_entries.clear();
}

ReplayCache& replayCache()
{
    static ReplayCache s_cache;
    return s_cache;
}

}  // namespace Mcp
