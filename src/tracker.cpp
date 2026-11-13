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
  QDateTime start = startedAt.isValid() ? wholeSecond(startedAt) : now;
  if (!start.isValid() || start > now) {
    start = now;
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
  setState(TrackerState::Active);
  timer_.start();
}

void Tracker::setStart(const QDateTime &startedAt) {
  if (state_ != TrackerState::Active || !report_) {
    return;
  }
  const QDateTime now = wholeSecond(QDateTime::currentDateTime());
  const QDateTime start = wholeSecond(startedAt);
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

void Tracker::stop() {
  if (state_ != TrackerState::Active || !report_) {
    timer_.stop();
    return;
  }
  timer_.stop();
  QDateTime now = QDateTime::currentDateTime();
  now.setTime(
      QTime(now.time().hour(), now.time().minute(), now.time().second()));
  if (now < report_->start) {
    now = report_->start;
  }
  report_->stop = now;
  report_->modified = now;
  try {
    store_.saveReport(*report_);
    emit reportSaved(*report_);
  } catch (const std::exception &ex) {
    emit errorOccurred(QString::fromUtf8(ex.what()));
  }
  report_.reset();
  project_.reset();
  setState(TrackerState::Inactive);
}

void Tracker::tick() {
  if (state_ != TrackerState::Active || !report_) {
    return;
  }
  QDateTime now = QDateTime::currentDateTime();
  now.setTime(
      QTime(now.time().hour(), now.time().minute(), now.time().second()));
  report_->stop = now;
  const qint64 idleFor = idle_ ? idle_->idleMs() : 0;
  if (idleFor > idleTimeoutMs_) {
    QDateTime stop = now.addMSecs(-idleTimeoutMs_);
    if (stop < report_->start) {
      stop = report_->start;
    }
    timer_.stop();
    report_->stop = stop;
    report_->modified = now;
    try {
      store_.saveReport(*report_);
      emit reportSaved(*report_);
    } catch (const std::exception &ex) {
      emit errorOccurred(QString::fromUtf8(ex.what()));
    }
    report_.reset();
    project_.reset();
    setState(TrackerState::Inactive);
    return;
  }
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
