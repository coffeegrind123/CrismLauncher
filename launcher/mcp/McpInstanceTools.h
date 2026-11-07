// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Mcp {

class ToolRegistry;

// instance_get and jobs
void registerInstanceTools(ToolRegistry& registry);
// Read-only catalog ops: groups, components, content, worlds, accounts
void registerInstanceOps(ToolRegistry& ops);

}  // namespace Mcp
