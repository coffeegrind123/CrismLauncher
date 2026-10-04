# Crism Launcher

[![Build Status](https://github.com/coffeegrind123/CrismLauncher/actions/workflows/ci.yml/badge.svg)](https://github.com/coffeegrind123/CrismLauncher/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/coffeegrind123/CrismLauncher)](https://github.com/coffeegrind123/CrismLauncher/releases/latest)

A Prism Launcher fork that installs blocked mods automatically, plays without a Microsoft account, and logs in to third-party authentication servers. Rebased on upstream `develop` and synced daily.

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

### Latest Builds

| Platform | Download |
|----------|----------|
| **Windows x64** | [PrismLauncher-Windows-MinGW-w64-Portable-12.0.30.zip](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Windows-MinGW-w64-Portable-12.0.30.zip)<br/>[PrismLauncher-Windows-MinGW-w64-Setup-12.0.30.exe](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Windows-MinGW-w64-Setup-12.0.30.exe) |
| **Windows ARM64** | [PrismLauncher-Windows-MSVC-arm64-Portable-12.0.30.zip](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Windows-MSVC-arm64-Portable-12.0.30.zip)<br/>[PrismLauncher-Windows-MSVC-arm64-Setup-12.0.30.exe](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Windows-MSVC-arm64-Setup-12.0.30.exe) |
| **macOS** | [PrismLauncher-macOS-12.0.30.dmg](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-macOS-12.0.30.dmg)<br/>[PrismLauncher-macOS-12.0.30.zip](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-macOS-12.0.30.zip) |
| **Linux x86_64** | [PrismLauncher-Linux-Qt6-Portable-12.0.30.tar.gz](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Linux-Qt6-Portable-12.0.30.tar.gz)<br/>[PrismLauncher-Linux-x86_64.AppImage](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Linux-x86_64.AppImage) |
| **Linux ARM64** | [PrismLauncher-Linux-aarch64-Qt6-Portable-12.0.30.tar.gz](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Linux-aarch64-Qt6-Portable-12.0.30.tar.gz)<br/>[PrismLauncher-Linux-aarch64.AppImage](https://github.com/coffeegrind123/CrismLauncher/releases/download/12.0.30/PrismLauncher-Linux-aarch64.AppImage) |

---

*Last updated: 2026-10-04 15:51 UTC*
