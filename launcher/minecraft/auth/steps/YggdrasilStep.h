#pragma once

#include <QJsonObject>
#include <QObject>

#include <functional>
#include <optional>

#include "minecraft/auth/AuthStep.h"
#include "minecraft/auth/Yggdrasil.h"
#include "net/Request.h"

/**
 * Logs in to, or refreshes the session of, a Yggdrasil-compatible server (authserver endpoints).
 *
 *   login:    authenticate -> [pick a profile -> refresh with selectedProfile]
 *   refresh:  validate -> (204: token still good) | refresh
 *
 * Validating first keeps a still-valid token, which a running game may hold, from being rotated.
 */
class YggdrasilStep : public AuthStep {
    Q_OBJECT

   public:
    //! With a password this logs in; without one it refreshes the stored session
    explicit YggdrasilStep(AccountData* data, std::optional<QString> password = std::nullopt);
    ~YggdrasilStep() noexcept override = default;

    void perform() override;
    QString describe() override;

   public slots:
    void abort() override;

   signals:
    /** Emitted on login when the account has several profiles and none is selected. Handlers set
     *  `chosenIndex` to an index into `names`, or leave it at -1 to cancel. Direct connection only. */
    void selectProfile(const QStringList& names, int* chosenIndex);

    //! Login was refused until a TOTP code is appended to the password ("password:code"; Ely.by)
    void twoFactorRequired();

   private:
    using ResponseHandler = std::function<void(int status, const QByteArray& body)>;

    void post(const QString& endpoint, const QJsonObject& body, ResponseHandler handler);

    void authenticate();
    void validate();
    void refresh(const std::optional<Yggdrasil::Profile>& selectProfile = std::nullopt);

    void onSession(int status, const QByteArray& body, bool isLogin);
    void finishWithError(int status, const QByteArray& body, const QString& action);

   private:
    std::optional<QString> m_password;
    Net::Request::Ptr m_request;
};
