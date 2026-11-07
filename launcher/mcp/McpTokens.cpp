// SPDX-License-Identifier: GPL-3.0-only
#include "McpTokens.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUuid>

#include <array>

#include "FileSystem.h"
#include "Json.h"

namespace Mcp {

namespace {
constexpr int SECRET_BYTES = 32;
constexpr int FORMAT_VERSION = 1;
// Saving on every request would rewrite the file constantly; last-used is accurate to this interval
constexpr qint64 LAST_USED_SAVE_INTERVAL_SECS = 60;

bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}
}  // namespace

const QList<Scope>& allScopes()
{
    static const QList<Scope> s_all{ Scope::Read,     Scope::Instances, Scope::Content, Scope::Launch,
                                     Scope::Settings, Scope::Accounts,  Scope::Files };
    return s_all;
}

QString scopeName(Scope scope)
{
    switch (scope) {
        case Scope::Read:
            return "read";
        case Scope::Instances:
            return "instances";
        case Scope::Content:
            return "content";
        case Scope::Launch:
            return "launch";
        case Scope::Settings:
            return "settings";
        case Scope::Accounts:
            return "accounts";
        case Scope::Files:
            return "files";
    }
    return {};
}

std::optional<Scope> scopeFromName(const QString& name)
{
    for (auto scope : allScopes()) {
        if (scopeName(scope) == name) {
            return scope;
        }
    }
    return std::nullopt;
}

QStringList scopeNames(Scopes scopes)
{
    QStringList out;
    for (auto scope : allScopes()) {
        if (scopes.testFlag(scope)) {
            out.append(scopeName(scope));
        }
    }
    return out;
}

Scopes scopesFromNames(const QStringList& names)
{
    Scopes out;
    for (const auto& name : names) {
        if (auto scope = scopeFromName(name)) {
            out |= *scope;
        }
    }
    return out;
}

QString policyName(ConfirmPolicy policy)
{
    switch (policy) {
        case ConfirmPolicy::AskDestructive:
            return "ask_destructive";
        case ConfirmPolicy::AskAllWrites:
            return "ask_all_writes";
        case ConfirmPolicy::NeverAsk:
            return "never_ask";
    }
    return {};
}

std::optional<ConfirmPolicy> policyFromName(const QString& name)
{
    for (auto policy : { ConfirmPolicy::AskDestructive, ConfirmPolicy::AskAllWrites, ConfirmPolicy::NeverAsk }) {
        if (policyName(policy) == name) {
            return policy;
        }
    }
    return std::nullopt;
}

TokenStore::TokenStore(QString path) : m_path(std::move(path)) {}

QByteArray TokenStore::hashSecret(const QString& secret)
{
    return QCryptographicHash::hash(secret.toUtf8(), QCryptographicHash::Sha256);
}

Result<> TokenStore::load()
{
    m_tokens.clear();
    if (!QFileInfo::exists(m_path)) {
        return {};
    }
    TRY_INTO(const auto root, Json::requireObject(m_path, "MCP tokens"))
    for (const auto& value : root["tokens"].toArray()) {
        const auto obj = value.toObject();
        TokenInfo token;
        token.id = obj["id"].toString();
        token.name = obj["name"].toString();
        token.hash = QByteArray::fromHex(obj["sha256"].toString().toLatin1());
        QStringList names;
        for (const auto& scope : obj["scopes"].toArray()) {
            names.append(scope.toString());
        }
        token.scopes = scopesFromNames(names);
        token.policy = policyFromName(obj["policy"].toString()).value_or(ConfirmPolicy::AskDestructive);
        token.created = QDateTime::fromString(obj["created"].toString(), Qt::ISODate);
        token.lastUsed = QDateTime::fromString(obj["lastUsed"].toString(), Qt::ISODate);
        if (token.id.isEmpty() || token.hash.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
            qWarning() << "MCP: skipping malformed token entry" << token.id;
            continue;
        }
        m_tokens.append(token);
    }
    return {};
}

Result<> TokenStore::save() const
{
    QJsonArray entries;
    for (const auto& token : m_tokens) {
        QJsonObject obj;
        obj["id"] = token.id;
        obj["name"] = token.name;
        obj["sha256"] = QString::fromLatin1(token.hash.toHex());
        obj["scopes"] = QJsonArray::fromStringList(scopeNames(token.scopes));
        obj["policy"] = policyName(token.policy);
        obj["created"] = token.created.toString(Qt::ISODate);
        if (token.lastUsed.isValid()) {
            obj["lastUsed"] = token.lastUsed.toString(Qt::ISODate);
        }
        entries.append(obj);
    }
    QJsonObject root;
    root["formatVersion"] = FORMAT_VERSION;
    root["tokens"] = entries;

    TRY(FS::write(m_path, QJsonDocument(root).toJson()))
    QFile::setPermissions(m_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return {};
}

std::pair<TokenInfo, QString> TokenStore::create(const QString& name, Scopes scopes, ConfirmPolicy policy)
{
    std::array<quint32, SECRET_BYTES / sizeof(quint32)> words{};
    QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
    const QByteArray random(reinterpret_cast<const char*>(words.data()), SECRET_BYTES);
    const auto secret =
        QString(SECRET_PREFIX) + QString::fromLatin1(random.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    TokenInfo token;
    token.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    token.name = name.trimmed().isEmpty() ? QStringLiteral("Agent") : name.trimmed();
    token.hash = hashSecret(secret);
    token.scopes = scopes;
    token.policy = policy;
    token.created = QDateTime::currentDateTimeUtc();
    m_tokens.append(token);
    return { token, secret };
}

bool TokenStore::revoke(const QString& id)
{
    return m_tokens.removeIf([&id](const TokenInfo& token) { return token.id == id; }) > 0;
}

bool TokenStore::update(const QString& id, const QString& name, Scopes scopes, ConfirmPolicy policy)
{
    for (auto& token : m_tokens) {
        if (token.id == id) {
            token.name = name.trimmed().isEmpty() ? token.name : name.trimmed();
            token.scopes = scopes;
            token.policy = policy;
            return true;
        }
    }
    return false;
}

std::optional<TokenInfo> TokenStore::find(const QString& id) const
{
    for (const auto& token : m_tokens) {
        if (token.id == id) {
            return token;
        }
    }
    return std::nullopt;
}

std::optional<TokenInfo> TokenStore::verify(const QString& secret)
{
    if (!secret.startsWith(SECRET_PREFIX)) {
        return std::nullopt;
    }
    const auto hash = hashSecret(secret);

    // Compare against every entry so the time taken doesn't reveal where (or whether) a match was found
    TokenInfo* match = nullptr;
    for (auto& token : m_tokens) {
        if (constantTimeEquals(token.hash, hash) && !match) {
            match = &token;
        }
    }
    if (!match) {
        return std::nullopt;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const bool stale = !match->lastUsed.isValid() || match->lastUsed.secsTo(now) >= LAST_USED_SAVE_INTERVAL_SECS;
    match->lastUsed = now;
    if (stale) {
        if (auto res = save(); !res) {
            qWarning() << "MCP: could not record token use:" << res.error();
        }
    }
    return *match;
}

}  // namespace Mcp
