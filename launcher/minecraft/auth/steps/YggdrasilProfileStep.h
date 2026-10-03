#pragma once

#include <QObject>

#include "minecraft/auth/AuthStep.h"
#include "net/NetJob.h"
#include "net/Request.h"

//! Fetches the selected profile's textures from a Yggdrasil server's session server
class YggdrasilProfileStep : public AuthStep {
    Q_OBJECT

   public:
    explicit YggdrasilProfileStep(AccountData* data);
    ~YggdrasilProfileStep() noexcept override = default;

    void perform() override;
    QString describe() override;

   private slots:
    void onRequestDone(QByteArray* response);

   private:
    Net::Request::Ptr m_request;
    NetJob::Ptr m_task;
};
