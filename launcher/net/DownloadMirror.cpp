#include "DownloadMirror.h"

#include <QStringList>

#include <array>
#include <cstddef>

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

bool hasDefaultPort(const QUrl& url)
{
    return url.port() == -1 || (url.scheme() == "https" && url.port() == 443) || (url.scheme() == "http" && url.port() == 80);
}

bool hasDotSegment(const QString& path)
{
    const auto segments = path.split('/');
    return segments.contains("..") || segments.contains(".");
}

struct McimRoute {
    const char* host;
    const char* pathPrefix;    //!< on the official host, with leading and trailing '/'
    const char* mirrorPrefix;  //!< replaces pathPrefix under the MCIM root, with trailing '/'
    ModPlatformMirror Config::* platform;
    bool needsPin;  //!< file downloads; API responses are what MCIM serves unpinned
};

// Checked live (2026-10): API answers match the official ones, files are served or redirected
constexpr std::array s_mcimRoutes{
    McimRoute{ "api.modrinth.com", "/v2/", "modrinth/v2/", &Config::modrinth, false },
    McimRoute{ "cdn.modrinth.com", "/data/", "data/", &Config::modrinth, true },
    McimRoute{ "api.curseforge.com", "/v1/", "curseforge/v1/", &Config::curseForge, false },
    McimRoute{ "edge.forgecdn.net", "/files/", "files/", &Config::curseForge, true },
    McimRoute{ "mediafilez.forgecdn.net", "/files/", "files/", &Config::curseForge, true },
};

std::array<int, 2> s_consecutiveFailures{};

int& failures(Provider provider)
{
    return s_consecutiveFailures[static_cast<std::size_t>(provider)];
}

//! http(s), no userinfo, default port, no dot segments: safe to move under another root
bool isPlainUrl(const QUrl& url)
{
    if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http")) {
        return false;
    }
    if (!url.userInfo().isEmpty() || !hasDefaultPort(url)) {
        return false;
    }
    return !hasDotSegment(url.path(QUrl::FullyDecoded));
}

#ifdef LAUNCHER_APPLICATION
ModPlatformMirror platformSetting(const QVariant& value)
{
    return value.toInt() == static_cast<int>(ModPlatformMirror::Mcim) ? ModPlatformMirror::Mcim : ModPlatformMirror::Official;
}
#endif
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

std::optional<QUrl> rewriteMcim(const QUrl& original, const Config& config, Verb verb, bool contentPinned)
{
    if (verb == Verb::Other || !isPlainUrl(original)) {
        return std::nullopt;
    }

    // Work on the encoded path so names like "fabric-api-0.161.2%2B26.4.jar" keep their escapes
    const auto host = original.host().toLower();
    const auto path = original.path(QUrl::FullyEncoded);
    for (const auto& route : s_mcimRoutes) {
        if (host != QLatin1String(route.host) || !path.startsWith(QLatin1String(route.pathPrefix))) {
            continue;
        }
        if (config.*route.platform != ModPlatformMirror::Mcim) {
            return std::nullopt;
        }
        if (route.needsPin && (verb != Verb::Get || !contentPinned)) {
            return std::nullopt;
        }

        const QUrl base(MCIM_BASE_URL);
        const auto rest = path.mid(static_cast<qsizetype>(qstrlen(route.pathPrefix)));
        QUrl mirrored(base);
        mirrored.setPath(base.path() + QLatin1String(route.mirrorPrefix) + rest, QUrl::TolerantMode);
        mirrored.setQuery(original.query(QUrl::FullyEncoded), QUrl::TolerantMode);
        return mirrored;
    }
    return std::nullopt;
}

std::optional<Target> choose(const QUrl& original, const Config& config, Verb verb, bool contentPinned)
{
    if (!isDisabledForSession(Provider::Mcim)) {
        if (auto mirrored = rewriteMcim(original, config, verb, contentPinned)) {
            return Target{ *mirrored, Provider::Mcim, true };
        }
    }

    const bool bmclapiUsable =
        config.mode == Mode::MirrorOnly || (config.mode == Mode::PreferMirror && !isDisabledForSession(Provider::Bmclapi));
    if (bmclapiUsable && verb == Verb::Get && contentPinned) {
        if (auto mirrored = rewrite(original, config.base)) {
            return Target{ *mirrored, Provider::Bmclapi, config.mode == Mode::PreferMirror };
        }
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

    Config config;
    config.modrinth = platformSetting(settings->get("ModrinthMirror"));
    config.curseForge = platformSetting(settings->get("CurseForgeMirror"));

    const auto mode = settings->get("DownloadMirrorMode").toInt();
    if (mode != static_cast<int>(Mode::PreferMirror) && mode != static_cast<int>(Mode::MirrorOnly)) {
        return config;
    }

    auto baseText = settings->get("DownloadMirrorURL").toString().trimmed();
    if (baseText.isEmpty()) {
        baseText = DEFAULT_BASE_URL;
    }
    const QUrl base(baseText, QUrl::StrictMode);
    if (!base.isValid() || base.host().isEmpty() || (base.scheme() != "https" && base.scheme() != "http")) {
        return config;
    }
    config.mode = static_cast<Mode>(mode);
    config.base = base;
    return config;
#else
    return {};
#endif
}

void reportSuccess(Provider provider)
{
    failures(provider) = 0;
}

void reportFailure(Provider provider)
{
    failures(provider)++;
}

bool isDisabledForSession(Provider provider)
{
    return failures(provider) >= FAILURES_BEFORE_DISABLING;
}

}  // namespace Net::DownloadMirror
