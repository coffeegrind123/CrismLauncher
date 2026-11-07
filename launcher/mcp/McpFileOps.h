// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

#include "Result.h"

namespace Mcp {

// `relative` resolved inside `rootPath`: no absolute paths, no "..", and no symlink that leads outside
Result<QString> resolveInsideRoot(const QString& rootPath, const QString& relative);

class ToolRegistry;

// files.* (confined to an instance folder), screenshots.*, window.screenshot
void registerFileOps(ToolRegistry& ops);

}  // namespace Mcp
