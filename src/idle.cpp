#include "idle.hpp"

#include <QDebug>
#include <QString>

#if defined(ERGOTIME_HAS_DBUS)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>
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

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
// ext-idle-notifier-v1 over the Wayland socket. Mutter does not implement
// this protocol; KWin, Sway, Hyprland, niri, labwc, and river do.
// Events are idled/resumed. Idle() rebuilds a duration from those timestamps.
constexpr uint32_t kWlDisplayId = 1;
constexpr uint16_t kWlDisplaySync = 0;
constexpr uint16_t kWlDisplayGetRegistry = 1;
constexpr uint16_t kWlDisplayError = 0;
constexpr uint16_t kWlDisplayDeleteId = 1;
constexpr uint16_t kWlRegistryBind = 0;
constexpr uint16_t kWlRegistryGlobal = 0;
constexpr uint16_t kWlCallbackDone = 0;
constexpr uint16_t kExtIdleGetIdleNotification = 1;
constexpr uint16_t kExtIdleGetInputIdleNotification = 2;
constexpr uint16_t kExtIdleIdled = 0;
constexpr uint16_t kExtIdleResumed = 1;
constexpr uint32_t kWaylandIdleTimeoutMs = 500;
constexpr int kWaylandSetupMs = 3000;

enum class RecvStatus { Ok, Timeout, Error };

struct WlMsg {
  uint32_t object = 0;
  uint16_t opcode = 0;
  std::vector<uint8_t> payload;
};

void appendU32(std::vector<uint8_t> &b, uint32_t v) {
  const size_t n = b.size();
  b.resize(n + 4);
  b[n] = static_cast<uint8_t>(v);
  b[n + 1] = static_cast<uint8_t>(v >> 8);
  b[n + 2] = static_cast<uint8_t>(v >> 16);
  b[n + 3] = static_cast<uint8_t>(v >> 24);
}

void appendStr(std::vector<uint8_t> &b, const char *s) {
  const uint32_t n = static_cast<uint32_t>(std::strlen(s) + 1);
  appendU32(b, n);
  b.insert(b.end(), s, s + n);
  while (b.size() % 4 != 0) {
    b.push_back(0);
  }
}

bool writeFull(int fd, const uint8_t *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

bool sendMsg(int fd, uint32_t objectId, uint16_t opcode,
             const std::vector<uint8_t> &body) {
  const uint32_t size = static_cast<uint32_t>(8 + body.size());
  uint8_t hdr[8];
  hdr[0] = static_cast<uint8_t>(objectId);
  hdr[1] = static_cast<uint8_t>(objectId >> 8);
  hdr[2] = static_cast<uint8_t>(objectId >> 16);
  hdr[3] = static_cast<uint8_t>(objectId >> 24);
  const uint32_t word = (size << 16) | opcode;
  hdr[4] = static_cast<uint8_t>(word);
  hdr[5] = static_cast<uint8_t>(word >> 8);
  hdr[6] = static_cast<uint8_t>(word >> 16);
  hdr[7] = static_cast<uint8_t>(word >> 24);
  if (!writeFull(fd, hdr, 8)) {
    return false;
  }
  return body.empty() || writeFull(fd, body.data(), body.size());
}

bool readExact(int fd, uint8_t *buf, size_t n, int timeoutMs, bool *timedOut) {
  size_t got = 0;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
  while (got < n) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) {
      if (got == 0) {
        *timedOut = true;
      }
      return false;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, static_cast<int>(left));
    if (pr == 0) {
      *timedOut = true;
      return false;
    }
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    const ssize_t r = ::read(fd, buf + got, n - got);
    if (r == 0) {
      errno = ECONNRESET;
      return false;
    }
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

RecvStatus recvMsg(int fd, WlMsg &msg, int timeoutMs) {
  uint8_t hdr[8];
  bool timedOut = false;
  if (!readExact(fd, hdr, 8, timeoutMs, &timedOut)) {
    return timedOut ? RecvStatus::Timeout : RecvStatus::Error;
  }
  const uint32_t object = static_cast<uint32_t>(hdr[0]) |
                          (static_cast<uint32_t>(hdr[1]) << 8) |
                          (static_cast<uint32_t>(hdr[2]) << 16) |
                          (static_cast<uint32_t>(hdr[3]) << 24);
  const uint32_t word = static_cast<uint32_t>(hdr[4]) |
                        (static_cast<uint32_t>(hdr[5]) << 8) |
                        (static_cast<uint32_t>(hdr[6]) << 16) |
                        (static_cast<uint32_t>(hdr[7]) << 24);
  const uint32_t size = word >> 16;
  if (size < 8 || size > 4096) {
    return RecvStatus::Error;
  }
  msg.object = object;
  msg.opcode = static_cast<uint16_t>(word & 0xffff);
  msg.payload.assign(size - 8, 0);
  if (!msg.payload.empty()) {
    // The header is already consumed. A short wait here would desync the
    // stream, so the body is read with the setup deadline and any failure
    // is fatal for this connection.
    bool ignored = false;
    if (!readExact(fd, msg.payload.data(), msg.payload.size(), kWaylandSetupMs,
                   &ignored)) {
      return RecvStatus::Error;
    }
  }
  return RecvStatus::Ok;
}

bool readU32(const std::vector<uint8_t> &b, size_t &off, uint32_t &out) {
  if (b.size() - off < 4) {
    return false;
  }
  out = static_cast<uint32_t>(b[off]) |
        (static_cast<uint32_t>(b[off + 1]) << 8) |
        (static_cast<uint32_t>(b[off + 2]) << 16) |
        (static_cast<uint32_t>(b[off + 3]) << 24);
  off += 4;
  return true;
}

bool readString(const std::vector<uint8_t> &b, size_t &off, std::string &out) {
  uint32_t n = 0;
  if (!readU32(b, off, n)) {
    return false;
  }
  if (n == 0) {
    out.clear();
    return true;
  }
  if (b.size() - off < n) {
    return false;
  }
  out.assign(reinterpret_cast<const char *>(b.data() + off), n - 1);
  const uint32_t padded = (n + 3) & ~uint32_t{3};
  if (b.size() - off < padded) {
    return false;
  }
  off += padded;
  return true;
}

QString displayError(const std::vector<uint8_t> &payload) {
  size_t off = 0;
  uint32_t obj = 0;
  uint32_t code = 0;
  std::string text;
  if (!readU32(payload, off, obj) || !readU32(payload, off, code) ||
      !readString(payload, off, text)) {
    return QStringLiteral("wayland display error");
  }
  return QStringLiteral("wayland error obj=%1 code=%2: %3")
      .arg(obj)
      .arg(code)
      .arg(QString::fromUtf8(text.c_str()));
}

int remainingMs(std::chrono::steady_clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
  return left > 0 ? static_cast<int>(left) : 0;
}

class WaylandIdle : public Idle {
public:
  explicit WaylandIdle(int fd) : fd_(fd) {}
  ~WaylandIdle() override {
    stop_.store(true);
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  WaylandIdle(const WaylandIdle &) = delete;
  WaylandIdle &operator=(const WaylandIdle &) = delete;

  bool setup(QString *error) {
    uint32_t nextId = 2;
    const auto alloc = [&nextId]() { return nextId++; };

    const uint32_t registryId = alloc();
    {
      std::vector<uint8_t> body;
      appendU32(body, registryId);
      if (!sendMsg(fd_, kWlDisplayId, kWlDisplayGetRegistry, body)) {
        *error = QStringLiteral("wayland send get_registry failed");
        return false;
      }
    }

    uint32_t seatName = 0;
    uint32_t seatVer = 0;
    uint32_t idleName = 0;
    uint32_t idleVer = 0;
    const uint32_t callbackId = alloc();
    {
      std::vector<uint8_t> body;
      appendU32(body, callbackId);
      if (!sendMsg(fd_, kWlDisplayId, kWlDisplaySync, body)) {
        *error = QStringLiteral("wayland send sync failed");
        return false;
      }
    }

    const auto setupDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(kWaylandSetupMs);
    bool syncDone = false;
    while (!syncDone) {
      const int left = remainingMs(setupDeadline);
      if (left <= 0) {
        *error = QStringLiteral("wayland: timeout waiting for registry");
        return false;
      }
      WlMsg msg;
      const RecvStatus st = recvMsg(fd_, msg, left);
      if (st != RecvStatus::Ok) {
        *error = st == RecvStatus::Timeout
                     ? QStringLiteral("wayland: timeout waiting for registry")
                     : QStringLiteral("wayland recv: %1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
      }
      if (msg.object == registryId && msg.opcode == kWlRegistryGlobal) {
        size_t off = 0;
        uint32_t name = 0;
        uint32_t ver = 0;
        std::string iface;
        if (!readU32(msg.payload, off, name) ||
            !readString(msg.payload, off, iface) ||
            !readU32(msg.payload, off, ver)) {
          *error = QStringLiteral("wayland: bad registry global");
          return false;
        }
        if (iface == "wl_seat") {
          seatName = name;
          seatVer = ver;
        } else if (iface == "ext_idle_notifier_v1") {
          idleName = name;
          idleVer = ver;
        }
      } else if (msg.object == callbackId && msg.opcode == kWlCallbackDone) {
        syncDone = true;
      } else if (msg.object == kWlDisplayId && msg.opcode == kWlDisplayError) {
        *error = displayError(msg.payload);
        return false;
      }
    }

    if (seatName == 0) {
      *error = QStringLiteral("wayland: no wl_seat global");
      return false;
    }
    if (idleName == 0) {
      *error =
          QStringLiteral("wayland: compositor has no ext_idle_notifier_v1");
      return false;
    }

    const uint32_t seatId = alloc();
    uint32_t bindVer = seatVer > 1 ? 1 : seatVer;
    if (bindVer == 0) {
      bindVer = 1;
    }
    {
      std::vector<uint8_t> body;
      appendU32(body, seatName);
      appendStr(body, "wl_seat");
      appendU32(body, bindVer);
      appendU32(body, seatId);
      if (!sendMsg(fd_, registryId, kWlRegistryBind, body)) {
        *error = QStringLiteral("wayland send bind seat failed");
        return false;
      }
    }

    const uint32_t idleId = alloc();
    uint32_t wantVer = idleVer > 2 ? 2 : idleVer;
    if (wantVer == 0) {
      wantVer = 1;
    }
    {
      std::vector<uint8_t> body;
      appendU32(body, idleName);
      appendStr(body, "ext_idle_notifier_v1");
      appendU32(body, wantVer);
      appendU32(body, idleId);
      if (!sendMsg(fd_, registryId, kWlRegistryBind, body)) {
        *error = QStringLiteral("wayland send bind idle notifier failed");
        return false;
      }
    }

    const uint32_t notifId = alloc();
    {
      std::vector<uint8_t> body;
      appendU32(body, notifId);
      appendU32(body, kWaylandIdleTimeoutMs);
      appendU32(body, seatId);
      const uint16_t op = wantVer >= 2 ? kExtIdleGetInputIdleNotification
                                       : kExtIdleGetIdleNotification;
      if (!sendMsg(fd_, idleId, op, body)) {
        *error = QStringLiteral("wayland send get_idle_notification failed");
        return false;
      }
    }
    notifId_ = notifId;

    const uint32_t callback2 = alloc();
    {
      std::vector<uint8_t> body;
      appendU32(body, callback2);
      if (!sendMsg(fd_, kWlDisplayId, kWlDisplaySync, body)) {
        *error = QStringLiteral("wayland send sync failed");
        return false;
      }
    }
    syncDone = false;
    const auto bindDeadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kWaylandSetupMs);
    while (!syncDone) {
      const int left = remainingMs(bindDeadline);
      if (left <= 0) {
        *error = QStringLiteral("wayland: timeout after idle bind");
        return false;
      }
      WlMsg msg;
      const RecvStatus st = recvMsg(fd_, msg, left);
      if (st != RecvStatus::Ok) {
        *error = st == RecvStatus::Timeout
                     ? QStringLiteral("wayland: timeout after idle bind")
                     : QStringLiteral("wayland recv: %1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
      }
      if (msg.object == callback2 && msg.opcode == kWlCallbackDone) {
        syncDone = true;
      } else if (msg.object == kWlDisplayId && msg.opcode == kWlDisplayError) {
        *error = displayError(msg.payload);
        return false;
      } else if (msg.object == notifId && msg.opcode == kExtIdleIdled) {
        noteIdled();
      } else if (msg.object == notifId && msg.opcode == kExtIdleResumed) {
        noteResumed();
      }
    }
    return true;
  }

  void start() {
    thread_ = std::thread([this] { loop(); });
  }

  qint64 idleMs() override {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_ || !idleSince_) {
      return 0;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - *idleSince_);
    return ms.count() > 0 ? static_cast<qint64>(ms.count()) : 0;
  }

private:
  void noteIdled() {
    std::lock_guard<std::mutex> lock(mu_);
    idleSince_ = std::chrono::steady_clock::now() -
                 std::chrono::milliseconds(kWaylandIdleTimeoutMs);
  }

  void noteResumed() {
    std::lock_guard<std::mutex> lock(mu_);
    idleSince_.reset();
  }

  void failClosed() {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    idleSince_.reset();
  }

  void loop() {
    while (!stop_.load()) {
      WlMsg msg;
      const RecvStatus st = recvMsg(fd_, msg, 500);
      if (stop_.load()) {
        return;
      }
      if (st == RecvStatus::Timeout) {
        continue;
      }
      if (st == RecvStatus::Error) {
        failClosed();
        return;
      }
      if (msg.object == notifId_ && msg.opcode == kExtIdleIdled) {
        noteIdled();
      } else if (msg.object == notifId_ && msg.opcode == kExtIdleResumed) {
        noteResumed();
      } else if (msg.object == kWlDisplayId && msg.opcode == kWlDisplayError) {
        qWarning() << "idle:" << displayError(msg.payload);
        failClosed();
        return;
      }
    }
  }

  int fd_ = -1;
  uint32_t notifId_ = 0;
  std::mutex mu_;
  bool closed_ = false;
  std::optional<std::chrono::steady_clock::time_point> idleSince_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

int dialWayland(QString *error) {
  const char *display = std::getenv("WAYLAND_DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    display = "wayland-0";
  }
  std::string path;
  if (display[0] == '/') {
    path = display;
  } else {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') {
      *error = QStringLiteral("XDG_RUNTIME_DIR not set");
      return -1;
    }
    path = std::string(runtime) + "/" + display;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = QStringLiteral("wayland socket: %1")
                 .arg(QString::fromLocal8Bit(std::strerror(errno)));
    return -1;
  }
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    *error = QStringLiteral("wayland socket path too long");
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    const int err = errno;
    ::close(fd);
    *error = QStringLiteral("wayland dial %1: %2")
                 .arg(QString::fromUtf8(path.c_str()))
                 .arg(QString::fromLocal8Bit(std::strerror(err)));
    return -1;
  }
  return fd;
}

std::unique_ptr<Idle> tryWayland() {
  const char *display = std::getenv("WAYLAND_DISPLAY");
  const char *inherited = std::getenv("WAYLAND_SOCKET");
  const bool displaySet = display != nullptr && display[0] != '\0';
  const bool inheritedSet = inherited != nullptr && inherited[0] != '\0';
  if (!displaySet && !inheritedSet) {
    return nullptr;
  }
  // WAYLAND_SOCKET is an inherited fd name. The Go client does not use it
  // and falls through to WAYLAND_DISPLAY (or wayland-0).
  QString error;
  const int fd = dialWayland(&error);
  if (fd < 0) {
    qInfo() << "idle: wayland:" << error;
    return nullptr;
  }
  auto det = std::make_unique<WaylandIdle>(fd);
  if (!det->setup(&error)) {
    qInfo() << "idle: wayland:" << error;
    return nullptr;
  }
  det->start();
  qInfo() << "idle: Wayland ext-idle-notifier-v1";
  return det;
}
#endif

} // namespace

std::unique_ptr<Idle> Idle::create() {
#ifdef Q_OS_WIN
  return std::make_unique<WinIdle>();
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
  if (auto wayland = tryWayland()) {
    return wayland;
  }
#if defined(ERGOTIME_HAS_DBUS)
  if (auto d = tryDBus()) {
    return d;
  }
#endif
  qWarning() << "idle: no session idle monitor; idle timeout will not fire";
  return std::make_unique<ZeroIdle>();
#else
  return std::make_unique<ZeroIdle>();
#endif
}
