#include "dialogs.hpp"

#include "ui_reportdialog.h"
#include "ui_settingsdialog.h"
#include "version.hpp"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTextDocument>
namespace {

int commentRowHeight(const QFontMetrics &fm, const QString &text) {
  const int lines = qMax(1, text.count(QLatin1Char('\n')) + 1);
  return fm.lineSpacing() * lines + 8;
}

void fitCommentEditor(QPlainTextEdit *edit, int row) {
  const int h = commentRowHeight(edit->fontMetrics(), edit->toPlainText());
  QWidget *parent = edit->parentWidget();
  QTableWidget *table = nullptr;
  while (parent != nullptr && table == nullptr) {
    table = qobject_cast<QTableWidget *>(parent);
    parent = parent->parentWidget();
  }
  if (table != nullptr && row >= 0) {
    table->setRowHeight(row, h);
  }
  QRect rect = edit->geometry();
  rect.setHeight(h);
  edit->setGeometry(rect);
}

class MultilineCommentDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                        const QModelIndex &index) const override {
    auto *edit = new QPlainTextEdit(parent);
    edit->setTabChangesFocus(true);
    edit->setFrameShape(QFrame::NoFrame);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const int row = index.row();
    QObject::connect(edit->document(), &QTextDocument::contentsChanged, edit,
                     [edit, row] { fitCommentEditor(edit, row); });
    return edit;
  }

  void setEditorData(QWidget *editor, const QModelIndex &index) const override {
    if (auto *edit = qobject_cast<QPlainTextEdit *>(editor)) {
      edit->setPlainText(index.data(Qt::EditRole).toString());
      fitCommentEditor(edit, index.row());
    }
  }

  void setModelData(QWidget *editor, QAbstractItemModel *model,
                    const QModelIndex &index) const override {
    if (auto *edit = qobject_cast<QPlainTextEdit *>(editor)) {
      model->setData(index, edit->toPlainText(), Qt::EditRole);
    }
  }

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(commentRowHeight(option.fontMetrics,
                                    index.data(Qt::DisplayRole).toString()));
    return size;
  }

  void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const override {
    QRect rect = option.rect;
    rect.setHeight(commentRowHeight(editor->fontMetrics(),
                                    index.data(Qt::EditRole).toString()));
    editor->setGeometry(rect);
  }

  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::KeyPress) {
      const auto *key = static_cast<const QKeyEvent *>(event);
      if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        return false;
      }
    }
    return QStyledItemDelegate::eventFilter(object, event);
  }
};

QString syncFlags(const Report &r) {
  QString s;
  if (r.conflict) {
    s += QStringLiteral("CONFLICT (local edits kept; will re-push) ");
  }
  if (r.serverId > 0) {
    s += QStringLiteral("on server(%1) ").arg(r.serverId);
  }
  if (r.updated) {
    s += QStringLiteral("locally updated ");
  }
  if (r.deleted) {
    s += QStringLiteral("to be deleted ");
  }
  return s.isEmpty() ? QStringLiteral("local only") : s;
}

QString lineComment(const Report &r, qint64 projectId) {
  for (const ReportLine &ln : r.lines) {
    if (ln.projectId == projectId) {
      return ln.comment;
    }
  }
  if (r.lines.empty() && r.projectId == projectId) {
    return r.comment;
  }
  return {};
}

void fillCommentTable(QTableWidget *table, Store &store, qint64 customerId,
                      const Report &r, qint64 anchorId) {
  table->setRowCount(0);
  if (customerId <= 0) {
    return;
  }
  std::vector<Project> projects;
  for (const Project &p : store.listProjects(customerId, false)) {
    const bool used = !lineComment(r, p.id).isNull() || p.id == r.projectId ||
                      p.id == anchorId;
    if (p.active || used) {
      projects.push_back(p);
    }
  }
  table->setRowCount(static_cast<int>(projects.size()));
  int select = 0;
  bool picked = false;
  for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
    const Project &p = projects[static_cast<size_t>(i)];
    auto *name = new QTableWidgetItem(
        p.isDefault ? p.name + QStringLiteral(" (default)") : p.name);
    name->setData(Qt::UserRole, static_cast<qlonglong>(p.id));
    name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    name->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
    table->setItem(i, 0, name);
    QString text = lineComment(r, p.id);
    if (text.isNull()) {
      text = QString();
    }
    auto *comment = new QTableWidgetItem(text);
    comment->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                      Qt::ItemIsEditable);
    comment->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
    table->setItem(i, 1, comment);
    if (!picked && anchorId > 0 && p.id == anchorId) {
      select = i;
      picked = true;
    } else if (!picked && anchorId <= 0 && p.isDefault) {
      select = i;
      picked = true;
    }
  }
  if (table->rowCount() > 0) {
    table->setCurrentCell(select, 1);
  }
}

} // namespace

void configureProjectCommentTable(QTableWidget *table) {
  table->setItemDelegateForColumn(1, new MultilineCommentDelegate(table));
  // Open the comment editor on one click. SelectedClicked waits out the
  // double-click interval before editing, and CurrentChanged would open an
  // editor whenever the highlighted project is selected in code.
  table->setEditTriggers(QAbstractItemView::EditKeyPressed |
                         QAbstractItemView::AnyKeyPressed);
  QObject::connect(table, &QTableWidget::cellClicked, table,
                   [table](int row, int column) {
                     if (column != 1) {
                       return;
                     }
                     if (QTableWidgetItem *item = table->item(row, column)) {
                       table->editItem(item);
                     }
                   });
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideRight);
  if (table->columnCount() >= 2) {
    table->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  }
  const int oneLine = commentRowHeight(table->fontMetrics(), QString());
  table->verticalHeader()->setMinimumSectionSize(oneLine);
  table->verticalHeader()->setDefaultSectionSize(oneLine);
  table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
  QObject::connect(table, &QTableWidget::itemChanged, table,
                   [table](QTableWidgetItem *item) {
                     if (item == nullptr || item->column() != 1) {
                       return;
                     }
                     table->setRowHeight(
                         item->row(),
                         commentRowHeight(table->fontMetrics(), item->text()));
                   });
}

std::optional<Config> showSettingsDialog(QWidget *parent, const Config &cfg) {
  QDialog dlg(parent);
  Ui::SettingsDialog ui;
  ui.setupUi(&dlg);
  // The form's geometry marks the dialog as already placed. Clear that so
  // exec() centers it on the parent, then keep the designed size.
  dlg.resize(520, 360);
  dlg.setAttribute(Qt::WA_Moved, false);
  ui.versionEdit->setText(ergotimeVersion());
  ui.usernameEdit->setText(cfg.username);
  ui.passwordEdit->setText(cfg.password);
  ui.idleSpin->setValue(cfg.idleTimeoutSec);
  ui.dataDirEdit->setText(cfg.dataDir);
  ui.catalogSyncSpin->setValue(cfg.activitySyncIntervalSec);
  ui.reportSyncSpin->setValue(cfg.reportSyncIntervalSec);
  ui.serverEdit->setText(cfg.serverUrl);
  ui.networkTimeoutSpin->setValue(cfg.networkTimeoutSec);
  QObject::connect(ui.buttonBox, &QDialogButtonBox::accepted, &dlg,
                   &QDialog::accept);
  QObject::connect(ui.buttonBox, &QDialogButtonBox::rejected, &dlg,
                   &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) {
    return std::nullopt;
  }
  Config out = cfg;
  out.username = ui.usernameEdit->text();
  out.password = ui.passwordEdit->text();
  out.idleTimeoutSec = ui.idleSpin->value();
  out.activitySyncIntervalSec = ui.catalogSyncSpin->value();
  out.reportSyncIntervalSec = ui.reportSyncSpin->value();
  out.serverUrl = ui.serverEdit->text();
  out.networkTimeoutSec = ui.networkTimeoutSpin->value();
  return out;
}

bool showReportDialog(QWidget *parent, Store &store, const Report *existing,
                      const QDate &defaultDate) {
  const bool isNew = existing == nullptr || existing->id == 0;
  Report r = existing ? *existing : Report{};
  if (isNew) {
    const QDateTime now = QDateTime::currentDateTime();
    const QDate day = defaultDate.isValid() ? defaultDate : now.date();
    const QDateTime start(day,
                          QTime(now.time().hour(), now.time().minute(), 0));
    r.serverId = -1;
    r.userId = 1;
    r.start = start;
    r.stop = start.addSecs(30 * 60);
    r.seq = 0;
  }

  QDialog dlg(parent);
  Ui::ReportDialog ui;
  ui.setupUi(&dlg);
  dlg.setWindowTitle(isNew ? QStringLiteral("Create report")
                           : QStringLiteral("Edit report"));
  // The form's geometry marks the dialog as already placed. Clear that so
  // exec() centers it on the parent, then keep the designed size.
  dlg.resize(640, 560);
  dlg.setAttribute(Qt::WA_Moved, false);
  ui.verticalLayout->setStretch(0, 1);
  ui.syncStateLabel->setText(syncFlags(r));

  auto *custCombo = ui.customerCombo;
  qint64 selectedCust = r.customerId;
  if (selectedCust <= 0 && r.projectId > 0) {
    selectedCust = store.projectCustomerLocalId(r.projectId);
  }
  for (const Customer &c : store.listCustomers(true)) {
    custCombo->addItem(c.name, static_cast<qlonglong>(c.id));
  }
  if (selectedCust > 0 &&
      custCombo->findData(static_cast<qlonglong>(selectedCust)) < 0) {
    if (auto c = store.getCustomer(selectedCust)) {
      custCombo->addItem(c->name, static_cast<qlonglong>(c->id));
    }
  }
  const int custIdx = custCombo->findData(static_cast<qlonglong>(selectedCust));
  if (custIdx >= 0) {
    custCombo->setCurrentIndex(custIdx);
  }
  auto *comments = ui.commentTable;
  configureProjectCommentTable(comments);
  qint64 anchorId = 0;
  if (r.lines.size() == 1) {
    anchorId = r.lines[0].projectId;
  } else if (r.projectId > 0) {
    anchorId = r.projectId;
  }
  auto reloadProjects = [&] {
    const qint64 custId = custCombo->currentData().toLongLong();
    const bool sameCustomer = custId == selectedCust;
    fillCommentTable(comments, store, custId, sameCustomer ? r : Report{},
                     sameCustomer ? anchorId : 0);
  };
  reloadProjects();
  QObject::connect(custCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                   &dlg, [&](int) { reloadProjects(); });
  auto *dtStart = ui.startEdit;
  auto *dtStop = ui.stopEdit;
  dtStart->setDateTime(r.start.toLocalTime());
  dtStop->setDateTime((r.stop ? *r.stop : r.start.addSecs(1800)).toLocalTime());
  if (!isNew) {
    const auto tip = QStringLiteral(
        "Changing the date asks for confirmation when you save.");
    dtStart->setToolTip(tip);
    dtStop->setToolTip(tip);
  }
  // Edit remembers the dates last accepted. A different day asks on save.
  // Create does not ask.
  QDate acceptedStart = dtStart->date();
  QDate acceptedStop = dtStop->date();
  auto *len = ui.lengthLabel;
  auto updateLen = [&] {
    if (dtStop->dateTime() < dtStart->dateTime()) {
      dtStop->setDateTime(dtStart->dateTime());
    }
    len->setText(
        formatDurationHms(dtStart->dateTime().msecsTo(dtStop->dateTime())));
  };
  QObject::connect(dtStart, &QDateTimeEdit::dateTimeChanged, &dlg, updateLen);
  QObject::connect(dtStop, &QDateTimeEdit::dateTimeChanged, &dlg, updateLen);
  updateLen();

  // Copy the editor pointers into the clicked handlers. A reference to a
  // local would dangle if the connection outlived this function.
  auto connectNudge = [&](QPushButton *btn, QDateTimeEdit *primary, int secs,
                          bool shiftBoth) {
    QObject::connect(btn, &QPushButton::clicked, &dlg,
                     [dtStart, dtStop, primary, secs, shiftBoth, &updateLen] {
                       const QDateTime start = dtStart->dateTime();
                       const QDateTime stop = dtStop->dateTime();
                       {
                         const QSignalBlocker blockStart(dtStart);
                         const QSignalBlocker blockStop(dtStop);
                         if (shiftBoth) {
                           dtStart->setDateTime(start.addSecs(secs));
                           dtStop->setDateTime(stop.addSecs(secs));
                         } else {
                           primary->setDateTime(
                               primary->dateTime().addSecs(secs));
                         }
                       }
                       updateLen();
                     });
  };
  connectNudge(ui.startMinusHour, dtStart, -3600, true);
  connectNudge(ui.startPlusHour, dtStart, 3600, true);
  connectNudge(ui.startMinusMinute, dtStart, -60, true);
  connectNudge(ui.startPlusMinute, dtStart, 60, true);
  connectNudge(ui.stopMinusHour, dtStop, -3600, false);
  connectNudge(ui.stopPlusHour, dtStop, 3600, false);
  connectNudge(ui.stopMinusMinute, dtStop, -60, false);
  connectNudge(ui.stopPlusMinute, dtStop, 60, false);

  ui.deleteButton->setEnabled(!isNew);

  bool saved = false;
  QObject::connect(ui.cancelButton, &QPushButton::clicked, &dlg,
                   &QDialog::reject);
  QObject::connect(ui.saveButton, &QPushButton::clicked, &dlg, [&] {
    const qint64 custId = custCombo->currentData().toLongLong();
    std::vector<Project> projects = store.listProjects(custId, false);
    std::vector<ReportLine> rows;
    qint64 anchor = 0;
    const int current = comments->currentRow();
    for (int i = 0; i < comments->rowCount(); ++i) {
      const auto *name = comments->item(i, 0);
      const auto *text = comments->item(i, 1);
      if (!name) {
        continue;
      }
      ReportLine ln;
      ln.projectId = name->data(Qt::UserRole).toLongLong();
      ln.comment = text ? text->text() : QString();
      rows.push_back(ln);
      if (i == current) {
        anchor = ln.projectId;
      }
    }
    if (!assignReportLines(r, projects, rows, anchor)) {
      QMessageBox::warning(
          &dlg, QStringLiteral("Save report"),
          QStringLiteral("Please select a customer with a project."));
      return;
    }
    if (!isNew) {
      const QDate startDay = dtStart->date();
      const QDate stopDay = dtStop->date();
      const bool startChanged = startDay != acceptedStart;
      const bool stopChanged = stopDay != acceptedStop;
      if (startChanged || stopChanged) {
        QStringList lines;
        if (startChanged) {
          lines << QStringLiteral("Start date changed from %1 to %2.")
                       .arg(acceptedStart.toString(Qt::ISODate),
                            startDay.toString(Qt::ISODate));
        }
        if (stopChanged) {
          lines << QStringLiteral("Stop date changed from %1 to %2.")
                       .arg(acceptedStop.toString(Qt::ISODate),
                            stopDay.toString(Qt::ISODate));
        }
        lines << QString();
        lines << QStringLiteral("Was that intended?");
        const auto answer = QMessageBox::question(
            &dlg, QStringLiteral("Change report date?"),
            lines.join(QLatin1Char('\n')), QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
          if (startChanged) {
            QDateTime dt = dtStart->dateTime();
            dt.setDate(acceptedStart);
            dtStart->setDateTime(dt);
          }
          if (stopChanged) {
            QDateTime dt = dtStop->dateTime();
            dt.setDate(acceptedStop);
            dtStop->setDateTime(dt);
          }
          updateLen();
          return;
        }
        acceptedStart = dtStart->date();
        acceptedStop = dtStop->date();
      }
    }
    QDateTime start = dtStart->dateTime();
    QDateTime stop = dtStop->dateTime();
    start.setTime(QTime(start.time().hour(), start.time().minute(),
                        start.time().second()));
    stop.setTime(
        QTime(stop.time().hour(), stop.time().minute(), stop.time().second()));
    if (stop < start) {
      QMessageBox::warning(
          &dlg, QStringLiteral("Save report"),
          QStringLiteral("Stop time must be after start time."));
      return;
    }
    if (start.secsTo(stop) >= 24 * 3600) {
      QMessageBox::warning(
          &dlg, QStringLiteral("Save report"),
          QStringLiteral("Report cannot be 24 hours or longer."));
      return;
    }
    r.start = start;
    r.stop = stop;
    try {
      store.saveReport(r);
    } catch (const std::exception &ex) {
      QMessageBox::critical(&dlg, QStringLiteral("Save report"),
                            QString::fromUtf8(ex.what()));
      return;
    }
    saved = true;
    dlg.accept();
  });
  QObject::connect(ui.deleteButton, &QPushButton::clicked, &dlg, [&] {
    if (QMessageBox::question(
            &dlg, QStringLiteral("Delete report"),
            QStringLiteral("Do you want to delete the report?")) !=
        QMessageBox::Yes) {
      return;
    }
    try {
      store.deleteReport(r.id);
    } catch (const std::exception &ex) {
      QMessageBox::critical(&dlg, QStringLiteral("Delete report"),
                            QString::fromUtf8(ex.what()));
      return;
    }
    saved = true;
    dlg.accept();
  });
  dlg.exec();
  return saved;
}
