#include "EnsureAuthlibInjector.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "Application.h"
#include "FileSystem.h"
#include "net/ChecksumValidator.h"
#include "settings/SettingsObject.h"

namespace {
const QString s_jarPrefix = QStringLiteral("authlib-injector-");

QByteArray sha256OfFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}
}  // namespace

EnsureAuthlibInjector::EnsureAuthlibInjector(LaunchTask* parent, AuthSessionPtr session) : LaunchStep(parent), m_session(std::move(session))
{}

QString EnsureAuthlibInjector::cacheDir() const
{
    return APPLICATION->metacache()->getBasePath("authlibinjector");
}

void EnsureAuthlibInjector::executeTask()
{
    const auto customPath = APPLICATION->settings()->get("AuthlibInjectorPath").toString();
    if (!customPath.isEmpty()) {
        if (!QFileInfo(customPath).isFile()) {
            emitFailed(tr("The configured authlib-injector jar does not exist: %1").arg(customPath));
            return;
        }
        emit logLine(tr("Using the configured authlib-injector jar %1").arg(customPath), MessageLevel::Launcher);
        finish(customPath);
        return;
    }

    setStatus(tr("Checking for authlib-injector updates"));
    fetchRelease(0);
}

bool EnsureAuthlibInjector::abort()
{
    if (m_job) {
        m_job->abort();
    }
    emitAborted();
    return true;
}

void EnsureAuthlibInjector::fetchRelease(int sourceIndex)
{
    const QUrl source(Yggdrasil::AGENT_RELEASE_SOURCES.at(sourceIndex));
    auto [request, response] = Net::Request::makeByteArray(source);

    m_job.reset(new NetJob("authlib-injector release", APPLICATION->network()));
    m_job->setAskRetry(false);
    m_job->addNetAction(request);

    connect(m_job.get(), &Task::finished, this, [this, request, response, sourceIndex, source] {
        if (m_job && m_job->getState() == Task::State::AbortedByUser) {
            return;
        }

        const auto artifact = request->error() == QNetworkReply::NoError ? Yggdrasil::parseAgentArtifact(*response) : std::nullopt;
        if (artifact) {
            downloadJar(*artifact);
            return;
        }

        emit logLine(tr("Could not get the authlib-injector release from %1: %2").arg(source.host(), request->errorString()),
                     MessageLevel::Warning);
        if (sourceIndex + 1 < Yggdrasil::AGENT_RELEASE_SOURCES.size()) {
            fetchRelease(sourceIndex + 1);
            return;
        }
        useCachedJar(tr("no release source is reachable"));
    });
    m_job->start();
}

void EnsureAuthlibInjector::downloadJar(const Yggdrasil::AgentArtifact& artifact)
{
    const auto jarPath = FS::PathCombine(cacheDir(), s_jarPrefix + artifact.version + ".jar");
    if (QFileInfo(jarPath).isFile() && sha256OfFile(jarPath) == artifact.sha256) {
        emit logLine(tr("Using authlib-injector %1").arg(artifact.version), MessageLevel::Launcher);
        finish(jarPath);
        return;
    }

    setStatus(tr("Downloading authlib-injector %1").arg(artifact.version));
    emit logLine(tr("Downloading authlib-injector %1 from %2").arg(artifact.version, artifact.downloadUrl.host()), MessageLevel::Launcher);

    if (!FS::ensureFolderPathExists(cacheDir())) {
        emitFailed(tr("Could not create %1").arg(cacheDir()));
        return;
    }

    auto request = Net::Request::makeFile(artifact.downloadUrl, jarPath);
    request->addValidator(new Net::ChecksumValidator(QCryptographicHash::Sha256, artifact.sha256));

    m_job.reset(new NetJob("authlib-injector download", APPLICATION->network()));
    m_job->setAskRetry(false);
    m_job->addNetAction(request);

    connect(m_job.get(), &Task::progress, this, &EnsureAuthlibInjector::setProgress);
    connect(m_job.get(), &Task::finished, this, [this, request, jarPath] {
        if (m_job && m_job->getState() == Task::State::AbortedByUser) {
            return;
        }
        if (request->error() != QNetworkReply::NoError || !m_job->wasSuccessful()) {
            useCachedJar(m_job->failReason());
            return;
        }
        finish(jarPath);
    });
    m_job->start();
}

void EnsureAuthlibInjector::useCachedJar(const QString& reason)
{
    // Prefer the most recently downloaded jar; each one was verified when it was saved
    const auto jars = QDir(cacheDir()).entryInfoList({ s_jarPrefix + "*.jar" }, QDir::Files, QDir::Time);
    if (jars.isEmpty()) {
        emitFailed(tr("Could not download authlib-injector (%1), and no earlier copy is available. "
                      "The game needs it to log in to %2.")
                       .arg(reason, QUrl(m_session->authlib_injector_url).host()));
        return;
    }

    const auto jarPath = jars.first().absoluteFilePath();
    emit logLine(tr("Could not update authlib-injector (%1), using %2").arg(reason, jars.first().fileName()), MessageLevel::Warning);
    finish(jarPath);
}

void EnsureAuthlibInjector::finish(const QString& jarPath)
{
    m_session->authlib_injector_jar = QFileInfo(jarPath).absoluteFilePath();
    emitSucceeded();
}
