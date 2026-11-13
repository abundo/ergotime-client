#pragma once

#include "settings.hpp"
#include "store.hpp"
#include "sync.hpp"
#include "tracker.hpp"

#include <QDate>
#include <QMainWindow>
#include <QSystemTrayIcon>

#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class QCheckBox;
class QDateEdit;
class QDateTimeEdit;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QSplitter;
class QTableWidget;

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  MainWindow(Settings &settings, Store &store, Engine &engine, Tracker &tracker,
             QWidget *parent = nullptr);
  ~MainWindow() override;

  void syncNow();
  void syncCatalogNow();
  void startProject(qint64 id);
  void quitApp();
  void spawn(std::function<void()> fn);
  void joinWorkers();

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  void buildUi();
  void refreshCatalog();
  void fillCommentTable(qint64 customerId, qint64 anchorId);
  void pushDraft();
  void beginTracking();
  void applyStartAdjustment();
  void refreshReports();
  void refreshSyncStatus();
  void rebuildTray();
  void saveLayout();
  void setTrackerState(TrackerState state);
  qint64 selectedId(QListWidget *list) const;
  QDate selectedDay() const;

  Settings &settings_;
  Store &store_;
  Engine &engine_;
  Tracker &tracker_;

  QSplitter *splitter_ = nullptr;
  QSplitter *trackSplitter_ = nullptr;
  QListWidget *customers_ = nullptr;
  QPushButton *startBtn_ = nullptr;
  QPushButton *stopBtn_ = nullptr;
  QDateTimeEdit *startEdit_ = nullptr;
  QTableWidget *comment_ = nullptr;
  QLabel *stateLabel_ = nullptr;
  QLabel *lengthLabel_ = nullptr;
  QLabel *idleLabel_ = nullptr;
  QLabel *syncLabel_ = nullptr;
  QLabel *statusLabel_ = nullptr;
  QTableWidget *table_ = nullptr;
  QCheckBox *autoSync_ = nullptr;
  QDateEdit *dateEdit_ = nullptr;
  QLabel *weekday_ = nullptr;

  QSystemTrayIcon *tray_ = nullptr;
  QMenu *trayMenu_ = nullptr;

  std::mutex workersMu_;
  std::vector<std::thread> workers_;
  bool quitting_ = false;
  bool listsLocked_ = false;
  qint64 lockedCust_ = 0;
  qint64 lockedProj_ = 0;
  bool commentEdit_ = false;
  // True once the user edits the start field while tracking is inactive.
  // Until then the field follows the clock and Start uses the current second.
  bool startTimeEdited_ = false;
};
