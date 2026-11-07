// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Mcp {

class ToolRegistry;

// create_instance: vanilla (+ loader), a Modrinth/CurseForge modpack, or an imported archive, as a job
void registerCreateTool(ToolRegistry& registry);
// pack.update: move a Modrinth/CurseForge modpack instance to another pack version
void registerPackOps(ToolRegistry& ops);

}  // namespace Mcp
