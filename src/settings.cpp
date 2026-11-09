#include "settings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <stdexcept>

namespace {

Config defaults() {
  QString home = QDir::homePath();
  if (home.isEmpty()) {
    home = QStringLiteral(".");
  }
  Config c;
  c.dataDir = home + QStringLiteral("/.ergotime2");
  c.database = c.dataDir + QStringLiteral("/ergotime.db");
  c.configPath = c.dataDir + QStringLiteral("/config.json");
  c.serverUrl = QStringLiteral("http://localhost:8080");
  const QString user = qEnvironmentVariable("USER");
  c.username = user.isEmpty() ? qEnvironmentVariable("USERNAME") : user;
  return c;
}

QVector<qint64> readIds(const QJsonArray &arr) {
  QVector<qint64> out;
  out.reserve(arr.size());
  for (const auto &v : arr) {
    out.push_back(static_cast<qint64>(v.toDouble()));
  }
  return out;
}

} // namespace

Settings::Settings() {
  Config cfg = defaults();
  if (!QDir().mkpath(cfg.dataDir)) {
    throw std::runtime_error("create data dir failed");
  }
  cfg_ = cfg;
  path_ = cfg.configPath;

  QFile f(cfg.configPath);
  if (!f.exists()) {
    saveLocked();
    return;
  }
  if (!f.open(QIODevice::ReadOnly)) {
    throw std::runtime_error("read config failed");
  }
  const auto doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject()) {
    throw std::runtime_error("parse config failed");
  }
  const QJsonObject o = doc.object();
  Config loaded = cfg;
  if (o.contains(QStringLiteral("server_url"))) {
    loaded.serverUrl = o.value(QStringLiteral("server_url")).toString();
  }
  loaded.username = o.value(QStringLiteral("username")).toString(cfg.username);
  loaded.password = o.value(QStringLiteral("password")).toString();
  loaded.networkTimeoutSec = o.value(QStringLiteral("network_timeout_sec"))
                                 .toInt(cfg.networkTimeoutSec);
  loaded.idleTimeoutSec =
      o.value(QStringLiteral("idle_timeout_sec")).toInt(cfg.idleTimeoutSec);
  loaded.activitySyncIntervalSec =
      o.value(QStringLiteral("activity_sync_interval_sec")).toInt(0);
  loaded.reportSyncIntervalSec =
      o.value(QStringLiteral("report_sync_interval_sec")).toInt(0);
  loaded.autoSync = o.value(QStringLiteral("auto_sync")).toBool(false);
  loaded.recentCustomerIds =
      readIds(o.value(QStringLiteral("recent_customer_ids")).toArray());
  loaded.windowGeometry = QByteArray::fromBase64(
      o.value(QStringLiteral("window_geometry")).toString().toLatin1());
  loaded.splitterState = QByteArray::fromBase64(
      o.value(QStringLiteral("splitter_state")).toString().toLatin1());
  loaded.dataDir = o.value(QStringLiteral("data_dir")).toString();
  loaded.database = o.value(QStringLiteral("database")).toString();
  if (loaded.dataDir.isEmpty()) {
    loaded.dataDir = cfg.dataDir;
  }
  if (loaded.database.isEmpty()) {
    loaded.database = loaded.dataDir + QStringLiteral("/ergotime.db");
  }
  loaded.configPath = cfg.configPath;
  if (loaded.serverUrl.isEmpty()) {
    loaded.serverUrl = cfg.serverUrl;
  }
  if (loaded.idleTimeoutSec == 0) {
    loaded.idleTimeoutSec = cfg.idleTimeoutSec;
  }
  if (loaded.networkTimeoutSec == 0) {
    loaded.networkTimeoutSec = cfg.networkTimeoutSec;
  }
  cfg_ = loaded;
  path_ = loaded.configPath;
}

Config Settings::get() const {
  std::lock_guard lock(mu_);
  return cfg_;
}

void Settings::update(const Config &cfg) {
  std::lock_guard lock(mu_);
  Config next = cfg;
  if (next.dataDir.isEmpty()) {
    next.dataDir = cfg_.dataDir;
  }
  if (next.database.isEmpty()) {
    next.database = next.dataDir + QStringLiteral("/ergotime.db");
  }
  next.configPath = path_;
  cfg_ = next;
  saveLocked();
}

void Settings::saveWindowLayout(const QByteArray &geometry,
                                const QByteArray &splitter) {
  std::lock_guard lock(mu_);
  cfg_.windowGeometry = geometry;
  cfg_.splitterState = splitter;
  saveLocked();
}

void Settings::touchRecentCustomer(qint64 id) {
  if (id <= 0) {
    return;
  }
  std::lock_guard lock(mu_);
  QVector<qint64> out;
  out.reserve(kMaxRecentCustomers);
  out.push_back(id);
  for (qint64 existing : cfg_.recentCustomerIds) {
    if (existing == id) {
      continue;
    }
    out.push_back(existing);
    if (out.size() >= kMaxRecentCustomers) {
      break;
    }
  }
  cfg_.recentCustomerIds = out;
  saveLocked();
}

void Settings::saveLocked() {
  QDir().mkpath(QFileInfo(path_).absolutePath());
  QJsonObject o;
  o.insert(QStringLiteral("server_url"), cfg_.serverUrl);
  o.insert(QStringLiteral("username"), cfg_.username);
  o.insert(QStringLiteral("password"), cfg_.password);
  o.insert(QStringLiteral("network_timeout_sec"), cfg_.networkTimeoutSec);
  o.insert(QStringLiteral("idle_timeout_sec"), cfg_.idleTimeoutSec);
  o.insert(QStringLiteral("activity_sync_interval_sec"),
           cfg_.activitySyncIntervalSec);
  o.insert(QStringLiteral("report_sync_interval_sec"),
           cfg_.reportSyncIntervalSec);
  o.insert(QStringLiteral("auto_sync"), cfg_.autoSync);
  if (!cfg_.recentCustomerIds.isEmpty()) {
    QJsonArray ids;
    for (qint64 id : cfg_.recentCustomerIds) {
      ids.append(static_cast<double>(id));
    }
    o.insert(QStringLiteral("recent_customer_ids"), ids);
  }
  if (!cfg_.windowGeometry.isEmpty()) {
    o.insert(QStringLiteral("window_geometry"),
             QString::fromLatin1(cfg_.windowGeometry.toBase64()));
  }
  if (!cfg_.splitterState.isEmpty()) {
    o.insert(QStringLiteral("splitter_state"),
             QString::fromLatin1(cfg_.splitterState.toBase64()));
  }
  if (!cfg_.dataDir.isEmpty()) {
    o.insert(QStringLiteral("data_dir"), cfg_.dataDir);
  }
  if (!cfg_.database.isEmpty()) {
    o.insert(QStringLiteral("database"), cfg_.database);
  }
  QFile f(path_);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    throw std::runtime_error("write config failed");
  }
  f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
  f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
