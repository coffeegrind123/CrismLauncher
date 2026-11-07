// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFlags>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

#include "Result.h"

namespace Mcp {

// What a token may do. Each tool and op names the one scope it needs.
enum class Scope : quint32 {
    Read = 1 << 0,       // look at anything that is not a secret
    Instances = 1 << 1,  // create, edit, copy, delete instances, groups and components
    Content = 1 << 2,    // mods, resource packs, shaders, data packs, worlds
    Launch = 1 << 3,     // start and stop the game
    Settings = 1 << 4,   // change launcher and instance settings
    Accounts = 1 << 5,   // manage accounts (never reveals their tokens)
    Files = 1 << 6,      // raw file access inside an instance folder
};
Q_DECLARE_FLAGS(Scopes, Scope)
Q_DECLARE_OPERATORS_FOR_FLAGS(Scopes)

const QList<Scope>& allScopes();
QString scopeName(Scope scope);
std::optional<Scope> scopeFromName(const QString& name);
QStringList scopeNames(Scopes scopes);
Scopes scopesFromNames(const QStringList& names);

// When the launcher asks the human before running an agent's change
enum class ConfirmPolicy { AskDestructive, AskAllWrites, NeverAsk };

QString policyName(ConfirmPolicy policy);
std::optional<ConfirmPolicy> policyFromName(const QString& name);

struct TokenInfo {
    QString id;
    QString name;
    QByteArray hash;  // SHA-256 of the secret; the secret itself is never stored
    Scopes scopes;
    ConfirmPolicy policy = ConfirmPolicy::AskDestructive;
    QDateTime created;
    QDateTime lastUsed;
};

// Bearer tokens for the MCP endpoint, kept in their own file (never in the main config) with owner-only permissions
class TokenStore {
   public:
    static constexpr auto SECRET_PREFIX = "crism_mcp_";

    explicit TokenStore(QString path);

    Result<> load();
    Result<> save() const;

    // Returns the stored record and the secret, which is shown to the user once and not kept
    std::pair<TokenInfo, QString> create(const QString& name, Scopes scopes, ConfirmPolicy policy);
    bool revoke(const QString& id);
    bool update(const QString& id, const QString& name, Scopes scopes, ConfirmPolicy policy);

    // Constant-time match of `secret` against every stored hash; records the use
    std::optional<TokenInfo> verify(const QString& secret);

    const QList<TokenInfo>& tokens() const { return m_tokens; }
    std::optional<TokenInfo> find(const QString& id) const;

    static QByteArray hashSecret(const QString& secret);

   private:
    QString m_path;
    QList<TokenInfo> m_tokens;
};

}  // namespace Mcp
