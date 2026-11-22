// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

#include <cstddef>

namespace Mcp {

class ToolRegistry;

// Limits of one execute call. The script runs on the GUI thread, so the slice limit (JavaScript running without awaiting
// anything) is what keeps the launcher responsive; the wall-clock timeout covers the whole run, waits for ops included.
constexpr int MAX_SCRIPT_CODE_BYTES = 256 * 1024;
constexpr int DEFAULT_SCRIPT_TIMEOUT_SECONDS = 60;
constexpr int MAX_SCRIPT_TIMEOUT_SECONDS = 600;
constexpr int MAX_SCRIPT_SLICE_MS = 5000;
constexpr std::size_t MAX_SCRIPT_MEMORY_BYTES = 64 * 1024 * 1024;
constexpr std::size_t MAX_SCRIPT_STACK_BYTES = 256 * 1024;
constexpr int MAX_SCRIPT_RESULT_BYTES = 1024 * 1024;
constexpr int MAX_SCRIPT_LOG_LINES = 200;
constexpr int MAX_SCRIPT_LOG_BYTES = 64 * 1024;
constexpr int MAX_SCRIPT_OPS = 10000;
constexpr int MAX_SCRIPT_OPS_LISTED = 200;

// Registers `execute` on `tools`: JavaScript (QuickJS, ES2023, no file/network/process access of its own) that calls every
// catalog op in `ops` as `await launcher.<area>.<op>(args)` and every direct tool in `tools` except the meta tools
// (search_ops, call, batch, execute) as `await launcher.<tool>(args)`. Each call goes through invokeTool like any other:
// scopes, argument validation and the token's confirmation policy all apply.
void registerScriptTool(ToolRegistry& tools, const ToolRegistry* ops);

// TypeScript declarations of the API a script sees (launcher.<area>.<op>, launcher.<tool>, LauncherError, console), generated
// from the input and output schemas; served as launcher://api/launcher.d.ts
QString scriptTypeDeclarations(const ToolRegistry& tools, const ToolRegistry& ops);

}  // namespace Mcp
