#pragma once

#include "settings.hpp"
#include "store.hpp"

#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>

struct SyncResult {
  bool ok = false;
  QString message;
  int unsyncedLeft = 0;
  int conflictsLeft = 0;
  int pushedCreates = 0;
  int pushedUpdates = 0;
  int pushedDeletes = 0;
  int pulled = 0;
  int conflicts = 0;
  int linkedOrphans = 0;
  bool skipped = false;
};

class Engine : public QObject {
  Q_OBJECT
public:
  Engine(Store &store, Settings &settings, QObject *parent = nullptr);
  ~Engine() override;

  void startPeriodic();
  void stopPeriodic();
  void restartPeriodic();

  SyncResult sync();
  SyncResult syncCatalog();

signals:
  void finished(SyncResult result);

private:
  void pullCatalog();
  struct PushStats {
    int creates = 0;
    int updates = 0;
    int deletes = 0;
  };
  struct PullStats {
    int pulled = 0;
    int conflicts = 0;
    int linked = 0;
  };
  PushStats pushOps();
  PullStats pullReports();
  qint64 mapServerProject(qint64 serverProjectId);
  Report serverReport(const Report &local) const;

  Store &store_;
  Settings &settings_;
  std::mutex syncMu_;
  std::mutex lifeMu_;
  std::thread periodic_;
  std::atomic<bool> stop_{false};
  int failStreak_ = 0;
};
