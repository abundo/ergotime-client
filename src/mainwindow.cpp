#include "mainwindow.hpp"

#include "dialogs.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateEdit>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QTimeZone>
#include <QVBoxLayout>

#include <thread>

namespace {

QIcon stateIcon(TrackerState state) {
  QColor color = QColor(QStringLiteral("#9aa0a6"));
  if (state == TrackerState::Active) {
    color = QColor(QStringLiteral("#1e8e3e"));
  } else if (state == TrackerState::Idle) {
    color = QColor(QStringLiteral("#f9ab00"));
  }
  QPixmap pix(32, 32);
  pix.fill(Qt::transparent);
  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(color);
  p.setPen(Qt::NoPen);
  p.drawEllipse(2, 2, 28, 28);
  return QIcon(pix);
}

void addItem(QListWidget *list, const QString &text, qint64 id) {
  auto *item = new QListWidgetItem(text);
  item->setData(Qt::UserRole, static_cast<qlonglong>(id));
  list->addItem(item);
}

qint64 itemId(QListWidgetItem *item) {
  if (!item) {
    return 0;
  }
  return item->data(Qt::UserRole).toLongLong();
}

} // namespace

MainWindow::MainWindow(Settings &settings, Store &store, Engine &engine,
                       Tracker &tracker, QWidget *parent)
    : QMainWindow(parent), settings_(settings), store_(store), engine_(engine),
      tracker_(tracker) {
  setWindowTitle(QStringLiteral("ErgoTime"));
  resize(820, 560);
  setWindowIcon(stateIcon(TrackerState::Inactive));
  buildUi();

  connect(&tracker_, &Tracker::stateChanged, this,
          [this](TrackerState s) { setTrackerState(s); });
  connect(&tracker_, &Tracker::statusChanged, this, [this](const Status &st) {
    lengthLabel_->setText(
        QStringLiteral("Duration: %1").arg(formatDurationHms(st.lengthMs)));
    idleLabel_->setText(
        QStringLiteral("Idle: %1").arg(formatDurationHms(st.idleMs)));
  });
  connect(&tracker_, &Tracker::reportSaved, this, [this](const Report &) {
    refreshReports();
    if (settings_.get().autoSync) {
      syncNow();
    }
  });
  connect(&tracker_, &Tracker::errorOccurred, this, [this](const QString &msg) {
    statusLabel_->setText(msg);
    qWarning() << msg;
  });
  connect(&engine_, &Engine::finished, this, [this](const SyncResult &res) {
    if (!res.ok && !res.skipped) {
      syncLabel_->setText(QStringLiteral("error: %1").arg(res.message));
    } else if (!res.message.isEmpty()) {
      syncLabel_->setText(res.message);
    }
    refreshSyncStatus();
    refreshCatalog();
    refreshReports();
  });

  const Config cfg = settings_.get();
  if (!cfg.windowGeometry.isEmpty()) {
    restoreGeometry(cfg.windowGeometry);
  }
  if (!cfg.splitterState.isEmpty()) {
    splitter_->restoreState(cfg.splitterState);
  }
  refreshCatalog();
  dateEdit_->setDate(QDate::currentDate());
  refreshSyncStatus();
  setTrackerState(TrackerState::Inactive);
  show();
  engine_.startPeriodic();
  if (cfg.autoSync) {
    syncNow();
  }
}

MainWindow::~MainWindow() { joinWorkers(); }

void MainWindow::spawn(std::function<void()> fn) {
  std::lock_guard lock(workersMu_);
  std::vector<std::thread> live;
  for (auto &t : workers_) {
    if (t.joinable()) {
      live.push_back(std::move(t));
    }
  }
  workers_.swap(live);
  workers_.emplace_back(std::move(fn));
}

void MainWindow::joinWorkers() {
  std::vector<std::thread> local;
  {
    std::lock_guard lock(workersMu_);
    local.swap(workers_);
  }
  for (auto &t : local) {
    if (t.joinable()) {
      t.join();
    }
  }
}

void MainWindow::buildUi() {
  auto *file = menuBar()->addMenu(QStringLiteral("&File"));
  file->addAction(QStringLiteral("Sync now"), this, &MainWindow::syncNow);
  file->addSeparator();
  file->addAction(QStringLiteral("Quit"), this, &MainWindow::quitApp);
  auto *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
  edit->addAction(QStringLiteral("Settings…"), this, [this] {
    auto updated = showSettingsDialog(this, settings_.get());
    if (!updated) {
      return;
    }
    try {
      settings_.update(*updated);
    } catch (const std::exception &ex) {
      QMessageBox::critical(this, QStringLiteral("Settings"),
                            QString::fromUtf8(ex.what()));
      return;
    }
    tracker_.setIdleTimeoutMs(updated->idleTimeoutMs());
    engine_.restartPeriodic();
    statusLabel_->setText(QStringLiteral("Settings saved"));
  });
  auto *help = menuBar()->addMenu(QStringLiteral("&Help"));
  help->addAction(QStringLiteral("About ErgoTime"), this, [this] {
    QMessageBox::about(
        this, QStringLiteral("About ErgoTime"),
        QStringLiteral("<h3>ErgoTime</h3><p>Desktop time tracker with offline "
                       "support and server sync.</p>"
                       "<p>C++ / Qt 6 client. License: GPLv3</p>"));
  });
  help->addAction(QStringLiteral("About Qt"), qApp, &QApplication::aboutQt);

  auto *central = new QWidget;
  auto *root = new QVBoxLayout(central);
  splitter_ = new QSplitter(Qt::Vertical);

  auto *top = new QWidget;
  auto *topLay = new QVBoxLayout(top);
  topLay->setContentsMargins(0, 0, 0, 0);
  auto *track = new QGroupBox(QStringLiteral("Time tracking"));
  auto *trackLay = new QHBoxLayout(track);
  auto *left = new QVBoxLayout;
  auto *lists = new QHBoxLayout;
  auto *custCol = new QVBoxLayout;
  custCol->addWidget(new QLabel(QStringLiteral("Customer")));
  customers_ = new QListWidget;
  customers_->setFixedWidth(200);
  customers_->setToolTip(
      QStringLiteral("Recently used customers appear at the top"));
  custCol->addWidget(customers_, 1);
  auto *projCol = new QVBoxLayout;
  projCol->addWidget(new QLabel(QStringLiteral("Project")));
  projects_ = new QListWidget;
  projects_->setFixedWidth(200);
  projCol->addWidget(projects_, 1);
  lists->addLayout(custCol);
  lists->addLayout(projCol);
  left->addLayout(lists, 1);
  auto *buttons = new QHBoxLayout;
  startBtn_ = new QPushButton(QStringLiteral("Start"));
  stopBtn_ = new QPushButton(QStringLiteral("Stop"));
  auto *syncCat = new QPushButton(QStringLiteral("Sync catalog"));
  syncCat->setToolTip(
      QStringLiteral("Pull customers and projects from the server"));
  buttons->addWidget(startBtn_);
  buttons->addWidget(stopBtn_);
  buttons->addWidget(syncCat);
  buttons->addStretch();
  left->addLayout(buttons);
  trackLay->addLayout(left);
  auto *commentBox = new QGroupBox(QStringLiteral("Comment"));
  auto *commentLay = new QVBoxLayout(commentBox);
  comment_ = new QTextEdit;
  comment_->setAcceptRichText(false);
  comment_->setPlaceholderText(QStringLiteral("What are you working on?"));
  comment_->setEnabled(false);
  commentLay->addWidget(comment_);
  trackLay->addWidget(commentBox, 1);
  topLay->addWidget(track, 1);
  auto *statusRow = new QHBoxLayout;
  stateLabel_ = new QLabel(QStringLiteral("State: inactive"));
  lengthLabel_ = new QLabel(QStringLiteral("Duration: —"));
  idleLabel_ = new QLabel(QStringLiteral("Idle: —"));
  statusRow->addWidget(stateLabel_);
  statusRow->addWidget(lengthLabel_);
  statusRow->addWidget(idleLabel_);
  statusRow->addStretch();
  topLay->addLayout(statusRow);
  splitter_->addWidget(top);

  auto *reports = new QGroupBox(QStringLiteral("Reports"));
  auto *repLay = new QVBoxLayout(reports);
  auto *tool = new QHBoxLayout;
  auto *syncRep = new QPushButton(QStringLiteral("Sync reports"));
  autoSync_ = new QCheckBox(QStringLiteral("Auto"));
  autoSync_->setToolTip(QStringLiteral(
      "When checked, sync reports on app start/exit and after local changes"));
  autoSync_->setChecked(settings_.get().autoSync);
  auto *prev = new QPushButton(QStringLiteral("<"));
  auto *today = new QPushButton(QStringLiteral("Today"));
  auto *next = new QPushButton(QStringLiteral(">"));
  prev->setFixedWidth(32);
  next->setFixedWidth(32);
  dateEdit_ = new QDateEdit;
  dateEdit_->setCalendarPopup(true);
  dateEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  weekday_ = new QLabel;
  auto *create = new QPushButton(QStringLiteral("Create"));
  auto *editBtn = new QPushButton(QStringLiteral("Edit"));
  tool->addWidget(syncRep);
  tool->addWidget(autoSync_);
  tool->addWidget(prev);
  tool->addWidget(today);
  tool->addWidget(next);
  tool->addWidget(dateEdit_);
  tool->addWidget(weekday_);
  tool->addStretch();
  tool->addWidget(create);
  tool->addWidget(editBtn);
  repLay->addLayout(tool);
  table_ = new QTableWidget(0, 7);
  table_->setHorizontalHeaderLabels(
      {QStringLiteral("Start"), QStringLiteral("Stop"),
       QStringLiteral("Customer"), QStringLiteral("Project"),
       QStringLiteral("Duration"), QStringLiteral("Flags"),
       QStringLiteral("Comment")});
  for (int col = 0; col < 6; ++col) {
    table_->horizontalHeader()->setSectionResizeMode(
        col, QHeaderView::ResizeToContents);
  }
  table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setTextElideMode(Qt::ElideRight);
  repLay->addWidget(table_, 1);
  splitter_->addWidget(reports);
  splitter_->setStretchFactor(0, 1);
  splitter_->setStretchFactor(1, 2);
  splitter_->setChildrenCollapsible(false);
  root->addWidget(splitter_, 1);
  syncLabel_ = new QLabel(QStringLiteral("Sync: not run yet"));
  root->addWidget(syncLabel_);
  setCentralWidget(central);
  statusLabel_ = new QLabel(QStringLiteral("Ready"));
  statusBar()->addWidget(statusLabel_);

  tray_ = new QSystemTrayIcon(stateIcon(TrackerState::Inactive), this);
  tray_->setToolTip(QStringLiteral("ErgoTime"));
  trayMenu_ = new QMenu(this);
  tray_->setContextMenu(trayMenu_);
  tray_->show();
  connect(tray_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
              show();
              raise();
              activateWindow();
            }
          });

  connect(customers_, &QListWidget::currentRowChanged, this,
          [this](int) { refreshProjects(); });
  connect(startBtn_, &QPushButton::clicked, this,
          [this] { startProject(selectedId(projects_)); });
  connect(stopBtn_, &QPushButton::clicked, &tracker_, &Tracker::stop);
  connect(syncCat, &QPushButton::clicked, this, &MainWindow::syncCatalogNow);
  connect(syncRep, &QPushButton::clicked, this, &MainWindow::syncNow);
  connect(autoSync_, &QCheckBox::toggled, this, [this](bool on) {
    Config cfg = settings_.get();
    if (cfg.autoSync == on) {
      return;
    }
    cfg.autoSync = on;
    try {
      settings_.update(cfg);
    } catch (const std::exception &ex) {
      qWarning() << ex.what();
      return;
    }
    if (on) {
      syncNow();
    }
  });
  connect(prev, &QPushButton::clicked, this,
          [this] { dateEdit_->setDate(dateEdit_->date().addDays(-1)); });
  connect(today, &QPushButton::clicked, this,
          [this] { dateEdit_->setDate(QDate::currentDate()); });
  connect(next, &QPushButton::clicked, this,
          [this] { dateEdit_->setDate(dateEdit_->date().addDays(1)); });
  connect(dateEdit_, &QDateEdit::dateChanged, this, [this](const QDate &day) {
    weekday_->setText(QLocale().dayName(day.dayOfWeek()));
    refreshReports();
  });
  connect(create, &QPushButton::clicked, this, [this] {
    if (showReportDialog(this, store_, nullptr, selectedDay())) {
      refreshReports();
      if (settings_.get().autoSync) {
        syncNow();
      }
    }
  });
  auto editCurrent = [this] {
    const auto *item = table_->item(table_->currentRow(), 0);
    if (!item) {
      return;
    }
    const qint64 id = item->data(Qt::UserRole).toLongLong();
    if (id <= 0) {
      return;
    }
    try {
      auto report = store_.getReport(id);
      if (!report) {
        return;
      }
      if (showReportDialog(this, store_, &*report, selectedDay())) {
        refreshReports();
        if (settings_.get().autoSync) {
          syncNow();
        }
      }
    } catch (const std::exception &ex) {
      QMessageBox::critical(this, QStringLiteral("Edit report"),
                            QString::fromUtf8(ex.what()));
    }
  };
  connect(editBtn, &QPushButton::clicked, this, editCurrent);
  connect(table_, &QTableWidget::itemDoubleClicked, this, editCurrent);
  connect(comment_, &QTextEdit::textChanged, this, [this] {
    if (!commentEdit_) {
      tracker_.setComment(comment_->toPlainText());
    }
  });
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (quitting_) {
    saveLayout();
    event->accept();
    return;
  }
  event->ignore();
  hide();
}

void MainWindow::saveLayout() {
  settings_.saveWindowLayout(saveGeometry(), splitter_->saveState());
}

void MainWindow::quitApp() {
  quitting_ = true;
  saveLayout();
  joinWorkers();
  QApplication::processEvents();
  engine_.stopPeriodic();
  tracker_.stop();
  if (settings_.get().autoSync) {
    try {
      const SyncResult res = engine_.sync();
      qInfo() << "exit sync" << res.message;
    } catch (const std::exception &ex) {
      qWarning() << "exit sync failed" << ex.what();
    }
  }
  qApp->quit();
}

void MainWindow::syncNow() {
  spawn([this] {
    SyncResult res;
    try {
      res = engine_.sync();
    } catch (const std::exception &ex) {
      res.message = QString::fromUtf8(ex.what());
    }
    QMetaObject::invokeMethod(
        this,
        [this, res] {
          if (!res.ok && !res.skipped) {
            syncLabel_->setText(QStringLiteral("error: %1").arg(res.message));
          } else {
            syncLabel_->setText(res.message);
          }
          refreshSyncStatus();
          refreshCatalog();
          refreshReports();
        },
        Qt::QueuedConnection);
  });
}

void MainWindow::syncCatalogNow() {
  spawn([this] {
    SyncResult res;
    try {
      res = engine_.syncCatalog();
    } catch (const std::exception &ex) {
      res.message = QString::fromUtf8(ex.what());
    }
    QMetaObject::invokeMethod(
        this,
        [this, res] {
          if (!res.ok && !res.skipped) {
            syncLabel_->setText(QStringLiteral("error: %1").arg(res.message));
          } else {
            syncLabel_->setText(res.message);
          }
          refreshSyncStatus();
          refreshCatalog();
        },
        Qt::QueuedConnection);
  });
}

void MainWindow::startProject(qint64 id) {
  if (id <= 0) {
    return;
  }
  try {
    auto project = store_.getProject(id);
    if (!project) {
      return;
    }
    tracker_.start(*project);
    if (project->customerId > 0) {
      settings_.touchRecentCustomer(project->customerId);
      refreshCatalog();
    }
    commentEdit_ = true;
    comment_->clear();
    commentEdit_ = false;
  } catch (const std::exception &ex) {
    QMessageBox::warning(this, QStringLiteral("Start"),
                         QString::fromUtf8(ex.what()));
  }
}

void MainWindow::setTrackerState(TrackerState state) {
  stateLabel_->setText(
      QStringLiteral("State: %1").arg(trackerStateName(state)));
  const bool active = state == TrackerState::Active;
  startBtn_->setEnabled(!active);
  stopBtn_->setEnabled(active);
  comment_->setEnabled(active);
  customers_->setEnabled(true);
  projects_->setEnabled(true);
  listsLocked_ = active;
  if (active) {
    lockedCust_ = selectedId(customers_);
    lockedProj_ = selectedId(projects_);
  } else {
    lockedCust_ = 0;
    lockedProj_ = 0;
    lengthLabel_->setText(QStringLiteral("Duration: —"));
    idleLabel_->setText(QStringLiteral("Idle: —"));
  }
  const QIcon icon = stateIcon(state);
  setWindowIcon(icon);
  tray_->setIcon(icon);
  tray_->setToolTip(
      QStringLiteral("ErgoTime — %1").arg(trackerStateName(state)));
  rebuildTray();
}

qint64 MainWindow::selectedId(QListWidget *list) const {
  return itemId(list->currentItem());
}

QDate MainWindow::selectedDay() const { return dateEdit_->date(); }

void MainWindow::refreshCatalog() {
  std::vector<Customer> custs;
  try {
    custs = store_.listCustomers(true);
  } catch (const std::exception &ex) {
    qWarning() << ex.what();
    return;
  }
  qint64 prev = selectedId(customers_);
  if (listsLocked_ && lockedCust_ > 0) {
    prev = lockedCust_;
  }
  customers_->blockSignals(true);
  customers_->clear();
  const auto recent = settings_.get().recentCustomerIds;
  int recentAdded = 0;
  for (qint64 id : recent) {
    for (const Customer &c : custs) {
      if (c.id == id) {
        addItem(customers_, c.name, c.id);
        recentAdded++;
        break;
      }
    }
  }
  if (recentAdded > 0 && recentAdded < static_cast<int>(custs.size())) {
    auto *sep = new QListWidgetItem(QStringLiteral("──────── All ────────"));
    sep->setFlags(Qt::NoItemFlags);
    customers_->addItem(sep);
  }
  for (const Customer &c : custs) {
    bool seen = false;
    for (qint64 id : recent) {
      if (id == c.id) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      addItem(customers_, c.name, c.id);
    }
  }
  bool picked = false;
  if (prev > 0) {
    for (int i = 0; i < customers_->count(); ++i) {
      if (itemId(customers_->item(i)) == prev) {
        customers_->setCurrentRow(i);
        picked = true;
        break;
      }
    }
  }
  if (!picked) {
    for (int i = 0; i < customers_->count(); ++i) {
      if (itemId(customers_->item(i)) > 0) {
        customers_->setCurrentRow(i);
        break;
      }
    }
  }
  customers_->blockSignals(false);
  refreshProjects();
  rebuildTray();
}

void MainWindow::refreshProjects() {
  qint64 prev = selectedId(projects_);
  if (listsLocked_ && lockedProj_ > 0) {
    prev = lockedProj_;
  }
  qint64 custId = selectedId(customers_);
  if (listsLocked_ && lockedCust_ > 0) {
    custId = lockedCust_;
  }
  projects_->blockSignals(true);
  projects_->clear();
  if (custId > 0) {
    try {
      for (const Project &p : store_.listProjects(custId, true)) {
        addItem(projects_, p.name, p.id);
      }
    } catch (const std::exception &ex) {
      qWarning() << ex.what();
    }
  }
  bool picked = false;
  if (prev > 0) {
    for (int i = 0; i < projects_->count(); ++i) {
      if (itemId(projects_->item(i)) == prev) {
        projects_->setCurrentRow(i);
        picked = true;
        break;
      }
    }
  }
  if (!picked && projects_->count() > 0) {
    projects_->setCurrentRow(0);
  }
  projects_->blockSignals(false);
}

void MainWindow::refreshReports() {
  const QDate day = selectedDay();
  const QDateTime from(day, QTime(0, 0), QTimeZone::LocalTime);
  const QDateTime to = from.addDays(1);
  std::vector<Report> reports;
  try {
    reports = store_.listReportsBetween(from, to);
  } catch (const std::exception &ex) {
    qWarning() << ex.what();
    return;
  }
  const auto projNames = store_.projectNames();
  const auto custNames = store_.customerNames();
  auto nameOf = [](const std::vector<std::pair<qint64, QString>> &rows,
                   qint64 id) {
    for (const auto &[k, v] : rows) {
      if (k == id) {
        return v;
      }
    }
    return QString();
  };
  const int n = static_cast<int>(reports.size());
  table_->setRowCount(n > 0 ? n + 1 : 0);
  qint64 total = 0;
  const QDateTime now = QDateTime::currentDateTime();
  for (int i = 0; i < n; ++i) {
    const Report &r = reports[static_cast<size_t>(i)];
    const QString stop =
        r.stop ? r.stop->toLocalTime().toString(QStringLiteral("HH:mm"))
               : QStringLiteral("…");
    const qint64 custId = store_.projectCustomerLocalId(r.projectId);
    QString cust = nameOf(custNames, custId);
    if (cust.isEmpty() && custId > 0) {
      cust = QStringLiteral("#%1").arg(custId);
    }
    QString proj = nameOf(projNames, r.projectId);
    if (proj.isEmpty()) {
      proj = QStringLiteral("#%1").arg(r.projectId);
    }
    const qint64 dur = r.durationMs(now);
    if (r.stop) {
      total += dur;
    }
    QString flags;
    if (r.conflict) {
      flags = QStringLiteral("conflict");
    } else if (r.serverId > 0) {
      flags = QStringLiteral("synced");
    }
    if (r.updated) {
      if (!flags.isEmpty()) {
        flags += QLatin1Char(' ');
      }
      flags += QStringLiteral("dirty");
    }
    auto *startItem = new QTableWidgetItem(
        r.start.toLocalTime().toString(QStringLiteral("HH:mm")));
    startItem->setData(Qt::UserRole, static_cast<qlonglong>(r.id));
    table_->setItem(i, 0, startItem);
    table_->setItem(i, 1, new QTableWidgetItem(stop));
    table_->setItem(i, 2, new QTableWidgetItem(cust));
    table_->setItem(i, 3, new QTableWidgetItem(proj));
    table_->setItem(i, 4, new QTableWidgetItem(formatDurationHm(dur)));
    table_->setItem(i, 5, new QTableWidgetItem(flags));
    auto *commentItem = new QTableWidgetItem(r.comment);
    if (!r.comment.isEmpty()) {
      commentItem->setToolTip(r.comment);
    }
    table_->setItem(i, 6, commentItem);
  }
  if (n > 0) {
    table_->setItem(n, 0, new QTableWidgetItem(QStringLiteral("Total")));
    for (int col = 1; col < 7; ++col) {
      table_->setItem(
          n, col,
          new QTableWidgetItem(col == 4 ? formatDurationHm(total) : QString()));
    }
  }
}

void MainWindow::refreshSyncStatus() {
  try {
    const SyncStatus st = store_.syncStatus();
    if (!st.lastSyncAt.isValid()) {
      syncLabel_->setText(QStringLiteral("Sync: not run yet"));
      return;
    }
    const QString when = st.lastSyncAt.toLocalTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm"));
    syncLabel_->setText(
        QStringLiteral("Sync: %1 at %2 (seq %3)")
            .arg(st.lastOk ? st.lastMessage
                           : QStringLiteral("error: ") + st.lastMessage,
                 when)
            .arg(st.lastSeq));
  } catch (const std::exception &ex) {
    syncLabel_->setText(QString::fromUtf8(ex.what()));
  }
}

void MainWindow::rebuildTray() {
  if (!trayMenu_) {
    return;
  }
  trayMenu_->clear();
  try {
    for (const Customer &c : store_.listCustomers(true)) {
      const auto projs = store_.listProjects(c.id, true);
      if (projs.empty()) {
        continue;
      }
      auto *sub = trayMenu_->addMenu(c.name);
      for (const Project &p : projs) {
        sub->addAction(p.name, this, [this, id = p.id] { startProject(id); });
      }
    }
  } catch (const std::exception &ex) {
    qWarning() << ex.what();
  }
  trayMenu_->addSeparator();
  trayMenu_->addAction(QStringLiteral("Stop"), &tracker_, &Tracker::stop);
  trayMenu_->addSeparator();
  trayMenu_->addAction(QStringLiteral("Show window"), this, [this] {
    show();
    raise();
    activateWindow();
  });
  trayMenu_->addAction(QStringLiteral("Sync catalog"), this,
                       &MainWindow::syncCatalogNow);
  trayMenu_->addAction(QStringLiteral("Sync now"), this, &MainWindow::syncNow);
  trayMenu_->addSeparator();
  trayMenu_->addAction(QStringLiteral("Exit"), this, &MainWindow::quitApp);
}
