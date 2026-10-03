#pragma once

#include <QByteArray>
#include <QUrl>

#include "net/Request.h"
#include "tasks/Task.h"

/**
 * Finds a Yggdrasil server's API root from any URL the user has for it (the skin site's homepage,
 * the API root itself, or a drag link) and fetches its metadata:
 *
 *   GET <input>  --X-Authlib-Injector-API-Location-->  GET <API root>  -> metadata JSON
 */
class YggdrasilServerResolveTask : public Task {
    Q_OBJECT

   public:
    explicit YggdrasilServerResolveTask(QUrl serverUrl);
    ~YggdrasilServerResolveTask() override = default;

    QString apiRoot() const { return m_apiRoot; }
    QByteArray metadata() const { return m_metadata; }

    bool canAbort() const override { return true; }

   public slots:
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    void fetch(const QUrl& url, bool followApiLocation);

   private:
    QUrl m_serverUrl;
    QString m_apiRoot;
    QByteArray m_metadata;
    Net::Request::Ptr m_request;
};
