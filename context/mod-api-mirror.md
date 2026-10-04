# Mod platform mirror (MCIM)

Status: implemented (2026-10). Code: `launcher/net/DownloadMirror.*` (`rewriteMcim`, `choose`), hook in
`Net::Request::executeTask()`, UI `ui/widgets/MirrorSettingsWidget.*` (Services page and setup wizard).
Tests: `tests/DownloadMirror_test.cpp`. Idea from LunaLauncher's `ModApiMirror`, which only swaps the API base URL.

Service: https://mod.mcimirror.top (github.com/mcmod-info-mirror), a public cache of the Modrinth and CurseForge
APIs, mainly for users in mainland China.

Settings: `ModrinthMirror`, `CurseForgeMirror`: 0 Official (default) | 1 MCIM. No MirrorOnly: MCIM lacks routes.

## Routes

| official | MCIM | needs content pin |
|---|---|---|
| `api.modrinth.com/v2/<p>` | `mod.mcimirror.top/modrinth/v2/<p>` | no (GET, POST) |
| `api.curseforge.com/v1/<p>` | `mod.mcimirror.top/curseforge/v1/<p>` | no (GET, POST) |
| `cdn.modrinth.com/data/<p>` | `mod.mcimirror.top/data/<p>` | yes (GET) |
| `edge.forgecdn.net/files/<p>`, `mediafilez.forgecdn.net/files/<p>` | `mod.mcimirror.top/files/<p>` | yes (GET) |

Rewriting happens in the request layer, so the API classes keep the official URLs; the path stays percent-encoded
(`%2B` in Modrinth file names). Same safety checks as BMCLAPI: no userinfo, default port, no dot segments.

## Fallback

Any MCIM failure (no response, DNS, timeout, any HTTP error, checksum mismatch) resends the same request, body
included, to the official URL. 8 consecutive failures turn MCIM off for the session; BMCLAPI has its own counter.
404s are not counted (for either mirror): MCIM answers 404 for unknown hashes, which update checks hit often.

No credentials reach MCIM: `ApiHeaderProxy` adds the CurseForge key / Modrinth token by official host only, and
headers are recomputed per URL, so the fallback request does carry them.

CurseForge modpack files are now pinned with the file's sha1/md5 from the API (they were unverified before), which is
what lets MCIM serve them.

## Observed behaviour (probed 2026-10-04)

- Works: Modrinth project, projects batch, versions, search, tag/category, version_file(s), version_files/update;
  CurseForge mods (GET, POST), search, files, file, categories, fingerprints.
- 404 with empty body: CurseForge `/mods/<id>/description`, `/mods/<id>/files/<id>/changelog` -> fallback.
- Files: both CDNs answered with a 302 to the official CDN, i.e. no speed-up today; the route is harmless and starts
  helping when MCIM serves files again.
- Responses carry a `sync_at` field/header; data can be hours old.
- Some DNS filters resolve `mod.mcimirror.top` to 0.0.0.0 (the dev machine's does); the fallback covers it.
- MCIM may whitelist launchers by user agent; launchers register `<Name>/<version>` in
  mcmod-info-mirror/mcim-rust-api#12 (Luna: `LunaLauncher/<version>`). Requests to MCIM always send
  `BuildConfig.USER_AGENT` (`CrismLauncher/<x.y.z>`), ignoring `UserAgentOverride`; a test pins that format.
  Crism is not registered there yet.
