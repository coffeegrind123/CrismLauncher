#include "YggdrasilServerResolveTask.h"

#include "Application.h"
#include "minecraft/auth/Yggdrasil.h"
#include "net/ByteArraySink.h"

namespace {
//! Keeps the API location header and the URL the response finally came from (after redirects)
class ApiLocationSink : public Net::ByteArraySink {
   public:
    Result<> finalize(QNetworkReply& reply) override
    {
        m_apiLocation = reply.rawHeader(Yggdrasil::API_LOCATION_HEADER);
        m_responseUrl = reply.url();
        return ByteArraySink::finalize(reply);
    }

    QByteArray m_apiLocation;
    QUrl m_responseUrl;
};
}  // namespace

YggdrasilServerResolveTask::YggdrasilServerResolveTask(QUrl serverUrl) : m_serverUrl(std::move(serverUrl)) {}

void YggdrasilServerResolveTask::executeTask()
{
    setStatus(tr("Contacting %1...").arg(m_serverUrl.host()));
    fetch(m_serverUrl, true);
}

bool YggdrasilServerResolveTask::abort()
{
    auto request = std::move(m_request);
    if (request) {
        request->abort();
    }
    emitAborted();
    return true;
}

void YggdrasilServerResolveTask::fetch(const QUrl& url, bool followApiLocation)
{
    auto request = Net::Request::makeCustomRequest({ .url = url });
    auto sink = std::make_unique<ApiLocationSink>();
    auto* sinkPtr = sink.get();
    request->setSink(std::move(sink));
    request->setNetwork(APPLICATION->network());

    auto* raw = request.get();
    connect(raw, &Task::finished, this, [this, raw, sinkPtr, url, followApiLocation] {
        if (m_request.get() != raw) {
            return;
        }

        if (raw->error() != QNetworkReply::NoError) {
            emitFailed(tr("Could not reach %1: %2").arg(url.toString(), raw->errorString()));
            return;
        }

        const auto responseUrl = sinkPtr->m_responseUrl.isValid() ? sinkPtr->m_responseUrl : url;
        const auto apiRoot = Yggdrasil::resolveApiLocation(responseUrl, sinkPtr->m_apiLocation);
        qDebug() << "Yggdrasil server" << url << "responded from" << responseUrl << "with API location" << sinkPtr->m_apiLocation;

        if (followApiLocation && apiRoot != Yggdrasil::resolveApiLocation(responseUrl, {})) {
            fetch(apiRoot, false);
            return;
        }

        const auto body = *sinkPtr->output();
        if (!Yggdrasil::isMetadata(body)) {
            qWarning() << "Not Yggdrasil metadata:" << body.left(256);
            emitFailed(tr("%1 is not an authlib-injector compatible authentication server.").arg(url.toString()));
            return;
        }

        m_apiRoot = apiRoot.toString();
        m_metadata = body;
        emitSucceeded();
    });

    m_request = request;
    m_request->start();
}
