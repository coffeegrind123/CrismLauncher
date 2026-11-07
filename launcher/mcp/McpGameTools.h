// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Mcp {

class ToolRegistry;

// launch and logs
void registerGameTools(ToolRegistry& registry);
// Account and Java ops
void registerGameOps(ToolRegistry& ops);

}  // namespace Mcp
