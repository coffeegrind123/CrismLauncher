# Download mirror

Status: implemented (2026-10). Code: `launcher/net/DownloadMirror.*`, hook in `Net::Request::executeTask()`.
Settings (Services page): `DownloadMirrorMode` 0 Off (default) | 1 PreferMirror | 2 MirrorOnly,
`DownloadMirrorURL` (empty = https://bmclapi2.bangbang93.com/). Tests: `tests/DownloadMirror_test.cpp`.

## Rule

A request goes to the mirror only if it is a GET whose sink has a validator pinning the content
(`Validator::pinsContent()`, i.e. a `ChecksumValidator` with an expected hash from official metadata).
Unpinned downloads (Fabric/Quilt libraries in Prism meta, legacy FML libs) always use the official host.

| official | mirror path |
|---|---|
| piston-meta / launchermeta / piston-data / launcher .mojang.com `/<p>` | `/<p>` |
| libraries.minecraft.net `/<p>` | `/maven/<p>` |
| resources.download.minecraft.net `/<p>` | `/assets/<p>` |
| maven.minecraftforge.net `/<p>`, files.minecraftforge.net `/maven/<p>` | `/maven/<p>` |
| maven.neoforged.net `/releases/<p>` | `/maven/<p>` |
| maven.fabricmc.net `/<p>` | `/maven/<p>` |

Not mirrored (no BMCLAPI route): maven.quiltmc.org, meta.prismlauncher.org, lwjgl, Maven Central.
URLs with userinfo, a non-default port or `.`/`..` segments are never rewritten.

## Fallback (PreferMirror)

Any mirror failure (HTTP error incl. 429, timeout, write failure, checksum mismatch, too many redirects) makes the
same `Request` abort its sink and resend to the original URL; NetJob never sees a failure. 8 consecutive mirror
failures disable the mirror for the session (`DownloadMirror::isDisabledForSession`). MirrorOnly never falls back;
the Java runtime `manifest.json` fails there because BMCLAPI serves a re-minified copy (different SHA-1).

Every attempt, NetJob retries included, starts again from the requested URL (previously from the last redirect).
