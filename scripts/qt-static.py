#!/usr/bin/env python3
"""Link flags and plugin imports for a static Qt prefix.

Official Qt desktop kits ship shared libraries only. A static build of qtbase
installs .a archives plus .prl files that list the real link dependencies.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

# Plugins this client actually starts. Everything else (eglfs, vnc, ibus) pulls
# device trees this desktop binary does not use.
WANTED = (
    "plugins/platforms/libqxcb.a",
    "plugins/platforms/libqoffscreen.a",
    "plugins/platforms/libqminimal.a",
    "plugins/platforms/libqwindows.a",
    "plugins/xcbglintegrations/libqxcb-glx-integration.a",
    "plugins/xcbglintegrations/libqxcb-egl-integration.a",
    "plugins/sqldrivers/libqsqlite.a",
    "plugins/tls/libqcertonlybackend.a",
    "plugins/tls/libqopensslbackend.a",
    "plugins/tls/libqschannelbackend.a",
    "plugins/platforminputcontexts/libcomposeplatforminputcontextplugin.a",
    "plugins/platforms/libqwayland.a",
    "plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.a",
    "plugins/wayland-shell-integration/libxdg-shell.a",
    "plugins/wayland-shell-integration/libwl-shell-plugin.a",
    "plugins/wayland-decoration-client/libbradient.a",
)

MODULES = (
    "Qt6Widgets",
    "Qt6Gui",
    "Qt6Sql",
    "Qt6Network",
    "Qt6DBus",
    "Qt6Core",
    "Qt6WaylandClient",
)

INSTALL_VARS = (
    "QT_INSTALL_PREFIX",
    "QT_INSTALL_LIBS",
    "QT_INSTALL_LIBEXECS",
    "QT_INSTALL_BINS",
    "QT_INSTALL_PLUGINS",
    "QT_INSTALL_HEADERS",
    "QT_INSTALL_ARCHDATA",
    "QT_INSTALL_DATA",
    "QT_INSTALL_QML",
    "QT_INSTALL_TRANSLATIONS",
)


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def prefix_vars(prefix: Path) -> dict[str, str]:
    return {
        "QT_INSTALL_PREFIX": str(prefix),
        "QT_INSTALL_LIBS": str(prefix / "lib"),
        "QT_INSTALL_LIBEXECS": str(prefix / "libexec"),
        "QT_INSTALL_BINS": str(prefix / "bin"),
        "QT_INSTALL_PLUGINS": str(prefix / "plugins"),
        "QT_INSTALL_HEADERS": str(prefix / "include"),
        "QT_INSTALL_ARCHDATA": str(prefix),
        "QT_INSTALL_DATA": str(prefix),
        "QT_INSTALL_QML": str(prefix / "qml"),
        "QT_INSTALL_TRANSLATIONS": str(prefix / "translations"),
    }


def expand(token: str, variables: dict[str, str]) -> str:
    def repl(match: re.Match[str]) -> str:
        key = match.group(1)
        if key not in variables:
            die(f"unhandled Qt .prl variable $$[{key}]")
        return variables[key]

    return re.sub(r"\$\$\[([A-Z0-9_]+)\]", repl, token)


def prl_libs(path: Path, variables: dict[str, str]) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    libs = ""
    for line in text.splitlines():
        if line.startswith("QMAKE_PRL_LIBS = "):
            libs = line[len("QMAKE_PRL_LIBS = ") :]
            break
    if not libs:
        return []
    return [expand(tok, variables) for tok in libs.split() if tok]


def companion_prl(archive: Path) -> Path | None:
    """Return the .prl that describes this static archive.

    Linux installs libqoffscreen.prl beside libqoffscreen.a. The MinGW
    install keeps the lib prefix on the archive and drops it on the
    metadata, so the same plugin is qoffscreen.prl. Module archives do
    the same: libQt6Core.a is described by Qt6Core.prl.
    """
    candidates = [archive.with_suffix(".prl")]
    name = archive.name
    if name.startswith("lib") and name.endswith(".a"):
        candidates.append(archive.with_name(name[len("lib") : -len(".a")] + ".prl"))
    for path in candidates:
        if path.is_file():
            return path
    return None


def style_archives(prefix: Path) -> list[Path]:
    styles = prefix / "plugins" / "styles"
    if not styles.is_dir():
        return []
    return sorted(styles.glob("*.a"))


def selected_archives(prefix: Path) -> list[Path]:
    found = []
    for rel in WANTED:
        path = prefix / rel
        if path.is_file():
            found.append(path)
    found.extend(style_archives(prefix))
    if not found:
        die(f"no Qt plugins under {prefix}/plugins")
    rels = {str(p.relative_to(prefix)) for p in found}
    if "plugins/sqldrivers/libqsqlite.a" not in rels:
        die(
            f"{prefix} has no SQLite driver (plugins/sqldrivers/libqsqlite.a). "
            "Rebuild static Qt with FEATURE_sql_sqlite=ON"
        )
    if (
        "plugins/platforms/libqxcb.a" not in rels
        and "plugins/platforms/libqwindows.a" not in rels
    ):
        die(f"{prefix} has neither the XCB nor the Windows platform plugin")
    if (
        "plugins/platforms/libqxcb.a" in rels
        and "plugins/platforms/libqwayland.a" not in rels
    ):
        die(
            f"{prefix} has no Wayland platform plugin (plugins/platforms/libqwayland.a). "
            "Rebuild static Qt with FEATURE_wayland=ON"
        )
    return found


def collect(prefix: Path) -> list[str]:
    variables = prefix_vars(prefix)
    seeds: list[Path] = []
    for name in MODULES:
        for filename in (f"lib{name}.prl", f"{name}.prl"):
            prl = prefix / "lib" / filename
            if prl.is_file():
                seeds.append(prl)
                break
    for archive in selected_archives(prefix):
        prl = companion_prl(archive)
        if prl is None:
            die(f"missing {archive.with_suffix('.prl')}")
        seeds.append(prl)

    seen_prl: set[Path] = set()
    tokens: list[str] = []
    queue = list(seeds)
    while queue:
        prl = queue.pop(0)
        if prl in seen_prl or not prl.is_file():
            continue
        seen_prl.add(prl)
        for tok in prl_libs(prl, variables):
            tokens.append(tok)
            if tok.endswith(".a"):
                child = companion_prl(Path(tok))
                if child is not None:
                    queue.append(child)

    deduped: list[str] = []
    seen: set[str] = set()
    for tok in tokens:
        if tok in seen:
            continue
        seen.add(tok)
        deduped.append(tok)
    if not deduped:
        die(f"no link flags from .prl files under {prefix}")
    return deduped


def mocflags(prefix: Path) -> list[str]:
    return [flag for flag in cflags(prefix) if flag.startswith(("-I", "-D"))]


def cflags(prefix: Path) -> list[str]:
    inc = prefix / "include"
    win = prefix / "mkspecs" / "win32-g++"
    linux = prefix / "mkspecs" / "linux-g++"
    # A MinGW prefix ships every mkspec, including linux-g++. The Windows
    # platform plugin is what says this prefix is the Windows one.
    if (prefix / "plugins/platforms/libqwindows.a").is_file() and win.is_dir():
        specs = win
    elif linux.is_dir():
        specs = linux
    elif win.is_dir():
        specs = win
    else:
        specs = prefix / "mkspecs"
    flags = [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wno-sfinae-incomplete",
        "-fPIC",
        "-O2",
        f"-I{inc}",
        f"-I{specs}",
    ]
    mapping = {
        "QtCore": "QT_CORE_LIB",
        "QtGui": "QT_GUI_LIB",
        "QtWidgets": "QT_WIDGETS_LIB",
        "QtSql": "QT_SQL_LIB",
        "QtNetwork": "QT_NETWORK_LIB",
        "QtDBus": "QT_DBUS_LIB",
    }
    for module, define in mapping.items():
        if (inc / module).is_dir():
            flags.append(f"-I{inc / module}")
            flags.append(f"-D{define}")
    if (prefix / "lib" / "libQt6DBus.a").is_file():
        flags.append("-DERGOTIME_HAS_DBUS=1")
    return flags


def plugin_names(archive: Path) -> list[str]:
    nm = os.environ.get("NM", "nm")
    try:
        out = subprocess.check_output(
            [nm, "-C", "-g", "--defined-only", str(archive)],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        die(f"nm failed on {archive}")
    names = []
    for line in out.splitlines():
        match = re.search(r"\bqt_static_plugin_(\w+)\b", line)
        if match and match.group(1) not in names:
            names.append(match.group(1))
    if not names:
        die(f"no qt_static_plugin symbol in {archive}")
    return names


def write_plugins(prefix: Path, dest: Path) -> None:
    lines = [
        "// Generated by scripts/qt-static.py. Static Qt does not load plugins from disk.",
        "#include <QtPlugin>",
        "",
    ]
    for archive in selected_archives(prefix):
        for name in plugin_names(archive):
            lines.append(f"Q_IMPORT_PLUGIN({name})")
    lines.append("")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text("\n".join(lines), encoding="utf-8")


def ldflags(prefix: Path) -> list[str]:
    # Plugin .prl files list their dependencies, not the plugin archive itself.
    objects = []
    group = [str(path) for path in selected_archives(prefix)]
    seen = set(group)
    for tok in collect(prefix):
        if tok in seen:
            continue
        seen.add(tok)
        if tok.endswith(".o"):
            objects.append(tok)
        else:
            group.append(tok)
    return objects + ["-Wl,--start-group", *group, "-Wl,--end-group", "-pthread"]


def main(argv: list[str]) -> None:
    if len(argv) < 3 or argv[1] not in {"cflags", "mocflags", "ldflags", "plugins"}:
        die("usage: qt-static.py cflags|mocflags|ldflags|plugins PREFIX [plugins.cpp]")
    prefix = Path(argv[2]).resolve()
    if not (prefix / "lib" / "libQt6Core.a").is_file():
        die(f"static Qt not found at {prefix} (expected lib/libQt6Core.a)")
    cmd = argv[1]
    if cmd == "cflags":
        print(" ".join(cflags(prefix)))
    elif cmd == "mocflags":
        print(" ".join(mocflags(prefix)))
    elif cmd == "ldflags":
        print(" ".join(ldflags(prefix)))
    else:
        if len(argv) != 4:
            die("usage: qt-static.py plugins PREFIX OUTPUT.cpp")
        write_plugins(prefix, Path(argv[3]))


if __name__ == "__main__":
    main(sys.argv)
