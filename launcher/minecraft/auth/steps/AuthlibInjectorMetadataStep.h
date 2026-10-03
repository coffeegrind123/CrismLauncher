#pragma once

#include <QObject>

#include "minecraft/auth/AuthStep.h"
#include "net/NetJob.h"
#include "net/Request.h"

/** Refreshes the API root's metadata document that is prefetched into the game. Never fails the
 *  flow: a stale copy still works, and authlib-injector fetches it itself when absent. */
class AuthlibInjectorMetadataStep : public AuthStep {
    Q_OBJECT

   public:
    explicit AuthlibInjectorMetadataStep(AccountData* data);
    ~AuthlibInjectorMetadataStep() noexcept override = default;

    void perform() override;
    QString describe() override;

   private slots:
    void onRequestDone(QByteArray* response);

   private:
    Net::Request::Ptr m_request;
    NetJob::Ptr m_task;
};
