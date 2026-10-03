#pragma once

#include <launch/LaunchStep.h>

#include "minecraft/auth/AuthSession.h"
#include "minecraft/auth/Yggdrasil.h"
#include "net/NetJob.h"

/**
 * Makes the authlib-injector agent jar available for a session on a Yggdrasil server and records
 * its path in the session. Releases are looked up from the official source, then the BMCLAPI
 * mirror; the jar is verified against the published sha256. When neither is reachable, the
 * newest previously downloaded jar is used.
 */
class EnsureAuthlibInjector : public LaunchStep {
    Q_OBJECT

   public:
    explicit EnsureAuthlibInjector(LaunchTask* parent, AuthSessionPtr session);
    ~EnsureAuthlibInjector() override = default;

    void executeTask() override;
    bool canAbort() const override { return true; }
    bool abort() override;

   private:
    void fetchRelease(int sourceIndex);
    void downloadJar(const Yggdrasil::AgentArtifact& artifact);
    void useCachedJar(const QString& reason);
    void finish(const QString& jarPath);

    QString cacheDir() const;

   private:
    AuthSessionPtr m_session;
    NetJob::Ptr m_job;
};
