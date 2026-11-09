#pragma once

#include <QDateTime>
#include <QString>
#include <QTimeZone>

#include <optional>

enum class TrackerState { Inactive, Active, Idle };

inline QString trackerStateName(TrackerState s) {
  switch (s) {
  case TrackerState::Inactive:
    return QStringLiteral("inactive");
  case TrackerState::Active:
    return QStringLiteral("active");
  case TrackerState::Idle:
    return QStringLiteral("idle");
  }
  return QStringLiteral("unknown");
}

struct Customer {
  qint64 id = 0;
  qint64 serverId = -1;
  QString name;
  QString description;
  bool active = false;
};

struct Project {
  qint64 id = 0;
  qint64 serverId = -1;
  qint64 customerId = -1;
  QString name;
  QString costCenter;
  std::optional<double> hourlyRate;
  std::optional<double> kmRate;
  bool active = false;
};

struct Report {
  qint64 id = 0;
  qint64 serverId = -1;
  qint64 userId = -1;
  qint64 projectId = -1;
  QDateTime start;
  std::optional<QDateTime> stop;
  // Empty, not null: Qt binds a null QString as SQL NULL, which report.comment
  // rejects.
  QString comment = QStringLiteral("");
  QDateTime modified;
  qint64 seq = -1;
  bool deleted = false;
  bool updated = false;
  QString clientUuid;
  bool conflict = false;

  qint64 durationMs(const QDateTime &now) const {
    const QDateTime end = stop.value_or(now);
    if (!start.isValid() || !end.isValid() || end < start) {
      return 0;
    }
    return start.msecsTo(end);
  }
};

struct Status {
  qint64 idleMs = 0;
  qint64 lengthMs = 0;
  std::optional<Project> project;
  std::optional<Report> report;
};

// Match the Go client's store.FormatTime: UTC RFC3339 with whole seconds.
inline QString formatTime(const QDateTime &t) {
  if (!t.isValid()) {
    return {};
  }
  return t.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

inline QDateTime parseTime(const QString &s) {
  if (s.isEmpty()) {
    return {};
  }
  QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
  if (!dt.isValid()) {
    dt = QDateTime::fromString(s, Qt::ISODate);
  }
  if (!dt.isValid()) {
    dt = QDateTime::fromString(s, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (dt.isValid()) {
      dt.setTimeZone(QTimeZone::LocalTime);
    }
  }
  return dt;
}

inline QString formatDurationHms(qint64 ms) {
  if (ms < 0) {
    ms = 0;
  }
  const qint64 total = (ms + 500) / 1000;
  const qint64 h = total / 3600;
  const qint64 m = (total % 3600) / 60;
  const qint64 s = total % 60;
  return QStringLiteral("%1:%2:%3")
      .arg(h, 2, 10, QChar('0'))
      .arg(m, 2, 10, QChar('0'))
      .arg(s, 2, 10, QChar('0'));
}

inline QString formatDurationHm(qint64 ms) {
  if (ms < 0) {
    ms = 0;
  }
  const qint64 totalMin = (ms + 30000) / 60000;
  const qint64 h = totalMin / 60;
  const qint64 m = totalMin % 60;
  return QStringLiteral("%1:%2")
      .arg(h, 2, 10, QChar('0'))
      .arg(m, 2, 10, QChar('0'));
}
