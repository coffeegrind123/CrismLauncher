// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <optional>

#include "Result.h"

namespace Mcp::Schema {

// Validates `value` against the subset of JSON Schema the launcher's tools use: type (single or list), enum, const,
// properties, required, additionalProperties (bool or schema), items, minItems/maxItems, minLength/maxLength,
// minimum/maximum, pattern. Unknown keywords are ignored, as JSON Schema requires.
// The error names the failing location as a JSON pointer, e.g. "/instances/0/id: expected string".
Result<> validate(const QJsonValue& value, const QJsonObject& schema);

// Builders that keep tool schemas short and consistent
QJsonObject object(const QJsonObject& properties, const QStringList& required = {}, bool closed = true);
QJsonObject string(const QString& description, const QStringList& enumValues = {});
QJsonObject integer(const QString& description, std::optional<qint64> minimum = std::nullopt, std::optional<qint64> maximum = std::nullopt);
QJsonObject boolean(const QString& description);
QJsonObject array(const QJsonObject& items, const QString& description = {});
QJsonObject withDescription(QJsonObject schema, const QString& description);

}  // namespace Mcp::Schema
