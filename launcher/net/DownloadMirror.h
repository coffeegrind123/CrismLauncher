#pragma once

#include <QUrl>

#include <optional>

/**
 * Optional download mirror (BMCLAPI or any server with the same layout) for game files.
 *
 * Only requests whose content is pinned by a checksum from official metadata are sent to the
 * mirror, so a mirror can make downloads fail but can't change what gets installed. In
 * PreferMirror mode a request that fails on the mirror for any reason (HTTP error, timeout,
 * rate limit, checksum mismatch) is retried against the original URL.
 *
 *   official URL                                   mirror URL
 *   piston-meta|piston-data|launcher[meta].mojang.com/<p>     <base>/<p>
 *   libraries.minecraft.net/<p>                    <base>/maven/<p>
 *   resources.download.minecraft.net/<p>           <base>/assets/<p>
 *   maven.minecraftforge.net/<p>                   <base>/maven/<p>
 *   files.minecraftforge.net/maven/<p>             <base>/maven/<p>
 *   maven.neoforged.net/releases/<p>               <base>/maven/<p>
 *   maven.fabricmc.net/<p>                         <base>/maven/<p>
 */
namespace Net::DownloadMirror {

enum class Mode { Off = 0, PreferMirror = 1, MirrorOnly = 2 };

inline const QString DEFAULT_BASE_URL = QStringLiteral("https://bmclapi2.bangbang93.com/");

//! Consecutive mirror failures after which PreferMirror stops trying the mirror for the session
constexpr int FAILURES_BEFORE_DISABLING = 8;

/** The mirror URL for `original`, or nothing when the host isn't mirrored or the URL could
 *  escape the mirrored tree (userinfo, unusual port, dot segments). `base` is the mirror root. */
std::optional<QUrl> rewrite(const QUrl& original, const QUrl& base);

struct Config {
    Mode mode = Mode::Off;
    QUrl base;
};

/** The configured mirror; Off when the base URL setting is not a usable https/http URL. */
Config currentConfig();

/** Session-wide circuit breaker: after FAILURES_BEFORE_DISABLING failures in a row the mirror is
 *  skipped, so an outage costs a few timeouts instead of one per file. */
void reportSuccess();
void reportFailure();
bool isDisabledForSession();

}  // namespace Net::DownloadMirror
