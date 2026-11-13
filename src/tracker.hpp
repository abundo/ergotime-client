#pragma once

#include "domain.hpp"
#include "idle.hpp"
#include "store.hpp"

#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

class Tracker : public QObject {
  Q_OBJECT
public:
  Tracker(Store &store, int idleTimeoutMs, QObject *parent = nullptr);

  TrackerState state() const { return state_; }
  void setComment(const QString &comment);
  void setDraft(const Report &draft);
  void setIdleTimeoutMs(int ms);
  // startedAt is the local time the interval began. An invalid value means now.
  void start(const Report &draft, const QDateTime &startedAt = {});
  // Move the running interval's start. Ignored when inactive or when startedAt
  // is invalid or later than the current second.
  void setStart(const QDateTime &startedAt);
  const Report *report() const { return report_ ? &*report_ : nullptr; }
  void stop();

signals:
  void stateChanged(TrackerState state);
  void statusChanged(Status status);
  void reportSaved(Report report);
  void errorOccurred(QString message);

private:
  void tick();
  void setState(TrackerState s);

  Store &store_;
  std::unique_ptr<Idle> idle_;
  QTimer timer_;
  TrackerState state_ = TrackerState::Inactive;
  std::optional<Report> report_;
  std::optional<Project> project_;
  int idleTimeoutMs_;
};
