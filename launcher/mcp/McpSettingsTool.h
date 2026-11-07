// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Mcp {

class ToolRegistry;

// The `settings` tool: list, read and change launcher and instance settings, with secrets redacted and
// code-running settings reserved for the user
void registerSettingsTool(ToolRegistry& registry);

}  // namespace Mcp
