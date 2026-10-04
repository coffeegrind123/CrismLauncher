# Branding (Prism -> Crism)

Status: implemented (2026-10). Code: `scripts/rebrand/rebrand.py`, tests `scripts/rebrand/test_rebrand.py`
(`python3 -m unittest discover -s scripts/rebrand`, Pillow needed for the image tests). Wired into
`.github/workflows/ci.yml` (step "Rebrand as Crism Launcher", every build job, before dependency setup).

The repository keeps upstream's names, so the daily upstream merge stays conflict-free. CI renames its own fresh
checkout before configure; nothing renamed is ever committed. To see the result locally, run the script on a copy
of the tree (`--dry-run` lists what would change, `--list-protected` every span that keeps "prism").

## Steps, in order

```
recolor logos -> targeted edits -> rename contents -> rename paths -> append CMake block -> audit
```

1. Logos (`LOGO_FILES`): hues squeezed into a 300-20 degree band (magenta, crimson, orange-red), original hue order
   kept; greys untouched. SVG colour attributes rewritten; PNG/ICO/ICNS recoloured per frame with Pillow, sizes and
   alpha preserved. Stopgap until real Crism artwork exists.
2. Targeted edits (`targeted_edits`): exact-match replacements; a mismatch fails CI, so upstream rewording is caught.
   - Launcher_Git, bug tracker, updater repo, metainfo bugtracker/vcs-browser/contribute -> the fork
   - "Crism Launcher Contributors" added to the copyright and author strings
   - About -> credits heading stays "Prism Launcher Developers" (it lists upstream's developers)
   - Import page names both Crism and Prism exports
3. Contents: every `prism` (any case) -> `crism`, first letter's case kept: `Prism Launcher`, `PrismLauncher`,
   `prismlauncher`, `PRISMLAUNCHER`, `m_prismVersion`, `org.prismlauncher.*`, ...
4. Paths: the 59 files/dirs with prism in their path move (Java package tree, `program_info/*`, updater sources).
5. `launcher/CMakeLists.txt` gets the legacy-name install block (below).
6. Audit: build-relevant paths (`AUDIT_SCOPE`) must contain no `prism` outside protected spans, in content or path.

## Kept as prism (`PROTECT`), and why

| Span | Reason |
|------|--------|
| Lines with Copyright / SPDX-FileCopyrightText / Contributors | Upstream attribution (GPL-3.0 sections 4, 5) |
| `*.prismlauncher.org` (meta, i18n, files, website) | Live services with no Crism equivalent; meta is required to launch anything. Wiki/help/discord/matrix/reddit links stay on Prism's site. `Launcher_Domain` stays prismlauncher.org |
| `prismlauncher.cachix.org`, Weblate, OpenCollective | Upstream infrastructure |
| `github.com/PrismLauncher/...`, `github:PrismLauncher/...` | Upstream issue references, libnbtplusplus submodule, contributors page, artwork source |
| `prismarine*` | PrismarineJS, not the brand |
| `x-prismlauncher-*` | packwiz keys; files must stay readable by every Prism-family launcher |
| `.prism_launcher_updater_unpack.marker` | Update hand-off with pre-rename installs (below) |
| `scheme() == "prismlauncher"` | `prismlauncher://` import links from third-party sites keep working |

Not rewritten at all (`SKIP_CONTENT`): `.github/workflows/` (already loaded when the script runs), licence files,
`.gitmodules`, `flake.lock`, `tests/testdata/` (captured real-world input), `cmake/vcpkg-ports/` (changes the
vcpkg ABI hash and so the binary cache).

## Effects on users

- Data folder `CrismLauncher`, config `crismlauncher.cfg`, env prefix `CRISMLAUNCHER_`: a fresh start, separate
  from Prism's folder (by decision; no migration). Portable installs keep their folder (instances and
  `accounts.json` stay, settings reset because the config file is renamed).
- URL scheme registered: `crismlauncher://`; `prismlauncher://` links are still accepted when they arrive.
- 4 `tr()` source strings contained "Prism" literally; their translations no longer match and show in English.

## Updating installs from before the rename

```
old updater (prism)            new package                         install dir
  unpack archive -> tmp/  ---> tmp/prismlauncher_updater  (copy)
  write tmp/.prism_launcher_updater_unpack.marker            |
  exec tmp/prismlauncher_updater -d <data>  ---------------> new updater finds the marker (name kept)
                                                              copies the new manifest's files ------> crismlauncher(.exe),
                                                              exec crismlauncher                      prismlauncher(.exe) copy
```

- Portable Windows/Linux: works through the block appended to `launcher/CMakeLists.txt`, active when CI passes
  `-D Launcher_LEGACY_APP_BINARY_NAME=prismlauncher`. It copies the installed `crismlauncher` and
  `crismlauncher_updater` to the old names in the `portable` component only (after the main install, so the RPATH
  is the installed one). The old launcher name also keeps old shortcuts and the old `PrismLauncher` start script
  working.
- AppImage: the updater replaces `$APPIMAGE` in place; asset names don't matter.
- Windows installer: installs to `%LOCALAPPDATA%\Programs\CrismLauncher` with new registry keys; the old install
  stays until uninstalled (it can't be told apart from a real Prism install, so the installer doesn't remove it).
- macOS (Sparkle): the update can't install on a pre-rename build (Sparkle matches the archive's app by the old
  name or bundle id). The appcast marks the update informational below `CRISM_RENAME_VERSION` (12.0.32, the first
  renamed release; set in ci.yml), so those builds link to the release page instead.

Drop the legacy copies and the informational item once pre-rename installs are no longer a concern.
