# Alternative login servers (authlib-injector)

Status: implemented (2026-10). Code: `launcher/minecraft/auth/Yggdrasil.*`, `YggdrasilServerResolveTask.*`,
`steps/Yggdrasil*Step.*`, `steps/AuthlibInjectorMetadataStep.*`, `minecraft/launch/EnsureAuthlibInjector.*`,
`ui/dialogs/AuthlibInjectorLoginDialog.*`. Tests: `tests/Yggdrasil_test.cpp` (fixtures in `tests/testdata/Yggdrasil/`).

## Account model

`AccountType::AuthlibInjector`, stored in `accounts.json` with Fjord Launcher's keys so files move both ways:

```json
{
  "type": "AuthlibInjector",
  "authlibInjectorUrl": "https://authserver.ely.by/api/authlib-injector",
  "authlibInjectorMetadata": "<base64 of GET authlibInjectorUrl>",
  "customAuthServerUrl": "<root>/authserver",
  "customAccountServerUrl": "<root>/api",
  "customSessionServerUrl": "<root>/sessionserver",
  "customServicesServerUrl": "<root>/minecraftservices",
  "ygg": { "token": "<access token>", "extra": { "userName": "<login>", "clientToken": "<uuid>" } },
  "profile": { "id": "...", "name": "...", "canUploadSkins": false }
}
```

- `custom*Url` are written only for Fjord; on load the root is `authlibInjectorUrl`, else `customAuthServerUrl` minus `/authserver`.
- Fjord's legacy `"type": "Elyby"` loads as Ely.by's root.
- `canUploadSkins` is written only when false (absent = allowed).
- Passwords are never stored.
- Accounts are identified by (type, `authlibInjectorUrl`, profile id): `AccountList::findSameAccount`.

## Flows

```
discover:  GET <user URL> --X-Authlib-Injector-API-Location (relative or absolute)--> GET <root> = metadata
login:     POST /authserver/authenticate {agent, username, password[:totp], clientToken, requestUser:false}
           selectedProfile? -> done
           0 profiles       -> account without character (launch tells user to create one on the website)
           1..n profiles    -> pick (dialog if n>1) -> POST /authserver/refresh {.., selectedProfile} to bind
refresh:   POST /authserver/validate -> 204 keep token | else POST /authserver/refresh
profile:   GET /sessionserver/session/minecraft/profile/<id>?unsigned=false  (textures optional)
metadata:  GET <root> each refresh; failures keep the stored copy
```

Error mapping: no HTTP response or 5xx -> STATE_OFFLINE (offline launch possible); ForbiddenOperationException ->
STATE_FAILED_HARD (token cleared, reauth); Ely "Account protected with two factor auth." -> TOTP field shown.
Auth POSTs run as single `Net::Request`s, never in a `NetJob`, which would retry a rejected password 3 times.

## Launch

- `EnsureAuthlibInjector` (after `ClaimAccount`, normal launches only): latest.json from authlib-injector.yushi.moe,
  then BMCLAPI; download host must be one of those over https; jar verified by sha256 into
  `cache/authlib-injector/authlib-injector-<version>.jar`; newest cached jar is used when offline.
  Setting `AuthlibInjectorPath` overrides with a local jar.
- JVM args first: `-javaagent:<jar>=<root>` and `-Dauthlibinjector.yggdrasil.prefetched=<base64>` (omitted >16 KiB).
- `user_type` is `msa` (`mojang` disables profile keys, breaking chat on 1.19.3+ secure-profile servers).
- Legacy online fixes are off for these sessions (they hard-code Mojang hosts).

## Skins

`PUT|DELETE <root>/api/user/profile/<uuid>/skin`, Bearer token, multipart `model` (`slim` or empty) + `file`.
Ely.by has no upload API, so Manage Skins is disabled there. No cape API: the cape picker is disabled.

## Verified against Drasl (local Docker, 2026-10-04)

ALI header absolute; validate 204/403; authenticate with the account name selects the matching character even
with several (Drasl selects characters by login name); refresh rotates the token and invalidates the old one;
profile `textures` is `{}` without a skin; skin upload/delete return 204 and honour `model=slim`.
Not testable here: Ely.by/LittleSkin real logins, the multi-profile picker (needs an e-mail-login server),
joining a server in-game.

## Not done

Ely.by OAuth device-code login: needs an OAuth client registered with Ely.by (`invalid_client` otherwise).
LittleSkin advertises OIDC ("Yggdrasil Connect"); password login still works.
