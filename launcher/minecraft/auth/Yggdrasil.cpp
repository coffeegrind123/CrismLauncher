#include "Yggdrasil.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace Yggdrasil {

namespace {
QJsonObject parseObject(const QByteArray& body)
{
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

QJsonObject metaFromBase64(const QString& base64Metadata)
{
    if (base64Metadata.isEmpty()) {
        return {};
    }
    return parseObject(QByteArray::fromBase64(base64Metadata.toLatin1())).value("meta").toObject();
}

std::optional<Profile> parseProfile(const QJsonValue& value)
{
    const auto object = value.toObject();
    const auto id = object.value("id").toString();
    const auto name = object.value("name").toString();
    if (id.isEmpty() || name.isEmpty()) {
        return std::nullopt;
    }
    return Profile{ id, name };
}
}  // namespace

QUrl normalizeServerInput(const QString& input)
{
    auto text = input.trimmed();
    if (text.startsWith(DRAG_LINK_PREFIX)) {
        text = QUrl::fromPercentEncoding(text.mid(DRAG_LINK_PREFIX.size()).toUtf8()).trimmed();
    }
    if (text.isEmpty()) {
        return {};
    }

    if (!text.contains("://")) {
        text.prepend("https://");
    }

    QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
        return {};
    }

    auto path = url.path();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    url.setPath(path);
    return url;
}

QUrl resolveApiLocation(const QUrl& responseUrl, const QByteArray& apiLocationHeader)
{
    const auto location = QString::fromUtf8(apiLocationHeader).trimmed();
    auto resolved = location.isEmpty() ? responseUrl : responseUrl.resolved(QUrl(location));

    auto path = resolved.path();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    resolved.setPath(path);
    resolved.setQuery(QString());
    resolved.setFragment(QString());
    return resolved;
}

bool isMetadata(const QByteArray& body)
{
    return parseObject(body).value("meta").isObject();
}

QString serverNameFromMetadata(const QString& base64Metadata)
{
    return metaFromBase64(base64Metadata).value("serverName").toString();
}

QString linkFromMetadata(const QString& base64Metadata, const QString& key)
{
    return metaFromBase64(base64Metadata).value("links").toObject().value(key).toString();
}

std::variant<AuthResponse, QString> parseAuthResponse(const QByteArray& body)
{
    const auto object = parseObject(body);
    if (object.isEmpty()) {
        return QStringLiteral("Response is not a JSON object");
    }

    AuthResponse response;
    response.accessToken = object.value("accessToken").toString();
    if (response.accessToken.isEmpty()) {
        return QStringLiteral("Response has no access token");
    }
    response.clientToken = object.value("clientToken").toString();

    if (object.contains("selectedProfile")) {
        response.selectedProfile = parseProfile(object.value("selectedProfile"));
    }
    for (const auto& value : object.value("availableProfiles").toArray()) {
        if (auto profile = parseProfile(value)) {
            response.availableProfiles.append(*profile);
        }
    }
    return response;
}

std::optional<Error> parseError(const QByteArray& body)
{
    const auto object = parseObject(body);
    const auto error = object.value("error").toString();
    if (error.isEmpty()) {
        return std::nullopt;
    }
    return Error{ error, object.value("errorMessage").toString(), object.value("cause").toString() };
}

std::optional<AgentArtifact> parseAgentArtifact(const QByteArray& body)
{
    const auto object = parseObject(body);
    const auto version = object.value("version").toString();
    const QUrl url(object.value("download_url").toString());
    const auto hex = object.value("checksums").toObject().value("sha256").toString();

    static const QRegularExpression s_sha256(QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (version.isEmpty() || !s_sha256.match(hex).hasMatch()) {
        return std::nullopt;
    }

    const bool trustedHost = std::ranges::any_of(AGENT_RELEASE_SOURCES, [&url](const QString& source) { return QUrl(source).host() == url.host(); });
    if (url.scheme() != "https" || !trustedHost) {
        qWarning() << "Refusing authlib-injector download from" << url;
        return std::nullopt;
    }

    return AgentArtifact{ version, url, QByteArray::fromHex(hex.toLatin1()) };
}

QStringList agentArguments(const QString& jarPath, const QString& apiRoot, const QString& base64Metadata)
{
    constexpr qsizetype MAX_PREFETCHED_METADATA = 16 * 1024;

    QStringList args{ "-javaagent:" + jarPath + "=" + apiRoot };
    if (!base64Metadata.isEmpty() && base64Metadata.size() <= MAX_PREFETCHED_METADATA) {
        args << "-Dauthlibinjector.yggdrasil.prefetched=" + base64Metadata;
    }
    return args;
}

bool isTwoFactorRequired(const Error& error)
{
    // Ely.by's AuthenticationForm maps TotpRequiredException to exactly this message
    return error.message.contains("two factor", Qt::CaseInsensitive);
}

}  // namespace Yggdrasil
