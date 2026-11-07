// SPDX-License-Identifier: GPL-3.0-only
#include "McpCommon.h"

#include <QJsonArray>
#include <QTimer>

#include <memory>

#include "Application.h"
#include "InstanceList.h"
#include "mcp/McpService.h"
#include "mcp/McpTool.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/mod/DataPackFolderModel.h"
#include "minecraft/mod/Mod.h"
#include "minecraft/mod/ModFolderModel.h"
#include "minecraft/mod/Resource.h"
#include "minecraft/mod/ResourceFolderModel.h"
#include "minecraft/mod/ResourcePackFolderModel.h"
#include "minecraft/mod/ShaderPackFolderModel.h"
#include "minecraft/mod/TexturePackFolderModel.h"
#include "modplatform/ModIndex.h"

namespace Mcp {

namespace {
constexpr int MODEL_READY_TIMEOUT_MS = 30 * 1000;
}

Result<MinecraftInstance*> findInstance(const QString& id)
{
    auto* instances = APPLICATION->instances();
    if (auto* instance = instances->getInstanceById(id)) {
        return instance;
    }

    QStringList candidates;
    for (int i = 0; i < instances->count(); ++i) {
        candidates.append(instances->at(i)->id());
        candidates.append(instances->at(i)->name());
    }
    auto message = QString("No instance with id '%1'.").arg(id);
    const auto close = closestMatches(id, candidates);
    for (int i = 0; i < instances->count(); ++i) {
        // A name was passed where an id belongs: say which id it is
        if (instances->at(i)->name().compare(id, Qt::CaseInsensitive) == 0) {
            return std::unexpected(message + QString(" '%1' is the name of instance id '%2'.").arg(id, instances->at(i)->id()));
        }
    }
    if (!close.isEmpty()) {
        message += QString(" Closest: %1.").arg(close.join(", "));
    }
    return std::unexpected(message + " Call `instances` for the list of ids.");
}

const QStringList& contentKindNames()
{
    static const QStringList s_names{ "mods", "resourcepacks", "texturepacks", "shaderpacks", "datapacks" };
    return s_names;
}

std::optional<ContentKind> contentKindFromName(const QString& name)
{
    const auto index = contentKindNames().indexOf(name);
    if (index < 0) {
        return std::nullopt;
    }
    return static_cast<ContentKind>(index);
}

QString contentKindName(ContentKind kind)
{
    return contentKindNames().value(static_cast<int>(kind));
}

ResourceFolderModel* contentModel(MinecraftInstance* instance, ContentKind kind)
{
    switch (kind) {
        case ContentKind::Mods:
            return instance->loaderModList();
        case ContentKind::ResourcePacks:
            return instance->resourcePackList();
        case ContentKind::TexturePacks:
            return instance->texturePackList();
        case ContentKind::ShaderPacks:
            return instance->shaderPackList();
        case ContentKind::DataPacks:
            return instance->dataPackList();
    }
    return nullptr;
}

void whenModelReady(ResourceFolderModel* model, std::function<void()> done)
{
    struct State {
        bool fired = false;
        QMetaObject::Connection updated;
        QMetaObject::Connection parsed;
        std::function<void()> done;
    };
    auto state = std::make_shared<State>();
    state->done = std::move(done);

    auto fire = [state] {
        if (state->fired) {
            return;
        }
        state->fired = true;
        QObject::disconnect(state->updated);
        QObject::disconnect(state->parsed);
        state->done();
    };
    auto afterUpdate = [model, state, fire] {
        if (!model->hasPendingParseTasks()) {
            fire();
            return;
        }
        state->parsed = QObject::connect(model, &ResourceFolderModel::parseFinished, model, [model, fire] {
            if (!model->hasPendingParseTasks()) {
                fire();
            }
        });
    };

    state->updated = QObject::connect(model, &ResourceFolderModel::updateFinished, model, afterUpdate);
    QTimer::singleShot(MODEL_READY_TIMEOUT_MS, model, fire);
    // update() returns false when a scan is already running; that scan still ends with updateFinished
    model->update();
}

QJsonObject resourceJson(const Resource& resource)
{
    QJsonObject out{
        { "file", resource.fileinfo().fileName() },
        { "name", resource.name() },
        { "version", resource.version() },
        { "enabled", resource.enabled() },
        { "sizeBytes", resource.sizeInfo() },
    };
    if (const auto metadata = resource.metadata()) {
        out["platform"] = QString::fromLatin1(ModPlatform::ProviderCapabilities::name(metadata->provider));
        out["projectId"] = metadata->projectId.toString();
        out["fileId"] = metadata->fileId.toString();
        out["slug"] = metadata->slug;
        if (!metadata->versionNumber.isEmpty()) {
            out["versionNumber"] = metadata->versionNumber;
        }
        if (metadata->lockUpdate) {
            out["updatesLocked"] = true;
        }
    }
    if (const auto* mod = dynamic_cast<const Mod*>(&resource)) {
        if (!mod->modId().isEmpty()) {
            out["modId"] = mod->modId();
        }
        if (const auto authors = mod->authors(); !authors.isEmpty()) {
            out["authors"] = QJsonArray::fromStringList(authors);
        }
        if (const auto loaders = mod->loaders(); !loaders.isEmpty()) {
            out["loaders"] = loaders;
        }
        if (const auto side = mod->side(); !side.isEmpty()) {
            out["side"] = side;
        }
    }
    if (resource.hasIssues()) {
        out["issues"] = QJsonArray::fromStringList(resource.issues());
    }
    return out;
}

void runTask(const Task::Ptr& task, std::function<void(bool ok, const QString& error)> done)
{
    // Keeps the task alive until it reports, then lets go so the connections below don't keep it alive forever
    auto keepAlive = std::make_shared<Task::Ptr>(task);
    auto report = [keepAlive, done = std::move(done)](bool ok, const QString& error) {
        if (!*keepAlive) {
            return;
        }
        Task::Ptr held = *keepAlive;
        keepAlive->reset();
        done(ok, error);
        // This runs inside the task's own signal; release it only once that emission has returned
        QTimer::singleShot(0, [held] { (void)held; });
    };
    QObject::connect(task.get(), &Task::succeeded, task.get(), [report] { report(true, {}); });
    QObject::connect(task.get(), &Task::failed, task.get(), [report](const QString& reason) { report(false, reason); });
    QObject::connect(task.get(), &Task::aborted, task.get(), [report] { report(false, QObject::tr("Cancelled")); });
    if (!task->isRunning()) {
        task->start();
    }
}

JobRegistry* jobs()
{
    return APPLICATION->mcp() ? APPLICATION->mcp()->jobs() : nullptr;
}

}  // namespace Mcp
