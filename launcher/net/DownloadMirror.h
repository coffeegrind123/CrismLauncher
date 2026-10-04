#pragma once

#include <QUrl>

#include <cstdint>
#include <optional>

/**
 * Optional download mirrors.
 *
 * BMCLAPI (or any server with the same layout) for game files. Only requests whose content is
 * pinned by a checksum from official metadata are sent there, so the mirror can make downloads
 * fail but can't change what gets installed. In PreferMirror mode a request that fails on the
 * mirror for any reason (HTTP error, timeout, rate limit, checksum mismatch) is retried against
 * the original URL.
 *
 *   official URL                                   mirror URL
 *   piston-meta|piston-data|launcher[meta].mojang.com/<p>     <base>/<p>
 *   libraries.minecraft.net/<p>                    <base>/maven/<p>
 *   resources.download.minecraft.net/<p>           <base>/assets/<p>
 *   maven.minecraftforge.net/<p>                   <base>/maven/<p>
 *   files.minecraftforge.net/maven/<p>             <base>/maven/<p>
 *   maven.neoforged.net/releases/<p>               <base>/maven/<p>
 *   maven.fabricmc.net/<p>                         <base>/maven/<p>
 *
 * MCIM (mod.mcimirror.top) for the Modrinth and CurseForge APIs and their file CDNs, switched on
 * per platform. API requests (GET and POST) go there unpinned, since mod metadata is what MCIM
 * caches; file downloads still need a content pin. Every MCIM failure falls back to the official
 * URL, including 404s for routes MCIM doesn't implement (CurseForge descriptions, changelogs).
 *
 *   api.modrinth.com/v2/<p>                        mod.mcimirror.top/modrinth/v2/<p>
 *   api.curseforge.com/v1/<p>                      mod.mcimirror.top/curseforge/v1/<p>
 *   cdn.modrinth.com/data/<p>                      mod.mcimirror.top/data/<p>          (pinned only)
 *   (edge|mediafilez).forgecdn.net/files/<p>       mod.mcimirror.top/files/<p>         (pinned only)
 */
namespace Net::DownloadMirror {

enum class Mode { Off = 0, PreferMirror = 1, MirrorOnly = 2 };

//! Values of the ModrinthMirror / CurseForgeMirror settings
enum class ModPlatformMirror { Official = 0, Mcim = 1 };

enum class Provider : std::uint8_t { Bmclapi, Mcim };

enum class Verb : std::uint8_t { Get, Post, Other };

inline const QString DEFAULT_BASE_URL = QStringLiteral("https://bmclapi2.bangbang93.com/");
inline const QString MCIM_BASE_URL = QStringLiteral("https://mod.mcimirror.top/");

//! Consecutive mirror failures after which a mirror is skipped for the rest of the session
constexpr int FAILURES_BEFORE_DISABLING = 8;

/** The BMCLAPI mirror URL for `original`, or nothing when the host isn't mirrored or the URL could
 *  escape the mirrored tree (userinfo, unusual port, dot segments). `base` is the mirror root. */
std::optional<QUrl> rewrite(const QUrl& original, const QUrl& base);

struct Config {
    Mode mode = Mode::Off;
    QUrl base;
    ModPlatformMirror modrinth = ModPlatformMirror::Official;
    ModPlatformMirror curseForge = ModPlatformMirror::Official;
};

/** The MCIM URL for `original` under `config`, or nothing (platform not switched to MCIM, host not
 *  mirrored, unsafe URL, or a file download without a content pin). */
std::optional<QUrl> rewriteMcim(const QUrl& original, const Config& config, Verb verb, bool contentPinned);

struct Target {
    QUrl url;
    Provider provider;
    //! false only for BMCLAPI in MirrorOnly mode
    bool mayFallBack;
};

/** Where a request for `original` goes first, or nothing for the official URL. Skips mirrors the
 *  circuit breaker has disabled. */
std::optional<Target> choose(const QUrl& original, const Config& config, Verb verb, bool contentPinned);

/** The configured mirrors; BMCLAPI is Off when its base URL setting is not a usable https/http URL. */
Config currentConfig();

/** Session-wide circuit breaker per mirror: after FAILURES_BEFORE_DISABLING failures in a row the
 *  mirror is skipped, so an outage costs a few timeouts instead of one per file. A 404 is not
 *  reported as a failure: it means one missing object, not a broken mirror. */
void reportSuccess(Provider provider);
void reportFailure(Provider provider);
bool isDisabledForSession(Provider provider);

}  // namespace Net::DownloadMirror
