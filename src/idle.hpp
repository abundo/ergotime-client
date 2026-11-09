#pragma once

#include <QtGlobal>

#include <memory>

// Idle reports how long the session has been without input, in milliseconds.
// Linux tries Wayland ext-idle-notifier-v1, then GNOME Mutter IdleMonitor,
// then org.freedesktop.ScreenSaver. Windows uses GetLastInputInfo.
class Idle {
public:
  virtual ~Idle() = default;
  virtual qint64 idleMs() = 0;
  static std::unique_ptr<Idle> create();
};
