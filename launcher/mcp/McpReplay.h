// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>

#include "mcp/McpTool.h"

namespace Mcp {

constexpr qint64 REPLAY_TTL_MS = 60LL * 60 * 1000;
constexpr int MAX_REPLAY_ENTRIES = 500;
constexpr int MAX_OPERATION_ID_LENGTH = 128;

// operation_id on batch and execute: a resend after a dropped connection or a client timeout returns the first run's
// result (marked replayed) instead of applying the writes twice. Keyed per token and tool, kept for an hour, and bound to
// the arguments, so the same id with different arguments is refused rather than silently answered with a stale result.
class ReplayCache {
   public:
    // Runs `work` unless `operationId` already ran or is running. `work` reports its result and whether anything was
    // applied; a result where nothing was applied (refused up front, waiting for the user's answer) is not remembered,
    // so the retry runs for real.
    using Finish = std::function<void(const ToolResult& result, bool applied)>;
    void run(const QString& tokenId,
             const QString& tool,
             const QString& operationId,
             const QJsonObject& arguments,
             const ToolReply& reply,
             const std::function<void(const Finish&)>& work);

    void clear();

   private:
    struct Entry {
        QByteArray argumentsDigest;
        qint64 created = 0;
        bool running = true;
        ToolResult result;
    };

    void prune(qint64 now);

    QHash<QString, Entry> m_entries;
};

// The process-wide cache the batch and execute tools share
ReplayCache& replayCache();

// SHA-256 of the arguments minus operation_id, in Qt's canonical (sorted-key) compact JSON
QByteArray argumentsDigest(const QJsonObject& arguments);

}  // namespace Mcp
