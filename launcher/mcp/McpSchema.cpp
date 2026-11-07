// SPDX-License-Identifier: GPL-3.0-only
#include "McpSchema.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace Mcp::Schema {

namespace {
QString typeOf(const QJsonValue& value)
{
    switch (value.type()) {
        case QJsonValue::Null:
            return "null";
        case QJsonValue::Bool:
            return "boolean";
        case QJsonValue::Double: {
            const double number = value.toDouble();
            return std::trunc(number) == number && std::isfinite(number) ? "integer" : "number";
        }
        case QJsonValue::String:
            return "string";
        case QJsonValue::Array:
            return "array";
        case QJsonValue::Object:
            return "object";
        case QJsonValue::Undefined:
            break;
    }
    return "undefined";
}

bool matchesType(const QJsonValue& value, const QString& type)
{
    const auto actual = typeOf(value);
    return actual == type || (type == "number" && actual == "integer");
}

QString escapePointer(const QString& key)
{
    QString out = key;
    out.replace('~', "~0");
    out.replace('/', "~1");
    return out;
}

QString where(const QString& pointer)
{
    return pointer.isEmpty() ? QStringLiteral("(arguments)") : pointer;
}

Result<> validateAt(const QJsonValue& value, const QJsonObject& schema, const QString& pointer)
{
    if (const auto type = schema.value("type"); !type.isUndefined()) {
        QStringList types;
        if (type.isArray()) {
            for (const auto& t : type.toArray()) {
                types.append(t.toString());
            }
        } else {
            types.append(type.toString());
        }
        const bool ok = std::any_of(types.begin(), types.end(), [&value](const QString& t) { return matchesType(value, t); });
        if (!ok) {
            return std::unexpected(QString("%1: expected %2, got %3").arg(where(pointer), types.join(" or "), typeOf(value)));
        }
    }

    if (const auto constant = schema.value("const"); !constant.isUndefined() && value != constant) {
        return std::unexpected(QString("%1: must be %2").arg(where(pointer), constant.toVariant().toString()));
    }

    if (const auto enumValues = schema.value("enum"); enumValues.isArray()) {
        const auto options = enumValues.toArray();
        if (!options.contains(value)) {
            QStringList names;
            for (const auto& option : options) {
                names.append(option.toVariant().toString());
            }
            return std::unexpected(
                QString("%1: '%2' is not one of: %3").arg(where(pointer), value.toVariant().toString(), names.join(", ")));
        }
    }

    if (value.isString()) {
        const auto text = value.toString();
        if (schema.contains("minLength") && text.size() < schema["minLength"].toInteger()) {
            return std::unexpected(QString("%1: shorter than %2 characters").arg(where(pointer)).arg(schema["minLength"].toInteger()));
        }
        if (schema.contains("maxLength") && text.size() > schema["maxLength"].toInteger()) {
            return std::unexpected(QString("%1: longer than %2 characters").arg(where(pointer)).arg(schema["maxLength"].toInteger()));
        }
        if (schema.contains("pattern")) {
            const QRegularExpression pattern(schema["pattern"].toString());
            if (pattern.isValid() && !pattern.match(text).hasMatch()) {
                return std::unexpected(QString("%1: does not match %2").arg(where(pointer), pattern.pattern()));
            }
        }
    }

    if (value.isDouble()) {
        const double number = value.toDouble();
        if (schema.contains("minimum") && number < schema["minimum"].toDouble()) {
            return std::unexpected(QString("%1: below the minimum %2").arg(where(pointer)).arg(schema["minimum"].toDouble()));
        }
        if (schema.contains("maximum") && number > schema["maximum"].toDouble()) {
            return std::unexpected(QString("%1: above the maximum %2").arg(where(pointer)).arg(schema["maximum"].toDouble()));
        }
    }

    if (value.isArray()) {
        const auto items = value.toArray();
        if (schema.contains("minItems") && items.size() < schema["minItems"].toInteger()) {
            return std::unexpected(QString("%1: needs at least %2 items").arg(where(pointer)).arg(schema["minItems"].toInteger()));
        }
        if (schema.contains("maxItems") && items.size() > schema["maxItems"].toInteger()) {
            return std::unexpected(QString("%1: allows at most %2 items").arg(where(pointer)).arg(schema["maxItems"].toInteger()));
        }
        if (const auto itemSchema = schema.value("items"); itemSchema.isObject()) {
            for (qsizetype i = 0; i < items.size(); ++i) {
                TRY(validateAt(items[i], itemSchema.toObject(), pointer + "/" + QString::number(i)))
            }
        }
    }

    if (value.isObject()) {
        const auto object = value.toObject();
        const auto properties = schema.value("properties").toObject();
        for (const auto& required : schema.value("required").toArray()) {
            if (!object.contains(required.toString())) {
                return std::unexpected(QString("%1: missing required property '%2'").arg(where(pointer), required.toString()));
            }
        }
        const auto additional = schema.value("additionalProperties");
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto childPointer = pointer + "/" + escapePointer(it.key());
            if (properties.contains(it.key())) {
                TRY(validateAt(it.value(), properties[it.key()].toObject(), childPointer))
                continue;
            }
            if (additional.isBool() && !additional.toBool()) {
                QStringList known = properties.keys();
                return std::unexpected(QString("%1: unknown property '%2' (known: %3)").arg(where(pointer), it.key(), known.join(", ")));
            }
            if (additional.isObject()) {
                TRY(validateAt(it.value(), additional.toObject(), childPointer))
            }
        }
    }

    return {};
}
}  // namespace

Result<> validate(const QJsonValue& value, const QJsonObject& schema)
{
    return validateAt(value, schema, QString());
}

QJsonObject object(const QJsonObject& properties, const QStringList& required, bool closed)
{
    QJsonObject out{ { "type", "object" }, { "properties", properties } };
    if (!required.isEmpty()) {
        out["required"] = QJsonArray::fromStringList(required);
    }
    if (closed) {
        out["additionalProperties"] = false;
    }
    return out;
}

QJsonObject string(const QString& description, const QStringList& enumValues)
{
    QJsonObject out{ { "type", "string" } };
    if (!description.isEmpty()) {
        out["description"] = description;
    }
    if (!enumValues.isEmpty()) {
        out["enum"] = QJsonArray::fromStringList(enumValues);
    }
    return out;
}

QJsonObject integer(const QString& description, std::optional<qint64> minimum, std::optional<qint64> maximum)
{
    QJsonObject out{ { "type", "integer" } };
    if (!description.isEmpty()) {
        out["description"] = description;
    }
    if (minimum) {
        out["minimum"] = *minimum;
    }
    if (maximum) {
        out["maximum"] = *maximum;
    }
    return out;
}

QJsonObject boolean(const QString& description)
{
    QJsonObject out{ { "type", "boolean" } };
    if (!description.isEmpty()) {
        out["description"] = description;
    }
    return out;
}

QJsonObject array(const QJsonObject& items, const QString& description)
{
    QJsonObject out{ { "type", "array" }, { "items", items } };
    if (!description.isEmpty()) {
        out["description"] = description;
    }
    return out;
}

QJsonObject withDescription(QJsonObject schema, const QString& description)
{
    schema["description"] = description;
    return schema;
}

}  // namespace Mcp::Schema
