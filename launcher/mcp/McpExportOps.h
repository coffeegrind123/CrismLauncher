// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Mcp {

class ToolRegistry;

// instance.export (zip, mrpack, curseforge) and instance.copy, as jobs
void registerExportOps(ToolRegistry& ops);

}  // namespace Mcp
