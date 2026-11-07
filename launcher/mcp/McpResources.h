// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

#include "Result.h"

// MCP resources (documents an agent reads on demand) and prompts (ready-made task starters). Both lists are fixed for
// the life of the process, like the tool list.
namespace Mcp {

class ToolRegistry;

struct ResourceContent {
    QString uri;
    QString mimeType;
    QString text;
};
using ResourceReply = std::function<void(Result<ResourceContent>)>;

class ResourceCatalog {
   public:
    struct Entry {
        QString uri;  // a fixed URI, or a template with {variables}
        QString name;
        QString title;
        QString description;
        QString mimeType;
        bool isTemplate = false;
    };
    using Reader = std::function<void(const QString& uri, const QStringList& captures, const ResourceReply& reply)>;

    void add(Entry entry, Reader reader);
    QJsonArray listJson() const;
    QJsonArray templatesJson() const;
    // Finds the entry whose URI or template matches and lets it answer; unknown URIs fail
    void read(const QString& uri, const ResourceReply& reply) const;

   private:
    QList<std::pair<Entry, Reader>> m_entries;
};

class PromptCatalog {
   public:
    struct Argument {
        QString name;
        QString description;
        bool required = false;
    };
    struct Prompt {
        QString name;
        QString title;
        QString description;
        QList<Argument> arguments;
        // Returns the user message that starts the task
        std::function<QString(const QJsonObject& arguments)> render;
    };

    void add(Prompt prompt);
    QJsonArray listJson() const;
    Result<QJsonObject> get(const QString& name, const QJsonObject& arguments) const;

   private:
    QList<Prompt> m_prompts;
};

// The launcher's resources: API overview, the op catalog's schemas, per-instance summaries and logs, guides
void registerLauncherResources(ResourceCatalog& catalog, const ToolRegistry* tools, const ToolRegistry* ops);
void registerLauncherPrompts(PromptCatalog& catalog);

}  // namespace Mcp
