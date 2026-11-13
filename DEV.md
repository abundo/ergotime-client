# Development

Official desktop client. A tagged release keeps data in `~/.ergotime2`
(`config.json` and `ergotime.db`), shared with the old Go client. Any other
build puts the git SHA-1 in the version and uses `~/.ergotime2-dev`, so a
development run does not open the release database. Release notes are in
`CHANGELOG.md`. User-facing build steps are in `README.md`.

# Prerequisites

- A C++17 compiler.
- Qt 6.12 with Widgets, Sql, and Network. Linux also uses DBus when
  `Qt6DBus` is installed. Every platform uses this same 6.12 release.
- CMake 3.16 or newer, or `make` with `pkg-config` when CMake is not installed.
- Python 3, used by `scripts/qt-static.py`.
- UPX (`upx`) if you pack the static binary. The build still succeeds without it.

Install the Linux desktop kit (the `icu` archive is required):

```bash
aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O "$HOME/Qt" --archives qtbase icu
```

A static binary needs a qtbase built with SQLite bundled. On this machine that
build runs in Podman or Docker, which supplies CMake and the X11 headers:

```bash
make static-container
```

The Windows `.exe` is a MinGW cross-compile from the same kind of container
(`make static-windows-container`). macOS is built on a Mac. See `README.md`.

# Build and run

Shared Qt, when CMake is installed:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/gcc_64"
cmake --build build
./build/ergotime
```

Without CMake, `make` writes `build/ergotime` against
`$HOME/Qt/6.12.0/gcc_64`. Override `QT_PREFIX` and `CXX`.

```bash
make
./build/ergotime
```

`make static` links `$HOME/Qt/static/6.12.0` into `build/ergotime-static`.
`ldd` on that binary should show system libraries only, not `libQt6` or ICU.

There is no unit-test target. Check a change by building and running. A
development build uses `~/.ergotime2-dev`. An offscreen smoke run is
`QT_QPA_PLATFORM=offscreen ./build/ergotime`.
