# MCP server: agents manage the launcher over one HTTP endpoint

Status (2026-11-07): plan confirmed; **phase 0 written, verified only through CI** (see §11).

Provisional decisions (asked 2026-11-07, no answer yet; recommended options taken, change any before phase 0):

| Question | Provisional decision |
|---|---|
| Exposure | **127.0.0.1 HTTP + `--mcp-stdio` shim.** No LAN, no OAuth (spec: auth is OPTIONAL; local servers SHOULD bind localhost). |
| Tool shape | **~12 core tools + an op catalog** reached through `search_ops` / `call` / `batch`. Fixed `tools/list`. No JS sandbox. |
| Accounts | **Manage, never secrets.** List, choose default/per-instance, add offline, refresh, remove (confirmed), MSA device-code login finished by the human. |
| Confirmation | **Per-token policy, in-app prompt.** Default: ask in the launcher window for destructive actions; MCP elicitation (MRTR) as well when the client supports it. |

Opt-in: the server is **off by default** and does nothing (no socket) until enabled in Settings.

---

## 1. Shape

```
 agent (Claude Code, Cursor, VS Code, Codex, ...)            Claude Desktop / stdio-only clients
   │ POST http://127.0.0.1:<port>/mcp                           │ spawns `<launcher> --mcp-stdio`
   │ Authorization: Bearer <token>                              │   (relays stdin/stdout JSON-RPC to the
   │                                                            │    running launcher's /mcp; starts the
   ▼                                                            ▼    launcher via LocalPeer if not running)
 ┌──────────────────────────── launcher process (GUI thread) ───────────────────────────────┐
 │ McpHttpServer (QHttpServer, 127.0.0.1 only)                                              │
 │   auth (token → scopes, policy) · Origin/Host check · headers · rate limit · audit log   │
 │ McpProtocol   JSON-RPC 2.0; 2026-07-28 (stateless, server/discover, MRTR)                │
 │               + 2025-11-25 legacy (initialize, no session minted)                        │
 │ McpToolHost   fixed tool list ─► op catalog (ops/*.cpp, one registry)                    │
 │ McpJobs       Task ─► job handle, progress stream, cancel                                │
 │ McpConfirm    in-app confirm bar + MRTR elicitation                                      │
 └──────┬───────────────────────────────────────────────────────────────────────────────────┘
        ▼  existing layer, never bypassed
   Application · InstanceList · MinecraftInstance/PackProfile · ResourceFolderModels ·
   ResourceAPI (Modrinth/Flame) · InstanceTask/creation tasks · LaunchController · AccountList ·
   JavaInstallList · Meta::Index · SettingsObject
```

Everything runs on the GUI thread, as the rest of the launcher does. Long work is an existing `Task`,
so HTTP handlers never block: a slow call returns a job handle or streams progress.

## 2. Transport and protocol

**HTTP server: our own, on `QTcpServer` (`launcher/mcp/McpHttp.*`).** Decided in phase 0 over QtHttpServer: MCP
needs only POST with a `Content-Length` (or chunked) body, keep-alive, and SSE responses that end by closing the
connection, and a new Qt module would have to be added for every packager (aqt modules, Nix, distro packages, the
container image). The parser is pure and unit-tested (`tests/McpHttp_test.cpp`); limits: 16 KiB headers, 1 MiB body,
32 connections, 60 s idle, loopback peers only.

**Protocol (verified against the spec repo 2026-11-07, `docs/specification/2026-07-28`):**

| Spec point | What we do |
|---|---|
| Stateless, no `initialize`, `server/discover` MUST exist | Implement `server/discover`; read version/capabilities from each request's `_meta`. |
| Legacy clients (2025-11-25 and earlier) | Also answer `initialize`/`notifications/initialized`. Never mint `Mcp-Session-Id`; GET/DELETE on `/mcp` → 405. Most clients in the wild are still legacy. |
| `MCP-Protocol-Version`, `Mcp-Method`, `Mcp-Name` headers | Validate; mismatch → `HeaderMismatchError` (-32020). Unsupported version → -32022 listing `supported`. |
| Origin MUST be validated; bind localhost | Bind `127.0.0.1` (and `::1`). Reject any present `Origin` that is not `null`/absent or an allow-listed loopback origin (403). Check `Host` is loopback (DNS rebinding). |
| Fixed list endpoints, deterministic order, `ttlMs`/`cacheScope` | `tools/list` never changes at runtime; sorted; `cacheScope: "private"`. |
| `resultType` on every result | `"complete"`, or `"input_required"` for MRTR. |
| MRTR replaces server→client requests | Confirmations return `input_required` with a form elicitation; the retry carries `inputResponses`; state in `requestState` (HMAC-signed, so a client can't forge "confirmed"). |
| Progress / cancellation | `notifications/progress` on the call's own SSE response; client disconnect or `notifications/cancelled` → `Task::abort()`. |
| `subscriptions/listen` | `resourcesListChanged` when instances are added/removed; resource subscriptions on `launcher://instances/{id}/state` (running, exited, crashed). |
| Tasks extension (`io.modelcontextprotocol/tasks`) | Advertised; long jobs return task handles. Clients without it use the `jobs` tool (same handles). |
| Logging, Sampling, Roots | Not used (deprecated). |
| Auth | Bearer tokens, not OAuth (§3). 401 with `WWW-Authenticate: Bearer` when missing/invalid. |

`instructions` (< 4,096 chars, Claude Code truncates there): what the launcher is, the id model
(instance ids, project ids per platform, job ids), "search then install", that the human may be using
the same window, confirmation behaviour, where the op catalog lives.

## 3. Enabling it, tokens, policy

New settings page **"AI agents (MCP)"** (`launcher/ui/pages/global/McpPage.*`):
- Enable toggle (setting `McpEnabled`, default false), port (`McpPort`, default 0 = pick free and
  remember; shown), status line ("Listening on 127.0.0.1:47211").
- **Tokens:** create/rename/revoke. Each has a name ("Claude Code"), scopes, a confirmation policy, created
  and last-used times. The secret is shown once at creation with copy buttons for ready-made configs:
  `claude mcp add --transport http launcher http://127.0.0.1:<port>/mcp --header "Authorization: Bearer <t>"`,
  a VS Code/Cursor `mcp.json` snippet, and the Claude Desktop stdio entry.
- Stored hashed (SHA-256) in `<dataRoot>/mcp/tokens.json`; never in the main `.cfg`, never logged.
- **Scopes:** `read`, `instances` (create/edit/delete instances, groups, components), `content`
  (mods/packs/shaders/datapacks/worlds), `launch`, `settings`, `accounts`, `files` (raw file access inside
  an instance dir). Missing scope → tool error naming the scope.
- **Policy per token:** `ask_destructive` (default) / `ask_all_writes` / `never_ask`.
- **Activity log:** every call (token name, tool, op, args summary, result, duration) in
  `<dataRoot>/logs/mcp.log` and a table on the page.
- **While an agent acts:** a status-bar strip in the main window ("Claude Code: installing 12 mods into
  'Create Above'") with **Stop** (aborts its jobs, rejects its calls until resumed).

`--mcp-stdio` (new `Commandline` option): no GUI; reads the port and a token from
`<dataRoot>/mcp/stdio.json` (a dedicated token created on first use and shown on the page); if the
launcher isn't running, starts it through the existing single-instance `LocalPeer` path with MCP forced on
for that run only if the user enabled it before. Pure relay, no protocol logic of its own.

## 4. Tools (fixed list)

| Tool | Does | Hints |
|---|---|---|
| `launcher_info` | Version, data dir, enabled platforms, default account (name only), Java installs, running instances, the caller's scopes and policy. | read-only |
| `instances` | List/filter instances: id, name, group, MC version, loader + version, managed pack (platform, pack id, version), running, last played, play time, mod count. | read-only |
| `instance_get` | One instance in depth: components, settings overrides, content summary per kind, worlds, servers, recent crash. | read-only |
| `search_content` | One search over Modrinth and CurseForge for mods, modpacks, resource packs, shaders, data packs: query, kind, MC version, loader, category, sort, page. Returns project ids per platform, merged when both list the same project. | read-only |
| `content_versions` | Versions of a project, filtered by MC version/loader, with dependencies and the chosen-by-default version. | read-only |
| `install_content` | Install one or many projects/versions into an instance, resolving dependencies (`GetModDependenciesTask`), with `dry_run` returning the full plan first. Job. | write |
| `create_instance` | From vanilla+loader, from a modpack (platform, pack id, version), or from an import URL/path. Takes every choice a dialog would ask (name, group, optional mods, icon). Job. | write |
| `launch` | Launch/stop an instance: account (or offline name), join server/world, demo flag. Never pops a dialog; missing choice → tool error listing options. | write |
| `logs` | Game log of a running/last run (tail, grep, level filter), other log files, crash reports, with an optional upload (confirm). | read-only (upload: write) |
| `settings` | Get/set launcher or instance settings by key, with type, default, current value and override state. Secrets redacted (§6). | write when setting |
| `jobs` | List, wait on, read progress of, or cancel jobs this token started. | read-only / cancel |
| `search_ops` | Search the op catalog: `detail: names \| signatures \| full`. | read-only |
| `call` | Run one catalog op by name with JSON args. | per op |
| `batch` | Run a list of catalog ops, validated whole first, stops at the first failure, `$N.field` refs to earlier results. Not transactional (filesystem/network ops can't roll back); the result says what ran. | per op |

All results: `structuredContent` + an `outputSchema`, and the same as text for clients that ignore it.
Errors are tool errors (`isError`) with a "did you mean" for ids, keys and op names.
`_meta["anthropic/alwaysLoad"]` on `instances`, `search_content`, `search_ops`.

## 5. Op catalog (the long tail behind `call` / `batch`)

One registry, `launcher/mcp/ops/*.cpp`; each op declares name, scope, destructive flag, JSON Schema for
args/result, docs, examples. The registry generates `search_ops` output and `launcher://api/ops.schema.json`,
and a unit test checks every op has a schema, docs and a scope.

| Area | Ops |
|---|---|
| instance | `rename`, `set_group`, `set_icon`, `set_notes`, `copy`, `delete` (trash), `undo_delete`, `export(format: zip\|mrpack\|curseforge, options)`, `create_shortcut`, `open_folder` (asks first), `repair` (re-resolve, redownload libs) |
| groups | `list`, `rename`, `delete`, `collapse` |
| components | `list`, `set_version(uid, version)` (MC, Forge, NeoForge, Fabric, Quilt, LiteLoader, LWJGL), `add_loader`, `remove`, `add_jar_mod(path)`, `available_versions(uid, filter)` via `Meta::Index` |
| content (per kind: mods, resourcepacks, texturepacks, shaderpacks, datapacks) | `list`, `enable`, `disable`, `remove`, `check_updates`, `update(ids\|all)`, `install_file(path)`, `change_version`, `metadata(id)` |
| worlds | `list`, `rename`, `copy`, `delete`, `backup`, `datapacks.*`, `reset_icon` |
| servers | `list`, `add`, `edit`, `remove`, `ping` |
| screenshots | `list`, `get` (image content), `delete` |
| java | `list_installs`, `detect`, `available(major)`, `download(vendor, major)`, `set_for_instance` |
| accounts | `list`, `set_default`, `add_offline`, `refresh`, `remove`, `login_msa` (returns the device code + URL; the human finishes it) |
| managed packs | `pack_info`, `pack_versions`, `update_pack(version)` |
| meta | `refresh_index`, `mc_versions(filter)` |
| window | `screenshot` (image of the launcher window, so an agent can check what the human sees), `show_instance` (select it in the UI) |
| files (`files` scope) | `list`, `read`, `write`, `delete` inside one instance's root only (`FS::getSafePathInRoot`); config editing (`config/*.toml`, `options.txt`) |

## 6. Safety rules

- **No secrets out, ever:** account tokens, `ProxyPass`, `FlameKeyOverride`, `ModrinthToken`,
  `MSAClientIDOverride`, `TechnicClientID`, MCP tokens. `settings` returns `"<redacted>"` and refuses writes
  to them unless the token has `settings` **and** the human confirms in-app.
- **Paths:** every path argument is resolved inside an instance root or the data dir; `..`, absolute paths
  elsewhere, and symlinks out are refused. Imports of local files outside those need confirmation.
- **No dialogs from agent calls.** Every flow that today calls `exec()` (launch account/offline-name pickers,
  blocked-mods dialog, optional-mods dialog, `ProgressDialog::execWithTask`, overwrite prompts) gets its
  choice from arguments. A new `InteractionMode::Unattended` flag on those tasks, default interactive, so
  GUI behaviour is unchanged.
- **Human first:** if a modal dialog is open (`QApplication::activeModalWidget()`), write calls wait up to
  N seconds then return `launcher_busy`. Content edits on a running instance → tool error unless `force`.
- **Destructive set:** delete instance/world/group/screenshot, remove content, remove account, overwrite
  files, change secrets, upload logs, launch with a different account. Governed by the token policy.
- **Concurrency:** a per-instance write lock; a second writer gets `instance_busy` with the job id holding it.
- Rate limit per token; request body cap (1 MB); response size cap with `resource_link` for big things
  (full logs, exported packs).

## 7. Resources and prompts

| URI | Content |
|---|---|
| `launcher://api/index.md` | The model: ids, scopes, jobs, confirmations, errors. |
| `launcher://api/ops.schema.json` | Every op's schema (generated). |
| `launcher://instances/{id}/summary` | Same as `instance_get`. |
| `launcher://instances/{id}/state` | Subscribable: idle/launching/running/exited/crashed. |
| `launcher://instances/{id}/logs/latest` | Last game log (`resource_link` from `logs`). |
| `launcher://instances/{id}/mods.json` | Full mod list with metadata. |
| `launcher://guide/{topic}` | `modpack-building` (loader choice, compat, perf mods), `crash-triage`, `java-versions`. |

Completions for `{id}` (instance ids/names), `{topic}`. Prompts: `build_modpack(theme, mc_version, loader)`,
`fix_crash(instance)`, `update_instance(instance)`, `optimize(instance)`.

## 8. Code layout

```
launcher/mcp/
  McpHttp.{h,cpp}          HTTP/1.1 parser, connection, SSE responder, loopback-only server
  McpService.{h,cpp}       binding, Host/Origin, auth, rate limit, routing, audit log, settings
  McpSchema.{h,cpp}        JSON Schema subset: validates tool arguments, builds schemas
  McpProtocol.{h,cpp}      JSON-RPC, version eras, discover/initialize, MRTR, errors
  McpTokens.{h,cpp}        token store, hashing, scopes, policy
  McpJobs.{h,cpp}          Task → job id, progress fan-out, cancel
  McpConfirm.{h,cpp}       in-app confirm bar + elicitation state (HMAC requestState)
  McpTool.{h,cpp}          Tool / ToolResult / ToolRegistry, "did you mean"
  McpLauncherTools.{h,cpp} the fixed tools
  ops/OpRegistry.{h,cpp}   catalog, schemas, search
  ops/{Instance,Component,Content,World,Server,Java,Account,Meta,Window,Files}Ops.cpp
  McpStdioRelay.cpp        --mcp-stdio
launcher/ui/pages/global/McpPage.{h,cpp,ui}
launcher/ui/widgets/McpActivityBar.{h,cpp}
tests/Mcp*_test.cpp
```

Ops call the same objects the UI pages call (`InstanceList`, `PackProfile`, `ResourceFolderModel`,
`ResourceAPI`, tasks). Logic that lives only in a page (for example ModFolderPage's update flow,
LogPage's upload) moves into a non-UI helper both use, so the page and the op can't drift.

## 9. Phases

| # | Delivers | Proves it |
|---|---|---|
| 0 | HTTP layer, `McpService` + `McpProtocol` (both eras), token store, settings page (enable/port/tokens/activity), `launcher_info` + `instances` | CI green on Linux/Windows/macOS; `curl` handshake tests; MCP Inspector lists tools; `claude mcp add --transport http` connects |
| 1 | `instance_get`, `settings`, `search_content`, `content_versions`, `jobs`, op registry + `search_ops`/`call`/`batch`, read-only ops | unit tests per op; protocol tests (headers, Origin, versions, 401/403) |
| 2 | Unattended mode on tasks; `install_content`, `create_instance`, content/component/instance write ops; confirmations (in-app + MRTR); activity bar + Stop | agent builds a Fabric 1.21.1 pack with 20 mods end to end; destructive op blocked until confirmed; Stop aborts a running install |
| 3 | `launch` (accounts, offline, join server), `logs`, state subscriptions, accounts ops, Java ops | agent launches, reads the crash, fixes a missing dependency, relaunches |
| 4 | Worlds, servers, screenshots, files scope, export, managed-pack updates, window screenshot, resources/prompts/guides, tasks extension | full catalog covered by tests |
| 5 | `--mcp-stdio` relay | Claude Desktop config works with the launcher closed (starts it) and open |

Docs when phase 0 lands: a user page in the fork's README ("AI agents (MCP)") and this file updated
with what was measured.

## 10. Risks / open points

- **Dialogs buried in tasks** (`ATLPackInstallTask`, `FTBPackInstallTask`, `FlameInstanceCreationTask`,
  `ModrinthInstanceCreationTask`, `ResourceFolderModel`, `Meta::Index`, `JavaCommon`, `InstanceTask` all
  include `QMessageBox`/`exec`). Each needs an unattended path; audit in phase 2 with a list.
- **Nested event loops:** a human opening a modal dialog while a request is in flight keeps serving HTTP
  inside `exec()`. Covered by the busy rule (§6), needs a test.
- **Other local software** can talk to 127.0.0.1. The bearer token is the gate; Origin checks stop
  browsers. Any local process that can read `<dataRoot>/mcp/` can already read the launcher's accounts, so
  that is not a new boundary.
- **Client support** for MRTR/tasks/subscriptions varies; everything degrades to plain calls, the `jobs`
  tool and in-app confirmation.
- Spec moved twice in 2026: re-check `docs/specification/` for a newer release before phase 0.

## 11. Progress

| Phase | State |
|---|---|
| 0 | Done 2026-11-07 (`32171ded1`): `launcher/mcp/*`, `ui/pages/global/McpPage.*`, settings `McpEnabled`/`McpPort`, tests `Mcp{Http,Schema,Tokens,Protocol,Service}_test`. CI green on Linux x64/arm64, Windows MSVC x64/arm64 and MinGW. Not yet tried with a real MCP client. |
| 1 | Written 2026-11-07: tools `instance_get`, `settings`, `search_content`, `content_versions`, `jobs`, `search_ops`, `call`, `batch`; ops `groups.list`, `components.list`, `components.available_versions`, `content.list`, `worlds.list`, `accounts.list`; `SettingsObject::settingIds()` (new public accessor, approved by the user 2026-11-07). Tests `Mcp{Ops,Jobs}_test`. |

Decisions made in phase 1:
- **Settings protection** (`McpSettingsTool.cpp`): secrets are redacted (`ProxyPass`, `ProxyUser`, API keys/tokens, client ids, and
  any key matching token/password/secret/apikey/clientid); `Mcp*` and `*Bookmark` keys are read-only; "sensitive" keys (commands,
  `JvmArgs`, `Env`, executable paths, download/upload/proxy URLs, data folders) are refused for agents, because changing one can run
  code or redirect downloads. Phase 2's confirmation flow may relax "sensitive" to "ask the user"; secrets stay refused.
- **batch** checks every op name and scope before running anything, validates each step's arguments after `$N` substitution,
  and reports `isError` when a step failed and `continueOnError` was not set.
- **jobs** are per token: a token sees and cancels only the jobs it started.
