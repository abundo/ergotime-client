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
  void setIdleTimeoutMs(int ms);
  void start(const Project &project);
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
