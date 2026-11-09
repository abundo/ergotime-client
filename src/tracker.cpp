#include "tracker.hpp"

#include <QDateTime>

#include <exception>

Tracker::Tracker(Store &store, int idleTimeoutMs, QObject *parent)
    : QObject(parent), store_(store), idle_(Idle::create()),
      idleTimeoutMs_(idleTimeoutMs > 0 ? idleTimeoutMs : 600000) {
  timer_.setInterval(1000);
  connect(&timer_, &QTimer::timeout, this, &Tracker::tick);
}

void Tracker::setComment(const QString &comment) {
  if (report_) {
    report_->comment = comment;
  }
}

void Tracker::setIdleTimeoutMs(int ms) {
  if (ms > 0) {
    idleTimeoutMs_ = ms;
  }
}

void Tracker::start(const Project &project) {
  if (state_ == TrackerState::Active) {
    return;
  }
  const QDateTime now = QDateTime::currentDateTime().toLocalTime();
  Report r;
  r.projectId = project.id;
  r.start = QDateTime(now.date(), QTime(now.time().hour(), now.time().minute(),
                                        now.time().second()));
  r.modified = r.start;
  r.updated = true;
  r.serverId = -1;
  project_ = project;
  report_ = r;
  setState(TrackerState::Active);
  timer_.start();
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
