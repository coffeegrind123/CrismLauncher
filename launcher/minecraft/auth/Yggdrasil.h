#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>
#include <variant>

// Protocol helpers for Yggdrasil-compatible servers used through authlib-injector, kept free of
// networking so they can be unit tested. Spec:
// https://github.com/yushijinhun/authlib-injector/wiki/Yggdrasil-%E6%9C%8D%E5%8A%A1%E7%AB%AF%E6%8A%80%E6%9C%AF%E8%A7%84%E8%8C%83
namespace Yggdrasil {

inline const QString ELYBY_API_ROOT = QStringLiteral("https://authserver.ely.by/api/authlib-injector");

//! Response header pointing from any page of a server to its API root ("ALI")
inline const QByteArray API_LOCATION_HEADER = QByteArrayLiteral("X-Authlib-Injector-API-Location");

//! Scheme of the links skin sites offer for drag and drop onto launchers
inline const QString DRAG_LINK_PREFIX = QStringLiteral("authlib-injector:yggdrasil-server:");

//! Where authlib-injector releases are described, official first; both serve the same builds
inline const QStringList AGENT_RELEASE_SOURCES{
    QStringLiteral("https://authlib-injector.yushi.moe/artifact/latest.json"),
    QStringLiteral("https://bmclapi2.bangbang93.com/mirrors/authlib-injector/artifact/latest.json"),
};

struct AgentArtifact {
    QString version;
    QUrl downloadUrl;
    QByteArray sha256;  //!< raw digest, not hex
};

struct Profile {
    QString id;
    QString name;
};

struct AuthResponse {
    QString accessToken;
    QString clientToken;
    std::optional<Profile> selectedProfile;
    QList<Profile> availableProfiles;
};

struct Error {
    QString error;
    QString message;
    QString cause;
};

/** Turns what a user typed or dropped into a URL: adds https:// when no scheme is given, unwraps
 *  authlib-injector drag links and drops trailing slashes. Returns an invalid URL for garbage. */
QUrl normalizeServerInput(const QString& input);

/** Applies an X-Authlib-Injector-API-Location header value, which may be relative, to the URL the
 *  response came from (after redirects). Without a header the URL itself is the API root. */
QUrl resolveApiLocation(const QUrl& responseUrl, const QByteArray& apiLocationHeader);

/** True if `body` is an API root's metadata document (a JSON object with a "meta" object). */
bool isMetadata(const QByteArray& body);

/** meta.serverName from base64 encoded metadata, empty if unavailable. */
QString serverNameFromMetadata(const QString& base64Metadata);

/** meta.links.<key> (e.g. "homepage", "register") from base64 encoded metadata. */
QString linkFromMetadata(const QString& base64Metadata, const QString& key);

/** Parses /authserver/authenticate and /authserver/refresh responses. */
std::variant<AuthResponse, QString> parseAuthResponse(const QByteArray& body);

/** Parses an {"error", "errorMessage", "cause"} error body. */
std::optional<Error> parseError(const QByteArray& body);

/** Parses a latest.json release description. Rejects builds served from anywhere but the release
 *  sources' own hosts over https, or without a well-formed sha256. */
std::optional<AgentArtifact> parseAgentArtifact(const QByteArray& body);

/** JVM arguments loading authlib-injector against `apiRoot`. The metadata is prefetched so the
 *  game doesn't have to fetch it at startup; oversized metadata is left for the agent to fetch,
 *  keeping the command line well within platform limits. */
QStringList agentArguments(const QString& jarPath, const QString& apiRoot, const QString& base64Metadata);

/** Ely.by refuses password logins for accounts with two-factor auth until the TOTP code is
 *  appended to the password as "password:code". */
bool isTwoFactorRequired(const Error& error);

}  // namespace Yggdrasil
