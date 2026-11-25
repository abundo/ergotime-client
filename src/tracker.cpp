#include "tracker.hpp"

#include <QDateTime>

#include <exception>

namespace {

QDateTime wholeSecond(QDateTime t) {
  if (!t.isValid()) {
    return {};
  }
  const QTime time = t.time();
  t.setTime(QTime(time.hour(), time.minute(), time.second()));
  return t;
}

bool sameNotes(const Report &a, const Report &b) {
  if (a.customerId != b.customerId || a.projectId != b.projectId ||
      a.comment != b.comment || a.lines.size() != b.lines.size()) {
    return false;
  }
  for (size_t i = 0; i < a.lines.size(); ++i) {
    if (a.lines[i].projectId != b.lines[i].projectId ||
        a.lines[i].comment != b.lines[i].comment) {
      return false;
    }
  }
  return true;
}

} // namespace

Tracker::Tracker(Store &store, int idleTimeoutMs, QObject *parent)
    : QObject(parent), store_(store), idle_(Idle::create()),
      idleTimeoutMs_(idleTimeoutMs > 0 ? idleTimeoutMs : 600000) {
  timer_.setInterval(1000);
  connect(&timer_, &QTimer::timeout, this, &Tracker::tick);
}

void Tracker::setComment(const QString &comment) {
  if (report_) {
    report_->comment = comment;
    if (report_->lines.size() == 1) {
      report_->lines[0].comment = comment;
    }
  }
}

void Tracker::setDraft(const Report &draft) {
  if (!report_) {
    return;
  }
  report_->customerId = draft.customerId;
  report_->projectId = draft.projectId;
  report_->comment =
      draft.comment.isNull() ? QStringLiteral("") : draft.comment;
  report_->lines = draft.lines;
  if (auto p = store_.getProject(draft.projectId)) {
    project_ = *p;
  }
}

void Tracker::setIdleTimeoutMs(int ms) {
  if (ms > 0) {
    idleTimeoutMs_ = ms;
  }
}

void Tracker::start(const Report &draft, const QDateTime &startedAt) {
  if (state_ == TrackerState::Active) {
    return;
  }
  if (draft.projectId <= 0) {
    return;
  }
  const QDateTime now = wholeSecond(QDateTime::currentDateTime());
  QDateTime start =
      startedAt.isValid() ? floorMinute(startedAt) : floorMinute(now);
  if (!start.isValid() || start > now) {
    start = floorMinute(now);
  }
  Report r = draft;
  r.id = 0;
  r.serverId = -1;
  r.seq = 0;
  r.updated = true;
  r.deleted = false;
  r.conflict = false;
  r.clientUuid.clear();
  r.start = start;
  r.stop.reset();
  r.modified = r.start;
  if (r.comment.isNull()) {
    r.comment = QStringLiteral("");
  }
  if (r.lines.empty()) {
    r.lines.push_back(ReportLine{r.projectId, r.comment});
  }
  if (auto p = store_.getProject(r.projectId)) {
    project_ = *p;
  } else {
    project_.reset();
  }
  report_ = r;
  saved_.reset();
  setState(TrackerState::Active);
  timer_.start();
}

void Tracker::setStart(const QDateTime &startedAt) {
  if (state_ != TrackerState::Active || !report_) {
    return;
  }
  const QDateTime now = wholeSecond(QDateTime::currentDateTime());
  const QDateTime start = floorMinute(startedAt);
  if (!start.isValid() || start > now) {
    return;
  }
  report_->start = start;
  Status st;
  st.idleMs = idle_ ? idle_->idleMs() : 0;
  st.lengthMs = report_->durationMs(now);
  st.project = project_;
  st.report = report_;
  emit statusChanged(st);
}

void Tracker::abandon() {
  timer_.stop();
  report_.reset();
  saved_.reset();
  project_.reset();
  if (state_ != TrackerState::Inactive) {
    setState(TrackerState::Inactive);
  }
}

void Tracker::stop() {
  if (state_ != TrackerState::Active || !report_) {
    timer_.stop();
    return;
  }
  timer_.stop();
  report_->stop = wholeSecond(QDateTime::currentDateTime());
  persist(true);
  report_.reset();
  saved_.reset();
  project_.reset();
  setState(TrackerState::Inactive);
}

void Tracker::persist(bool closing) {
  if (!report_) {
    return;
  }
  Report snapshot = *report_;
  if (!closing) {
    snapshot.stop = wholeSecond(QDateTime::currentDateTime());
  }
  alignStoredReport(snapshot);
  const QDateTime now = wholeSecond(QDateTime::currentDateTime());
  if (!closing) {
    // Wait until that minute has actually been reached. A stop in the future
    // would claim time that has not happened yet.
    if (!snapshot.stop || now < *snapshot.stop) {
      return;
    }
    // This minute is already on disk. Comments typed since then are written
    // on the next minute, or when tracking ends.
    if (saved_ && snapshot.start == saved_->start &&
        snapshot.stop == saved_->stop) {
      return;
    }
  } else if (saved_ && snapshot.start == saved_->start &&
             snapshot.stop == saved_->stop && sameNotes(snapshot, *saved_)) {
    return;
  }
  if (closing) {
    report_->start = snapshot.start;
    report_->stop = snapshot.stop;
  }
  snapshot.modified = now;
  try {
    store_.saveReport(snapshot);
  } catch (const std::exception &ex) {
    emit errorOccurred(QString::fromUtf8(ex.what()));
    return;
  }
  report_->id = snapshot.id;
  report_->serverId = snapshot.serverId;
  report_->seq = snapshot.seq;
  report_->userId = snapshot.userId;
  report_->clientUuid = snapshot.clientUuid;
  saved_ = snapshot;
  emit reportSaved(snapshot);
}

void Tracker::tick() {
  if (state_ != TrackerState::Active || !report_) {
    return;
  }
  const QDateTime now = wholeSecond(QDateTime::currentDateTime());
  report_->stop = now;
  const qint64 idleFor = idle_ ? idle_->idleMs() : 0;
  if (idleFor > idleTimeoutMs_) {
    timer_.stop();
    report_->stop = now.addMSecs(-idleTimeoutMs_);
    persist(true);
    report_.reset();
    saved_.reset();
    project_.reset();
    setState(TrackerState::Inactive);
    return;
  }
  persist(false);
  Status st;
  st.idleMs = idleFor;
  st.lengthMs = report_->durationMs(now);
  st.project = project_;
  st.report = report_;
  emit statusChanged(st);
}

void Tracker::setState(TrackerState s) {
  state_ = s;
  emit stateChanged(s);
}
