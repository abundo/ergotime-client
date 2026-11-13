## Design before features

A feature must fit the architecture: in the layer that owns it, through the
existing seams (domain, store, sync, tracker, idle, dialogs, the main window),
keeping the invariants below. If it can't be added that way without a workaround
(a special case in a layer that shouldn't know about it, duplicated logic, state
kept beside the store), don't build the workaround. Stop, before writing code or
as soon as it shows up, and tell the maintainer: what doesn't fit and why, the
refactor you'd recommend and its rough size, and any smaller part of the feature
that fits cleanly now. Go on only once they've chosen.

## Layers

`main` wires settings, the store, the sync engine, the tracker, and the window.
Each layer talks only to the one under it.

- `domain` is the shared types: customer, project, report, report line, times.
- `store` is the only SQLite access. Schema matches the Go client so a release
  build can share `~/.ergotime2/ergotime.db`. A version string with a git SHA-1
  uses `~/.ergotime2-dev`.
- `sync` (`Engine`) is the only HTTP. It pushes pending ops, pulls by `seq`, and
  pulls the catalog.
- `tracker` starts and stops the running report and applies the idle timeout.
  `idle` is the platform probe and returns milliseconds, nothing else.
- `dialogs` and `mainwindow` draw Qt widgets and call the store, engine, and
  tracker. They do not open SQL or HTTP themselves.

## Invariants

- Sync speaks protocol 2 only. A server that does not advertise `protocol` 2 on
  `/healthz` is refused.
- A report is one customer and one time interval. Project notes are lines on
  that interval. Several lines share the interval. Do not split the interval
  across projects.
- Times on the wire are UTC RFC3339 with whole seconds.
- A dirty local report wins a conflict. The row is flagged, not overwritten.
- Catalog sync deletes customers and projects the server no longer returns.
  A report line on a removed project is folded onto a project that remains.
- `comment` is an empty string when there is no text. SQLite rejects NULL.
- Idle on Linux tries Wayland `ext-idle-notifier-v1`, then Mutter
  `IdleMonitor`, then `org.freedesktop.ScreenSaver`. Windows uses
  `GetLastInputInfo`. Other platforms stay at zero. A missing probe does not
  invent idle time.
- Qt is 6.12 on every platform. Do not pin a different minor for one target.
- The tree stays GPLv3.
