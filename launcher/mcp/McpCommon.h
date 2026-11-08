// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

#include "Result.h"
#include "tasks/Task.h"

class MinecraftInstance;
class Resource;
class ResourceFolderModel;

// Helpers shared by the launcher's tools and ops
namespace Mcp {

class JobRegistry;

// Finds an instance by id; the error lists the closest ids and names so the model can correct itself
Result<MinecraftInstance*> findInstance(const QString& id);

// The content folders an instance has, by the names tools use
enum class ContentKind { Mods, ResourcePacks, TexturePacks, ShaderPacks, DataPacks };
const QStringList& contentKindNames();
std::optional<ContentKind> contentKindFromName(const QString& name);
QString contentKindName(ContentKind kind);
// The instance's folder model for `kind`; an error when the instance has none (data packs need "global data packs" on)
Result<ResourceFolderModel*> contentModel(MinecraftInstance* instance, ContentKind kind);

// Runs `done` once `model` has rescanned its folder and finished parsing file metadata (names, versions), or after a
// timeout so a stuck parse never hangs a call
void whenModelReady(ResourceFolderModel* model, std::function<void()> done);

QJsonObject resourceJson(const Resource& resource);

// Starts `task` (if needed) and reports once it finished
void runTask(const Task::Ptr& task, std::function<void(bool ok, const QString& error)> done);

// The service's job registry
JobRegistry* jobs();

}  // namespace Mcp
