#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <mutex>

// Config is the JSON document shared with the Go client under ~/.ergotime2.
struct Config {
  QString serverUrl;
  QString username;
  QString password;
  int networkTimeoutSec = 60;
  int idleTimeoutSec = 600;
  int activitySyncIntervalSec = 600;
  int reportSyncIntervalSec = 600;
  bool autoSync = false;
  QVector<qint64> recentCustomerIds;
  QByteArray windowGeometry;
  QByteArray splitterState;
  QString dataDir;
  QString database;
  QString configPath;

  int idleTimeoutMs() const {
    return (idleTimeoutSec > 0 ? idleTimeoutSec : 600) * 1000;
  }
  int networkTimeoutMs() const {
    return (networkTimeoutSec > 0 ? networkTimeoutSec : 60) * 1000;
  }
};

inline constexpr int kMaxRecentCustomers = 5;

class Settings {
public:
  Settings();
  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

  Config get() const;
  void update(const Config &cfg);
  void saveWindowLayout(const QByteArray &geometry, const QByteArray &splitter);
  void touchRecentCustomer(qint64 id);

private:
  void saveLocked();

  mutable std::mutex mu_;
  Config cfg_;
  QString path_;
};
