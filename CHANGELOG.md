# Changelog

All notable changes to the official ErgoTime desktop client are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions are tagged `v*` on GitHub. That tag builds the static binaries and attaches them to a GitHub release.

## [Unreleased]

### Added

- Static release binaries are stripped and packed with UPX, so the download stays an executable. Linux, Windows, and macOS builds all go through `scripts/pack-upx.sh`.
- Sync refuses to pull or push unless `/healthz` advertises sync protocol 1. A missing number or any other number stops the sync before catalog or report changes, with a message naming the protocol this client speaks.
- Linux idle detection through Wayland `ext-idle-notifier-v1` (KWin, Sway, Hyprland, niri, labwc, river). The client tries this before GNOME Mutter `IdleMonitor` and `org.freedesktop.ScreenSaver`. Version 2 of the protocol uses input idle, so a video idle inhibitor does not keep a running report open.

## [1.0.0] - 2026-11-09

First release of the C++ client as the official desktop client. It replaces the Go client and the older Python/PyQt5 client. Local data stays in `~/.ergotime2` (`config.json` and `ergotime.db`), and the HTTP sync protocol is unchanged.

### Added

- Qt 6.12 Widgets interface: customer and project lists (five most recently used customers pinned), start and stop, a comment on the running report, hide-to-tray, and a tray menu that starts a project.
- Local reports for the selected day, with create, edit, and delete.
- SQLite through Qt Sql (`QSQLITE`), including the SQLite driver inside the static build.
- Catalog sync and report sync: batch push, per-report fallback, sequence pull, and a local-wins conflict flag.
- Settings dialog written back to the shared `config.json`.
- Idle detection on Linux through GNOME Mutter `IdleMonitor`, then `org.freedesktop.ScreenSaver`. Idle detection on Windows through `GetLastInputInfo`.
- Static release builds for Linux x86_64, Windows x86_64, and macOS Apple Silicon. Qt is linked in. The display stack stays shared (X11, OpenGL, fonts, D-Bus, and OpenSSL on Linux; Windows system DLLs such as `USER32` and `OPENGL32`; Cocoa and SecureTransport on macOS).
- GitHub Actions workflow that builds those three binaries on every push to `main` and on every `v*` tag. A tag also publishes a GitHub release.

### Changed

- The UI is C++ against Qt 6.12, instead of Go and miqt against Qt 6.8.
- Linux, Windows, and macOS use the same Qt release, 6.12.0. The Windows binary is cross-compiled with MinGW from the 6.12.0 qtbase sources. It no longer comes from the miqt MXE Qt 6.8 image.
- The local database goes through Qt Sql instead of `modernc.org/sqlite`.

### Removed

- The Wayland `ext-idle-notifier-v1` idle backend. On a desktop where neither D-Bus probe succeeds, idle time stays at zero and the idle timeout does not stop a running report.
- The optional X11 XScreenSaver idle backend.
- Android, iOS, and WebAssembly builds. macOS idle detection is not implemented in this release, so idle time stays at zero there.
