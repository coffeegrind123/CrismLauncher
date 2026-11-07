// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Mcp {

class ToolRegistry;

// Catalog ops that change things: instances, groups, components, content. Each declares its effect so the token's
// confirmation policy can ask the user first.
void registerWriteOps(ToolRegistry& ops);

}  // namespace Mcp
