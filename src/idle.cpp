#include "idle.hpp"

#include <QDebug>

#if defined(ERGOTIME_HAS_DBUS)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

class ZeroIdle : public Idle {
public:
  qint64 idleMs() override { return 0; }
};

#ifdef Q_OS_WIN
class WinIdle : public Idle {
public:
  qint64 idleMs() override {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) {
      return 0;
    }
    return static_cast<qint64>(GetTickCount() - info.dwTime);
  }
};
#endif

#if defined(ERGOTIME_HAS_DBUS)
class DBusIdle : public Idle {
public:
  DBusIdle(const QString &service, const QString &path, const QString &iface,
           const QString &method)
      : iface_(service, path, iface, QDBusConnection::sessionBus()),
        method_(method) {}

  qint64 idleMs() override {
    const QDBusMessage msg = iface_.call(method_);
    if (msg.type() != QDBusMessage::ReplyMessage || msg.arguments().isEmpty()) {
      return -1;
    }
    return msg.arguments().at(0).toLongLong();
  }

  bool probe() { return iface_.isValid() && idleMs() >= 0; }

private:
  QDBusInterface iface_;
  QString method_;
};

std::unique_ptr<Idle> tryDBus() {
  if (!QDBusConnection::sessionBus().isConnected()) {
    return nullptr;
  }
  auto mutter = std::make_unique<DBusIdle>(
      QStringLiteral("org.gnome.Mutter.IdleMonitor"),
      QStringLiteral("/org/gnome/Mutter/IdleMonitor/Core"),
      QStringLiteral("org.gnome.Mutter.IdleMonitor"),
      QStringLiteral("GetIdletime"));
  if (mutter->probe()) {
    qInfo() << "idle: GNOME Mutter IdleMonitor";
    return mutter;
  }
  auto fdo =
      std::make_unique<DBusIdle>(QStringLiteral("org.freedesktop.ScreenSaver"),
                                 QStringLiteral("/ScreenSaver"),
                                 QStringLiteral("org.freedesktop.ScreenSaver"),
                                 QStringLiteral("GetSessionIdleTime"));
  if (fdo->probe()) {
    qInfo() << "idle: org.freedesktop.ScreenSaver";
    return fdo;
  }
  return nullptr;
}
#endif

} // namespace

std::unique_ptr<Idle> Idle::create() {
#ifdef Q_OS_WIN
  return std::make_unique<WinIdle>();
#elif defined(ERGOTIME_HAS_DBUS)
  if (auto d = tryDBus()) {
    return d;
  }
  qWarning() << "idle: no session idle monitor; idle timeout will not fire";
  return std::make_unique<ZeroIdle>();
#else
  return std::make_unique<ZeroIdle>();
#endif
}
