// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Mcp {

class ToolRegistry;

// worlds.* (rename, copy, backup, delete) and servers.* (the multiplayer list)
void registerWorldOps(ToolRegistry& ops);

}  // namespace Mcp
