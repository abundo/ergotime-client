#pragma once

#include <QDateTime>
#include <QString>
#include <QTimeZone>

#include <optional>
#include <vector>

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
  bool isDefault = false;
};

// One project note on a customer report. Several lines share one interval.
struct ReportLine {
  qint64 projectId = -1;
  QString comment;
};

struct Report {
  qint64 id = 0;
  qint64 serverId = -1;
  qint64 userId = -1;
  qint64 customerId = -1;
  qint64 projectId = -1;
  QDateTime start;
  std::optional<QDateTime> stop;
  // Empty, not null: Qt binds a null QString as SQL NULL, which report.comment
  // rejects.
  QString comment = QStringLiteral("");
  std::vector<ReportLine> lines;
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
  }
  if (!dt.isValid()) {
    return {};
  }
  // Zone-less values are UTC, matching the database. Qt otherwise treats them
  // as local, which makes the same stored instant compare differently per
  // machine.
  const bool zoned = s.endsWith(QLatin1Char('Z')) ||
                     s.contains(QLatin1Char('+')) ||
                     s.lastIndexOf(QLatin1Char('-')) > 10;
  if (!zoned) {
    dt = QDateTime(dt.date(), dt.time(), QTimeZone::UTC);
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

inline QString projectDisplayName(const std::vector<Project> &projects,
                                  qint64 id) {
  for (const Project &p : projects) {
    if (p.id == id && !p.name.isEmpty()) {
      return p.name;
    }
  }
  return QStringLiteral("Project #%1").arg(id);
}

// Several project notes, labeled. One note stays as the comment text.
inline QString formatLinesComment(const std::vector<ReportLine> &lines,
                                  const std::vector<Project> &projects) {
  if (lines.empty()) {
    return QStringLiteral("");
  }
  if (lines.size() == 1) {
    return lines[0].comment.isNull() ? QStringLiteral("") : lines[0].comment;
  }
  QString out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      out += QStringLiteral("\n\n");
    }
    out += projectDisplayName(projects, lines[i].projectId);
    if (!lines[i].comment.trimmed().isEmpty()) {
      out += QLatin1Char('\n');
      out += lines[i].comment;
    }
  }
  return out;
}

// rows is every project shown in the comment table. anchorId is the
// preselected row. Empty comments are dropped when another row has text.
// When every comment is empty, only the preselected project is kept.
inline bool assignReportLines(Report &r, const std::vector<Project> &projects,
                              const std::vector<ReportLine> &rows,
                              qint64 anchorId) {
  std::vector<ReportLine> present;
  qint64 customerId = 0;
  bool anchorSeen = false;
  qint64 defaultId = 0;
  for (const ReportLine &row : rows) {
    const Project *found = nullptr;
    for (const Project &proj : projects) {
      if (proj.id == row.projectId) {
        found = &proj;
        break;
      }
    }
    if (!found) {
      continue;
    }
    if (customerId == 0) {
      customerId = found->customerId;
    } else if (found->customerId != customerId) {
      continue;
    }
    if (found->isDefault && defaultId == 0) {
      defaultId = found->id;
    }
    if (row.projectId == anchorId) {
      anchorSeen = true;
    }
    ReportLine kept;
    kept.projectId = row.projectId;
    kept.comment = row.comment.isNull() ? QString() : row.comment;
    present.push_back(kept);
  }
  if (present.empty()) {
    return false;
  }
  std::vector<ReportLine> kept;
  for (const ReportLine &row : present) {
    if (!row.comment.trimmed().isEmpty()) {
      kept.push_back(row);
    }
  }
  if (kept.empty()) {
    qint64 pick = present[0].projectId;
    if (anchorSeen) {
      pick = anchorId;
    } else if (defaultId > 0) {
      pick = defaultId;
    }
    for (const ReportLine &row : present) {
      if (row.projectId == pick) {
        ReportLine one;
        one.projectId = row.projectId;
        one.comment = QStringLiteral("");
        kept.push_back(one);
        break;
      }
    }
  }
  if (kept.empty()) {
    return false;
  }
  r.customerId = customerId;
  r.lines = kept;
  r.projectId = kept[0].projectId;
  if (kept.size() > 1 && anchorSeen) {
    for (const ReportLine &row : kept) {
      if (row.projectId == anchorId) {
        r.projectId = anchorId;
        break;
      }
    }
  }
  r.comment = formatLinesComment(kept, projects);
  if (r.comment.isNull()) {
    r.comment = QStringLiteral("");
  }
  return true;
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
