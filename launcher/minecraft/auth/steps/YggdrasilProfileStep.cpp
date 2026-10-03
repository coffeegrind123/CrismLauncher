#include "YggdrasilProfileStep.h"

#include "Application.h"
#include "minecraft/auth/Parsers.h"
#include "net/NetUtils.h"

YggdrasilProfileStep::YggdrasilProfileStep(AccountData* data) : AuthStep(data) {}

QString YggdrasilProfileStep::describe()
{
    return tr("Fetching the Minecraft profile.");
}

void YggdrasilProfileStep::perform()
{
    if (m_data->minecraftProfile.id.isEmpty()) {
        emit finished(AccountTaskState::STATE_WORKING, tr("Account has no Minecraft profile."));
        return;
    }

    // unsigned=false asks for signed properties, which some servers require to include textures
    QUrl url(m_data->sessionServerUrl() + "/session/minecraft/profile/" + m_data->minecraftProfile.id + "?unsigned=false");
    auto [request, response] = Net::Request::makeByteArray(url);
    m_request = request;
    m_request->enableAutoRetry(true);

    m_task.reset(new NetJob("YggdrasilProfileStep", APPLICATION->network()));
    m_task->setAskRetry(false);
    m_task->addNetAction(m_request);

    connect(m_task.get(), &Task::finished, this, [this, response] { onRequestDone(response); });

    m_task->start();
}

void YggdrasilProfileStep::onRequestDone(QByteArray* response)
{
    if (m_request->error() != QNetworkReply::NoError) {
        qWarning() << "Error getting Yggdrasil profile:";
        qWarning() << " HTTP Status       :" << m_request->replyStatusCode();
        qWarning() << " Internal error no.:" << m_request->error();
        qWarning() << " Error string      :" << m_request->errorString();
        qWarning() << " Response:";
        qWarning() << QString::fromUtf8(*response);

        if (Net::isApplicationError(m_request->error()) && !Net::isServerError(m_request->error())) {
            emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("Minecraft profile acquisition failed: %1").arg(m_request->errorString()));
        } else {
            m_data->networkError = m_request->error();
            emit finished(AccountTaskState::STATE_OFFLINE, tr("Minecraft profile acquisition failed: %1").arg(m_request->errorString()));
        }
        return;
    }

    // A 204 means the server doesn't know the profile any more
    if (response->isEmpty()) {
        emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("The server no longer has the profile %1.").arg(m_data->profileName()));
        return;
    }

    if (!Parsers::parseMinecraftProfileMojang(*response, m_data->minecraftProfile)) {
        emit finished(AccountTaskState::STATE_FAILED_SOFT, tr("Minecraft profile response could not be parsed"));
        return;
    }

    emit finished(AccountTaskState::STATE_WORKING, tr("Got Minecraft profile"));
}
