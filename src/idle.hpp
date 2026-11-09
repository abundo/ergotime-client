#pragma once

#include <QtGlobal>

#include <memory>

// Idle reports how long the session has been without input, in milliseconds.
// Linux uses GNOME Mutter IdleMonitor, then org.freedesktop.ScreenSaver.
// The Wayland ext-idle-notifier backend from the Go client is not ported.
class Idle {
public:
  virtual ~Idle() = default;
  virtual qint64 idleMs() = 0;
  static std::unique_ptr<Idle> create();
};
