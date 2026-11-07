// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QJsonValue>

#include "Result.h"

namespace Mcp {

class ToolRegistry;

constexpr int MAX_BATCH_OPS = 50;

// Registers search_ops, call and batch on `tools`. The ops themselves live in `ops`, a registry of Tool entries with
// dotted names ("components.list"); keeping them out of tools/list keeps the listed surface small and fixed.
void registerOpTools(ToolRegistry& tools, const ToolRegistry* ops);

// batch references: a string argument "$2.instances.0.id" becomes that path of op #2's result. "$2" alone is the
// whole result; "$$..." is a literal string starting with "$". Only earlier, successful ops can be referenced.
Result<QJsonValue> resolveReferences(const QJsonValue& value, const QJsonArray& results, int current);

}  // namespace Mcp
