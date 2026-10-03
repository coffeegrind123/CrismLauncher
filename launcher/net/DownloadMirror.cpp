#include "DownloadMirror.h"

#include <QStringList>

#include <array>

#ifdef LAUNCHER_APPLICATION
#include "Application.h"
#include "settings/SettingsObject.h"
#endif

namespace Net::DownloadMirror {

namespace {
struct Route {
    const char* host;
    const char* pathPrefix;    //!< on the official host, with leading and trailing '/'
    const char* mirrorPrefix;  //!< under the mirror base, with trailing '/'
};

// Each mapping was checked against the official SHA-1 of a real file (2026-10)
constexpr std::array s_routes{
    Route{ "piston-meta.mojang.com", "/", "" },
    Route{ "launchermeta.mojang.com", "/", "" },
    Route{ "piston-data.mojang.com", "/", "" },
    Route{ "launcher.mojang.com", "/", "" },
    Route{ "libraries.minecraft.net", "/", "maven/" },
    Route{ "resources.download.minecraft.net", "/", "assets/" },
    Route{ "maven.minecraftforge.net", "/", "maven/" },
    Route{ "files.minecraftforge.net", "/maven/", "maven/" },
    Route{ "maven.neoforged.net", "/releases/", "maven/" },
    Route{ "maven.fabricmc.net", "/", "maven/" },
};

int s_consecutiveFailures = 0;

bool hasDefaultPort(const QUrl& url)
{
    return url.port() == -1 || (url.scheme() == "https" && url.port() == 443) || (url.scheme() == "http" && url.port() == 80);
}

bool hasDotSegment(const QString& path)
{
    const auto segments = path.split('/');
    return segments.contains("..") || segments.contains(".");
}
}  // namespace

std::optional<QUrl> rewrite(const QUrl& original, const QUrl& base)
{
    if (!original.isValid() || !base.isValid() || base.host().isEmpty()) {
        return std::nullopt;
    }
    if (original.scheme() != "https" && original.scheme() != "http") {
        return std::nullopt;
    }
    if (!original.userInfo().isEmpty() || !hasDefaultPort(original)) {
        return std::nullopt;
    }

    const auto path = original.path(QUrl::FullyDecoded);
    if (hasDotSegment(path)) {
        return std::nullopt;
    }

    const auto host = original.host().toLower();
    for (const auto& route : s_routes) {
        if (host != QLatin1String(route.host) || !path.startsWith(QLatin1String(route.pathPrefix))) {
            continue;
        }

        auto basePath = base.path();
        if (!basePath.endsWith('/')) {
            basePath += '/';
        }

        const auto rest = path.mid(static_cast<qsizetype>(qstrlen(route.pathPrefix)));
        QUrl mirrored(base);
        mirrored.setPath(basePath + QLatin1String(route.mirrorPrefix) + rest, QUrl::DecodedMode);
        mirrored.setQuery(original.query());
        return mirrored;
    }
    return std::nullopt;
}

Config currentConfig()
{
#ifdef LAUNCHER_APPLICATION
    if (APPLICATION == nullptr || APPLICATION->settings() == nullptr) {
        return {};
    }
    const auto settings = APPLICATION->settings();

    const auto mode = settings->get("DownloadMirrorMode").toInt();
    if (mode != static_cast<int>(Mode::PreferMirror) && mode != static_cast<int>(Mode::MirrorOnly)) {
        return {};
    }

    auto baseText = settings->get("DownloadMirrorURL").toString().trimmed();
    if (baseText.isEmpty()) {
        baseText = DEFAULT_BASE_URL;
    }
    const QUrl base(baseText, QUrl::StrictMode);
    if (!base.isValid() || base.host().isEmpty() || (base.scheme() != "https" && base.scheme() != "http")) {
        return {};
    }
    return { static_cast<Mode>(mode), base };
#else
    return {};
#endif
}

void reportSuccess()
{
    s_consecutiveFailures = 0;
}

void reportFailure()
{
    s_consecutiveFailures++;
}

bool isDisabledForSession()
{
    return s_consecutiveFailures >= FAILURES_BEFORE_DISABLING;
}

}  // namespace Net::DownloadMirror
