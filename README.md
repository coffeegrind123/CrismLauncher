<p align="center">
  <img src=".github/assets/crism-logo.svg" alt="Crism Launcher logo" width="128" height="128">
</p>

<h1 align="center">Crism Launcher</h1>

<p align="center">
  <a href="https://github.com/coffeegrind123/CrismLauncher/actions/workflows/ci.yml"><img src="https://github.com/coffeegrind123/CrismLauncher/actions/workflows/ci.yml/badge.svg" alt="Build Status"></a>
  <a href="https://github.com/coffeegrind123/CrismLauncher/releases/latest"><img src="https://img.shields.io/github/v/release/coffeegrind123/CrismLauncher?color=c2185b" alt="Latest Release"></a>
  <a href="https://github.com/coffeegrind123/CrismLauncher/releases/latest"><img src="https://img.shields.io/github/downloads/coffeegrind123/CrismLauncher/total?color=c2185b" alt="Downloads"></a>
</p>

<p align="center">
  A Minecraft launcher forked from <a href="https://github.com/PrismLauncher/PrismLauncher">Prism Launcher</a> that installs blocked mods automatically, plays without a Microsoft account, and logs in to third-party authentication servers. Rebased on upstream <code>develop</code> and synced daily.
</p>

## Changes from Upstream

**Mod downloads**
- CurseForge, FTB and ATLauncher modpacks install mods whose authors disabled third-party downloads, straight from CurseForge's file server
- OptiFine in ATLauncher packs downloads automatically from optifine.net, with the BMCLAPI mirror as a fallback
- Every such file is still checked against the checksum in the pack; the "blocked mods" dialog only remains for files hosted on forums or personal sites

**Accounts**
- Offline accounts work without owning Minecraft or adding a Microsoft account, and game files download without any account
- authlib-injector accounts: log in to Ely.by, LittleSkin, Blessing Skin, Drasl or any Yggdrasil-compatible server (Accounts → Add authlib-injector). The agent is downloaded, verified and injected automatically, and skins can be changed where the server allows it. Fjord Launcher account files load as-is

**Download mirror**
- Optional (Settings → Services): fetch Minecraft, libraries, assets, Java, Forge and NeoForge from BMCLAPI or a compatible mirror, falling back to the official servers on any failure. Mirrored files are verified against the official checksums

**Updates**
- The built-in updater (Sparkle on macOS) follows this fork's releases, not upstream Prism's

**Branding**
- Builds are named Crism Launcher throughout, and keep their data in a `CrismLauncher` folder of their own, separate from Prism Launcher's. Portable installs keep using their own folder
- Updating from a build that still said Prism: portable Windows and Linux installs update in place, and old shortcuts keep working. The Windows installer sets up Crism Launcher next to the old install, which can then be uninstalled. On macOS, download the new version once; updates are automatic again from there

### Latest Builds

| Platform | Download |
|----------|----------|
| **Windows x64** | [CrismLauncher-Windows-MinGW-w64-Portable-12.0.36.zip](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Windows-MinGW-w64-Portable-12.0.36.zip)<br/>[CrismLauncher-Windows-MinGW-w64-Setup-12.0.36.exe](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Windows-MinGW-w64-Setup-12.0.36.exe) |
| **Windows ARM64** | [CrismLauncher-Windows-MSVC-arm64-Portable-12.0.36.zip](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Windows-MSVC-arm64-Portable-12.0.36.zip)<br/>[CrismLauncher-Windows-MSVC-arm64-Setup-12.0.36.exe](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Windows-MSVC-arm64-Setup-12.0.36.exe) |
| **macOS** | [CrismLauncher-macOS-12.0.36.dmg](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-macOS-12.0.36.dmg)<br/>[CrismLauncher-macOS-12.0.36.zip](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-macOS-12.0.36.zip) |
| **Linux x86_64** | [CrismLauncher-Linux-Qt6-Portable-12.0.36.tar.gz](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Linux-Qt6-Portable-12.0.36.tar.gz)<br/>[CrismLauncher-Linux-x86_64.AppImage](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Linux-x86_64.AppImage) |
| **Linux ARM64** | [CrismLauncher-Linux-aarch64-Qt6-Portable-12.0.36.tar.gz](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Linux-aarch64-Qt6-Portable-12.0.36.tar.gz)<br/>[CrismLauncher-Linux-aarch64.AppImage](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.36/CrismLauncher-Linux-aarch64.AppImage) |

---

*Last updated: 2026-10-04 20:41 UTC*
