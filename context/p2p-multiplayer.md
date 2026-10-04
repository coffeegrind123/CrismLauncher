# P2P multiplayer (Terracotta, YukariConnect)

Status: not scheduled (2026-10; researched, user chose not to port yet). Ported from LunaLauncher (GPL-3.0, `launcher/minecraft/online/*`,
`launcher/ui/pages/global/{Terracotta,YukariConnect}*`); Luna copyright lines are kept on derived files.

Lets two players join each other's LAN world over the internet without port forwarding. The launcher runs a helper
program that builds an EasyTier P2P network and speaks the Scaffolding protocol, so rooms are interoperable with
HMCL, PCL CE and Luna.

## Helpers

| | Terracotta | YukariConnect |
|---|---|---|
| Upstream | github.com/burningtnt/Terracotta, AGPL-3.0 + launcher exception | github.com/ElicaseTech/YukariConnect, MPL-2.0 |
| Binary source | upstream releases (`/releases/latest`), every desktop platform | fork `coffeegrind123/YukariConnect` releases (upstream ships only a win-x64 prerelease) |
| Asset | `terracotta-<v>-<os>-<arch>-pkg.tar.gz` (os: windows, linux, macos, freebsd) | `YukariConnect-<rid>-<v>[.exe]` (rid: win-x64, win-arm64, linux-x64, linux-arm64, osx-x64, osx-arm64) |
| Start | `terracotta --hmcl <dataRoot>/p2p/terracotta/port.json` | `YukariConnect` (stdout `YUKARI_PORT_INFO:port=<n>`, default 5062) |
| Port discovery | poll the port file (`{"port": n}`, written atomically) | parse the stdout line; fall back to 5062 + `GET /meta` |
| Stop | `GET /panic?peaceful=true` (process may have detached on Windows) | `POST /room/stop`, then terminate the process |
| Vendor string | n/a | `POST /config/launcher {"launcherCustomString": "<Launcher_Name>/<version>"}` |

Licence conditions we rely on:
- Terracotta: binary unmodified, used only over HTTP, and its copyright shown prominently in our UI (the exception in
  its README). The online window shows "Terracotta © burningtnt, AGPL-3.0" with a link, always visible.
- YukariConnect: MPL-2.0 file-level copyleft; the fork's changes stay public in the fork.

Fork `coffeegrind123/YukariConnect` (new repo): upstream at pinned commit `ea488a71` plus
- a release workflow: `dotnet publish -c Release -r <rid> --self-contained -p:PublishAot=true -p:PublishSingleFile=true`
  per rid (native runners per OS), assets uploaded to a GitHub release, so GitHub computes sha256 digests;
- fix: `EasyTierResourceInitializer` tests for `easytier-core.exe` on every OS, so non-Windows re-downloads EasyTier on
  every start. Test the platform's executable name.

## Install (`P2PHelperInstallTask`, a `Task`)

```
GET api.github.com/repos/<repo>/releases(/latest)   -> pick asset for OS+arch, read its "digest" (sha256:...)
Net::Download asset (Gitee mirror optional for Terracotta) + ChecksumValidator(sha256)
extract tar.gz/zip with ArchiveReader into <dataRoot>/p2p/<helper>/<version>/, chmod +x
write <dataRoot>/p2p/<helper>/installed.json {version, asset, sha256, path}; delete older versions
```

- Release metadata always comes from the GitHub API. With the Gitee source only the asset bytes come from Gitee, and
  they must match GitHub's digest. No digest -> install refused (never run unverified code).
- Archive entries with absolute paths or `..` are rejected.
- Luna bugs not carried over: dead repo `YukariC/YukariConnect`; `/releases/latest` on a repo with only prereleases
  (404); no hash check; raw QNetworkAccessManager outside the launcher's proxy/UA settings.

## Client (`P2PService`, owned by `Application`)

One state model for both helpers; YukariConnect implements Terracotta's API, with camelCase field names.

```
GET  /state                                 -> {state, index, room, url, profile_index|profileIndex,
                                                profiles[{name, machine_id|machineId, vendor, kind}],
                                                difficulty, type}
GET  /state/scanning?room=&player=&public_nodes=...   host (Terracotta >= 0.4.2 takes public_nodes)
GET  /state/guesting?room=&player=&public_nodes=...   join; 400 = bad code or not waiting
GET  /state/ide                             back to waiting (also exception recovery)
GET  /meta                                  version, easytier_version
GET  /log?fetch=true                        Terracotta log file
```

States: waiting, host-scanning, host-starting, host-ok, guest-connecting, guest-starting, guest-ok, exception
(type 0 PingHostFail, 1 PingHostRst, 2 GuestEasytierCrash, 3 HostEasytierCrash, 4 PingServerRst,
5 ScaffoldingInvalidResponse). `index` changes -> refresh everything; only `profile_index` -> refresh player list.

- Fully asynchronous: QTimer polling (setting, default 1000 ms) and signals. No nested event loops, `msleep`, or
  `waitFor*` on the GUI thread (Luna does all of these).
- Unknown `state` values and unparsable bodies are logged with the raw body and surfaced as "unrecognised response".
- Requests go to 127.0.0.1 only, through a dedicated QNetworkAccessManager with proxy disabled (a system proxy must
  not see local traffic).
- Player name defaults to the selected account's profile name; editable, remembered.
- On launcher exit: stop the helper if `P2P/StopOnClose` (default true); otherwise leave it running and reattach on next
  start via the port file / `GET /meta`.

## UI

- Main window toolbar action "Online" (icon `terracotta-online` in every icon theme) -> `OnlineMultiplayerDialog`,
  a non-modal window: helper picker, status, host (shows room code + copy), join (code field with format check),
  leave, player list, guest address + copy, "Launch and join": choose an instance, launch it with quick-play
  to the guest address (existing `MinecraftTarget` server join), log pane (max lines setting), fetch log, restart,
  force stop, open helper web UI, attribution line.
  "Launch and join" is an addition; Luna's panel stops at showing the address.
- Status bar label: helper + state, coloured; hidden by setting.
- Settings page "Multiplayer" (global settings): per helper installed version/size/path, install/update/delete,
  download source (GitHub/Gitee for Terracotta), server URL override (attach to an already-running helper),
  Terracotta startup mode (HMCL port file / foreground), Terracotta public nodes (one per line), poll interval,
  max log lines, stop on close; LauncherPage gets toolbar/status-bar visibility toggles.

Settings: `P2P/Backend` (terracotta|yukari), `P2P/PlayerName`, `P2P/PollInterval`, `P2P/MaxLogLines`,
`P2P/StopOnClose`, `P2P/ShowToolbar`, `P2P/ShowStatusBar`, `Terracotta/Source` (github|gitee),
`Terracotta/ServerURL`, `Terracotta/StartupMode`, `Terracotta/PublicNodes`, `YukariConnect/ServerURL`.

## Tests (`tests/P2P_test.cpp`, fixtures in `tests/testdata/P2P/`)

State JSON parsing for every state from both helpers (captured from real v0.4.2 / fork binaries in CI), room-code
validation, asset picking per OS/arch from real release JSON, digest parsing, archive path-traversal rejection,
port-file and `YUKARI_PORT_INFO` parsing.
