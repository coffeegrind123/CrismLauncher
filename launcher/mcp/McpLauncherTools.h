// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonObject>

class MinecraftInstance;

namespace Mcp {

class ToolRegistry;

// Registers the launcher's fixed tools. Called once when the service is created.
void registerLauncherTools(ToolRegistry& registry);

// The compact description of an instance shared by several tools
QJsonObject instanceSummary(MinecraftInstance* instance);

}  // namespace Mcp
