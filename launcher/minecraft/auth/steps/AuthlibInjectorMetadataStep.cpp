#include "AuthlibInjectorMetadataStep.h"

#include "Application.h"
#include "minecraft/auth/Yggdrasil.h"

AuthlibInjectorMetadataStep::AuthlibInjectorMetadataStep(AccountData* data) : AuthStep(data) {}

QString AuthlibInjectorMetadataStep::describe()
{
    return tr("Fetching authentication server metadata.");
}

void AuthlibInjectorMetadataStep::perform()
{
    auto [request, response] = Net::Request::makeByteArray(QUrl(m_data->authlibInjectorUrl));
    m_request = request;

    m_task.reset(new NetJob("AuthlibInjectorMetadataStep", APPLICATION->network()));
    m_task->setAskRetry(false);
    m_task->addNetAction(m_request);

    connect(m_task.get(), &Task::finished, this, [this, response] { onRequestDone(response); });

    m_task->start();
}

void AuthlibInjectorMetadataStep::onRequestDone(QByteArray* response)
{
    if (m_request->error() != QNetworkReply::NoError || !Yggdrasil::isMetadata(*response)) {
        qWarning() << "Could not refresh metadata from" << m_data->authlibInjectorUrl << "-" << m_request->errorString()
                   << "- keeping the stored copy";
        emit finished(AccountTaskState::STATE_WORKING, tr("Kept the stored server metadata"));
        return;
    }

    m_data->authlibInjectorMetadata = QString::fromLatin1(response->toBase64());
    emit finished(AccountTaskState::STATE_WORKING, tr("Got server metadata"));
}
