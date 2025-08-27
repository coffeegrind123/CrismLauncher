# Prism Launcher Fork with Dynamic API Keys

[![Build Status](https://github.com/coffeegrind123/CrismLauncher/actions/workflows/build.yml/badge.svg)](https://github.com/coffeegrind123/CrismLauncher/actions/workflows/build.yml)
[![Latest Release](https://img.shields.io/github/v/release/coffeegrind123/CrismLauncher)](https://github.com/coffeegrind123/CrismLauncher/releases/latest)

This fork automatically retrieves API keys from the upstream [PrismLauncher repository](https://github.com/PrismLauncher/PrismLauncher) during build, ensuring full compatibility with Microsoft login, CurseForge, and Imgur features.

## Download

**[Latest Release](https://github.com/coffeegrind123/CrismLauncher/releases/latest)** | **[All Releases](https://github.com/coffeegrind123/CrismLauncher/releases)**

### Available Builds

| Platform | Download |
|----------|----------|
| **Linux ARM64 (Qt6)** | [PrismLauncher-Linux-Qt6-arm64-869de06.tar.gz](https://github.com/coffeegrind123/CrismLauncher/releases/download/v20250827-869de06/PrismLauncher-Linux-Qt6-arm64-869de06.tar.gz)<br/>[PrismLauncher-Linux-aarch64.AppImage](https://github.com/coffeegrind123/CrismLauncher/releases/download/v20250827-869de06/PrismLauncher-Linux-aarch64.AppImage) |
| **Linux (Qt6)** | [PrismLauncher-Linux-Qt6-869de06.tar.gz](https://github.com/coffeegrind123/CrismLauncher/releases/download/v20250827-869de06/PrismLauncher-Linux-Qt6-869de06.tar.gz)<br/>[PrismLauncher-Linux-aarch64.AppImage](https://github.com/coffeegrind123/CrismLauncher/releases/download/v20250827-869de06/PrismLauncher-Linux-aarch64.AppImage) |
| **Windows MinGW ARM64** | [PrismLauncher-Windows-MinGW-arm64-Setup-869de06.exe](https://github.com/coffeegrind123/CrismLauncher/releases/download/v20250827-869de06/PrismLauncher-Windows-MinGW-arm64-Setup-869de06.exe) |
| **Windows MinGW x64** | [PrismLauncher-Windows-MinGW-w64-Setup-869de06.exe](https://github.com/coffeegrind123/CrismLauncher/releases/download/v20250827-869de06/PrismLauncher-Windows-MinGW-w64-Setup-869de06.exe) |
| **macOS** | [PrismLauncher-macOS-869de06.zip](https://github.com/coffeegrind123/CrismLauncher/releases/download/v20250827-869de06/PrismLauncher-macOS-869de06.zip) |

## How it Works

This fork automatically:
1. Downloads the latest CMakeLists.txt from [PrismLauncher/develop](https://github.com/PrismLauncher/PrismLauncher/blob/develop/CMakeLists.txt)
2. Replaces the local CMakeLists.txt with the upstream version (including all API keys)
3. Builds with full API integration
4. Creates releases with direct download links

## Installation

### Linux
- **Portable**: Extract tar.gz and run the executable
- **AppImage**: Make executable and run: `chmod +x *.AppImage && ./PrismLauncher*.AppImage`

### Windows
- **Portable**: Extract ZIP and run `prismlauncher.exe`
- **Setup**: Run the installer executable

### macOS
- **ZIP**: Extract and move to Applications folder
- **DMG**: Open and drag to Applications folder

## Features

All standard Prism Launcher features are included:
- Microsoft/Xbox account login
- CurseForge mod browsing and installation
- Imgur screenshot uploading
- Multiple instance management
- Mod management and installation

## API Keys

This build uses the official API keys from upstream Prism Launcher:
- **Microsoft MSA Client ID** - Xbox/Microsoft authentication
- **CurseForge API Key** - Mod browsing and downloads
- **Imgur Client ID** - Screenshot uploads
- **Sparkle Public Key** - macOS auto-updates

## Links

- [Upstream Prism Launcher](https://github.com/PrismLauncher/PrismLauncher)
- [Report Issues](https://github.com/coffeegrind123/CrismLauncher/issues)
- [Build Workflow](https://github.com/coffeegrind123/CrismLauncher/actions)

---

*Last updated: 2025-08-27 21:05 UTC*
