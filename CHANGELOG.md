# Changelog

All notable changes to the official ErgoTime desktop client are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions are tagged `v*` on GitHub. That tag builds the static binaries and attaches them to a GitHub release.

## [Unreleased]

### Fixed

- A batch create is marked synced only when its client_uuid matches. An empty id no longer claims another report's server row.
- A date stored without a timezone is read as UTC, the same as values written with a Z suffix.
- Catalog sync removes customers and projects the server no longer returns. A report line on a removed project keeps its text on a project that remains, so the time entry can still be pushed.

### Changed

- A report is a customer and a time range. The comment area is a table of that customer's projects. The default project is preselected. Comments on several projects share the one interval. Comments on a single project keep the whole interval on that project.
- Sync protocol is 2. Protocol 1 servers are refused, because a report can now carry several project comments.

### Added

- The window title, tray tooltip, About dialog, and settings show the version. A clean build of the matching `vX.Y.Z` tag shows `X.Y.Z` and keeps using `~/.ergotime2`. Any other build appends `+` and the git SHA-1, and that suffix stores settings and the database in `~/.ergotime2-dev`.
- The time-tracking panel has a Started date and time. It follows the clock until edited. Start uses that time, and the field stays editable while the entry is running. A time in the future is refused.
- A divider between the customer side and the comment can be dragged. Its position is saved when the program exits.
- Static release binaries are stripped and packed with UPX, so the download stays an executable. Linux, Windows, and macOS builds all go through `scripts/pack-upx.sh`.
- Sync refuses to pull or push unless `/healthz` advertises sync protocol 2. A missing number or any other number stops the sync before catalog or report changes, with a message naming the protocol this client speaks.
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
