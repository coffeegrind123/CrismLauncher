#include "YggdrasilStep.h"

#include <QJsonDocument>

#include "Application.h"
#include "net/RawHeaderProxy.h"

namespace {
// The authserver answers 204 No Content to /validate when the access token is still usable
constexpr int HTTP_NO_CONTENT = 204;
constexpr int HTTP_OK = 200;
constexpr int HTTP_SERVER_ERROR = 500;
}  // namespace

YggdrasilStep::YggdrasilStep(AccountData* data, std::optional<QString> password) : AuthStep(data), m_password(std::move(password)) {}

QString YggdrasilStep::describe()
{
    return m_password ? tr("Logging in to %1.").arg(m_data->serverName()) : tr("Refreshing the %1 session.").arg(m_data->serverName());
}

void YggdrasilStep::perform()
{
    if (m_data->clientToken().isEmpty()) {
        m_data->generateClientToken();
    }

    if (m_password) {
        authenticate();
        return;
    }

    if (m_data->accessToken().isEmpty()) {
        emit finished(AccountTaskState::STATE_FAILED_HARD, tr("There is no session to refresh. Please log in again."));
        return;
    }
    validate();
}

void YggdrasilStep::abort()
{
    // Detach first so the aborted request's finished signal doesn't report a failure
    auto request = std::move(m_request);
    if (request) {
        request->abort();
    }
}

void YggdrasilStep::post(const QString& endpoint, const QJsonObject& body, ResponseHandler handler)
{
    const QUrl url(m_data->authServerUrl() + endpoint);
    auto [request, response] = Net::Request::makeByteArray(url, QJsonDocument(body).toJson(QJsonDocument::Compact));
    request->addHeaderProxy(std::make_unique<Net::RawHeaderProxy>(
        QList<Net::HeaderPair>{ { "Content-Type", "application/json" }, { "Accept", "application/json" } }));

    // Run the request on its own rather than in a NetJob: NetJob retries failures, which would
    // repeat a rejected password against the server
    request->setNetwork(APPLICATION->network());

    auto* raw = request.get();
    connect(raw, &Task::finished, this, [this, raw, response, handler] {
        if (m_request.get() != raw) {
            return;
        }
        const auto body = *response;
        handler(raw->replyStatusCode(), body);
    });

    m_request = request;
    m_request->start();
}

void YggdrasilStep::authenticate()
{
    QJsonObject agent{ { "name", "Minecraft" }, { "version", 1 } };
    QJsonObject body{ { "agent", agent },
                      { "username", m_data->userName() },
                      { "password", *m_password },
                      { "clientToken", m_data->clientToken() },
                      { "requestUser", false } };

    post("/authenticate", body, [this](int status, const QByteArray& response) { onSession(status, response, true); });
}

void YggdrasilStep::validate()
{
    QJsonObject body{ { "accessToken", m_data->accessToken() }, { "clientToken", m_data->clientToken() } };

    post("/validate", body, [this](int status, const QByteArray& response) {
        if (status == HTTP_NO_CONTENT) {
            m_data->yggdrasilToken.validity = Validity::Certain;
            emit finished(AccountTaskState::STATE_WORKING, tr("Session is still valid"));
            return;
        }
        if (status <= 0) {
            finishWithError(status, response, tr("Validating the session"));
            return;
        }
        refresh();
    });
}

void YggdrasilStep::refresh(const std::optional<Yggdrasil::Profile>& selectProfile)
{
    QJsonObject body{ { "accessToken", m_data->accessToken() }, { "clientToken", m_data->clientToken() }, { "requestUser", false } };
    if (selectProfile) {
        body["selectedProfile"] = QJsonObject{ { "id", selectProfile->id }, { "name", selectProfile->name } };
    }

    const bool isLogin = m_password.has_value();
    post("/refresh", body, [this, isLogin](int status, const QByteArray& response) { onSession(status, response, isLogin); });
}

void YggdrasilStep::onSession(int status, const QByteArray& body, bool isLogin)
{
    if (status != HTTP_OK) {
        finishWithError(status, body, isLogin ? tr("Logging in") : tr("Refreshing the session"));
        return;
    }

    const auto parsed = Yggdrasil::parseAuthResponse(body);
    if (const auto* error = std::get_if<QString>(&parsed)) {
        qWarning() << "Unexpected Yggdrasil response:" << *error << QString::fromUtf8(body);
        emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("The server sent an invalid response: %1").arg(*error));
        return;
    }
    const auto& session = std::get<Yggdrasil::AuthResponse>(parsed);

    // The spec requires the server to echo our client token; a token bound to another one would
    // fail every later refresh, so adopt what the server says it is bound to
    if (!session.clientToken.isEmpty() && session.clientToken != m_data->clientToken()) {
        qWarning() << "Yggdrasil server replaced the client token";
        m_data->yggdrasilToken.extra["clientToken"] = session.clientToken;
    }

    m_data->yggdrasilToken.token = session.accessToken;
    m_data->yggdrasilToken.issueInstant = QDateTime::currentDateTimeUtc();
    m_data->yggdrasilToken.notAfter = QDateTime();
    m_data->yggdrasilToken.validity = Validity::Certain;

    if (session.selectedProfile) {
        if (m_data->minecraftProfile.id != session.selectedProfile->id) {
            m_data->minecraftProfile = MinecraftProfile();
        }
        m_data->minecraftProfile.id = session.selectedProfile->id;
        m_data->minecraftProfile.name = session.selectedProfile->name;
        m_data->minecraftProfile.validity = Validity::Assumed;
        emit finished(AccountTaskState::STATE_WORKING, tr("Logged in as %1").arg(session.selectedProfile->name));
        return;
    }

    if (session.availableProfiles.isEmpty()) {
        // Valid on servers where players create characters on the website after registering
        m_data->minecraftProfile = MinecraftProfile();
        emit finished(AccountTaskState::STATE_WORKING, tr("Account has no Minecraft profile"));
        return;
    }

    if (!isLogin) {
        emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("No profile is selected for this session. Please log in again."));
        return;
    }

    // Unbound token: bind it to a profile through refresh, as the spec asks launchers to do
    int chosen = 0;
    if (session.availableProfiles.size() > 1) {
        QStringList names;
        for (const auto& profile : session.availableProfiles) {
            names.append(profile.name);
        }
        chosen = -1;
        emit selectProfile(names, &chosen);
        if (chosen < 0 || chosen >= session.availableProfiles.size()) {
            emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("No profile was selected."));
            return;
        }
    }
    refresh(session.availableProfiles.at(chosen));
}

void YggdrasilStep::finishWithError(int status, const QByteArray& body, const QString& action)
{
    const auto error = Yggdrasil::parseError(body);
    const auto detail = error && !error->message.isEmpty() ? error->message : m_request->errorString();

    qWarning() << action << "failed:";
    qWarning() << " HTTP Status       :" << status;
    qWarning() << " Internal error no.:" << m_request->error();
    qWarning() << " Error string      :" << m_request->errorString();
    qWarning() << " Response          :" << QString::fromUtf8(body);

    if (status <= 0 || status >= HTTP_SERVER_ERROR) {
        m_data->networkError = m_request->error();
        emit finished(AccountTaskState::STATE_OFFLINE, tr("%1 failed: %2").arg(action, detail));
        return;
    }

    if (error && Yggdrasil::isTwoFactorRequired(*error)) {
        emit twoFactorRequired();
        emit finished(AccountTaskState::STATE_FAILED_SOFT, detail);
        return;
    }

    if (error && error->error == "ForbiddenOperationException") {
        emit finished(AccountTaskState::STATE_FAILED_HARD, tr("%1 failed: %2").arg(action, detail));
        return;
    }

    emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("%1 failed: %2").arg(action, detail));
}
