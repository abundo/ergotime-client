#include "dialogs.hpp"

#include "ui_reportdialog.h"
#include "ui_settingsdialog.h"
#include "version.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFocusEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
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

QString stripDefaultSuffix(QString name) {
  name = name.trimmed();
  const QString suffix = QStringLiteral(" (default)");
  if (name.endsWith(suffix)) {
    name.chop(suffix.size());
  }
  return name.trimmed();
}

QString projectMatchKey(const QString &name) {
  return stripDefaultSuffix(name).toCaseFolded();
}

struct NotedComment {
  qint64 projectId = 0;
  QString name;
  QString comment;
};

// The comment cell keeps its text in the editor until the view commits it.
void commitCommentEditor(QTableWidget *table) {
  commitOpenCommentEditor(table);
}

std::vector<NotedComment> notesFromCommentTable(QTableWidget *table) {
  commitCommentEditor(table);
  QPlainTextEdit *open = nullptr;
  const auto edits = table->findChildren<QPlainTextEdit *>();
  for (QPlainTextEdit *edit : edits) {
    if (edit->isVisible()) {
      open = edit;
      break;
    }
  }
  const int editingRow = open != nullptr ? table->currentRow() : -1;
  std::vector<NotedComment> notes;
  for (int i = 0; i < table->rowCount(); ++i) {
    const auto *name = table->item(i, 0);
    const auto *text = table->item(i, 1);
    if (!name) {
      continue;
    }
    NotedComment note;
    note.projectId = name->data(Qt::UserRole).toLongLong();
    note.name = name->text();
    if (open != nullptr && i == editingRow) {
      note.comment = open->toPlainText();
    } else {
      note.comment = text ? text->text() : QString();
    }
    notes.push_back(note);
  }
  return notes;
}

void appendCommentBlock(QString &dst, const QString &text) {
  if (text.trimmed().isEmpty()) {
    return;
  }
  if (!dst.isEmpty()) {
    dst += QStringLiteral("\n\n");
  }
  dst += text;
}

// Place notes on custId's projects. The same project id is kept, then a
// project with the same name. Anything left over is kept on the default
// project so changing customer does not drop the text.
Report commentsForCustomer(Store &store, qint64 customerId,
                           const std::vector<NotedComment> &notes,
                           qint64 anchorId) {
  Report out;
  const std::vector<Project> projects = store.listProjects(customerId, false);
  if (projects.empty()) {
    return out;
  }
  std::vector<QString> texts(projects.size());
  int def = -1;
  for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
    if (projects[static_cast<size_t>(i)].isDefault && def < 0) {
      def = i;
    }
  }
  auto indexOfId = [&](qint64 id) {
    if (id <= 0) {
      return -1;
    }
    for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
      if (projects[static_cast<size_t>(i)].id == id) {
        return i;
      }
    }
    return -1;
  };
  auto indexOfName = [&](const QString &name) {
    const QString key = projectMatchKey(name);
    if (key.isEmpty()) {
      return -1;
    }
    for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
      if (projectMatchKey(projects[static_cast<size_t>(i)].name) == key) {
        return i;
      }
    }
    return -1;
  };
  struct Pending {
    QString name;
    QString comment;
  };
  std::vector<Pending> leftover;
  for (const NotedComment &note : notes) {
    if (note.comment.trimmed().isEmpty()) {
      continue;
    }
    int idx = indexOfId(note.projectId);
    if (idx < 0) {
      idx = indexOfName(note.name);
    }
    if (idx >= 0) {
      appendCommentBlock(texts[static_cast<size_t>(idx)], note.comment);
    } else {
      leftover.push_back(Pending{note.name, note.comment});
    }
  }
  if (!leftover.empty()) {
    const int idx = def >= 0 ? def : 0;
    QString &dst = texts[static_cast<size_t>(idx)];
    const bool label = leftover.size() > 1 || !dst.trimmed().isEmpty();
    for (const Pending &pending : leftover) {
      QString block = pending.comment;
      if (label) {
        const QString shown = stripDefaultSuffix(pending.name);
        if (!shown.isEmpty()) {
          block = shown + QLatin1Char('\n') + pending.comment;
        }
      }
      appendCommentBlock(dst, block);
    }
  }
  qint64 newAnchor = 0;
  if (indexOfId(anchorId) >= 0) {
    newAnchor = anchorId;
  } else {
    for (const NotedComment &note : notes) {
      if (note.projectId != anchorId) {
        continue;
      }
      const int idx = indexOfName(note.name);
      if (idx >= 0) {
        newAnchor = projects[static_cast<size_t>(idx)].id;
      }
      break;
    }
  }
  for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
    const QString &text = texts[static_cast<size_t>(i)];
    if (text.trimmed().isEmpty()) {
      continue;
    }
    ReportLine ln;
    ln.projectId = projects[static_cast<size_t>(i)].id;
    ln.comment = text;
    out.lines.push_back(ln);
  }
  out.projectId = newAnchor;
  return out;
}

// A stored customer can disagree with the project the lines belong to.
// Open on the customer that owns those lines so the comments are visible.
qint64 customerForReport(Store &store, const Report &r, bool isNew) {
  if (isNew) {
    if (r.customerId > 0) {
      return r.customerId;
    }
    if (r.projectId > 0) {
      return store.projectCustomerLocalId(r.projectId);
    }
    return 0;
  }
  const qint64 anchorOwner =
      r.projectId > 0 ? store.projectCustomerLocalId(r.projectId) : 0;
  bool linesOnAnchor = r.lines.empty() && anchorOwner > 0;
  for (const ReportLine &ln : r.lines) {
    if (anchorOwner > 0 &&
        store.projectCustomerLocalId(ln.projectId) == anchorOwner) {
      linesOnAnchor = true;
      break;
    }
  }
  if (anchorOwner > 0 &&
      (r.customerId <= 0 || (r.customerId != anchorOwner && linesOnAnchor))) {
    return anchorOwner;
  }
  if (r.customerId <= 0) {
    for (const ReportLine &ln : r.lines) {
      const qint64 owner = store.projectCustomerLocalId(ln.projectId);
      if (owner > 0) {
        return owner;
      }
    }
  }
  return r.customerId;
}

void fillCommentTable(QTableWidget *table, Store &store, qint64 customerId,
                      const Report &r, qint64 anchorId) {
  // Drop the editor before the rows. Removing the row first leaves the
  // editor registered against an index that no longer exists, and Qt warns
  // that commitData was called for an editor that does not belong to the view.
  commitOpenCommentEditor(table);
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

void commitOpenCommentEditor(QTableWidget *table) {
  if (table == nullptr) {
    return;
  }
  QPointer<QPlainTextEdit> open;
  const auto edits = table->findChildren<QPlainTextEdit *>();
  for (QPlainTextEdit *edit : edits) {
    if (edit->isVisible()) {
      open = edit;
      break;
    }
  }
  if (open == nullptr) {
    return;
  }
  // Focus lives on the viewport, so the delegate's filter (installed on the
  // QPlainTextEdit) never sees the FocusOut from setFocus. Deliver it after
  // focus has left the editor. The view commits while the editor is still
  // registered, and the current cell stays put.
  table->setFocus(Qt::OtherFocusReason);
  if (open == nullptr) {
    return;
  }
  QFocusEvent out(QEvent::FocusOut, Qt::OtherFocusReason);
  QCoreApplication::sendEvent(open, &out);
}

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
  qint64 selectedCust = customerForReport(store, r, isNew);
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
  // The first fill uses the stored lines. Later fills, including a customer
  // change, keep whatever is on screen and move it onto the new projects.
  bool commentsReady = false;
  auto reloadProjects = [&] {
    const qint64 custId = custCombo->currentData().toLongLong();
    Report shown = r;
    qint64 anchor = anchorId;
    if (commentsReady) {
      const int current = comments->currentRow();
      if (current >= 0 && comments->item(current, 0)) {
        anchor = comments->item(current, 0)->data(Qt::UserRole).toLongLong();
      }
      shown = commentsForCustomer(store, custId,
                                  notesFromCommentTable(comments), anchor);
      anchor = shown.projectId;
    }
    fillCommentTable(comments, store, custId, shown, anchor);
    commentsReady = true;
  };
  reloadProjects();
  QObject::connect(custCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                   &dlg, [&](int) { reloadProjects(); });
  auto *dtStart = ui.startEdit;
  auto *dtStop = ui.stopEdit;
  {
    Report shown = r;
    if (!shown.stop) {
      shown.stop = r.start.addSecs(30 * 60);
    }
    alignStoredReport(shown);
    dtStart->setDateTime(shown.start.toLocalTime());
    dtStop->setDateTime(shown.stop->toLocalTime());
  }
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
    Report shown;
    shown.start = dtStart->dateTime();
    shown.stop = dtStop->dateTime();
    alignStoredReport(shown);
    if (dtStart->dateTime() != shown.start) {
      const QSignalBlocker block(dtStart);
      dtStart->setDateTime(shown.start);
    }
    if (shown.stop && dtStop->dateTime() != *shown.stop) {
      const QSignalBlocker block(dtStop);
      dtStop->setDateTime(*shown.stop);
    }
    len->setText(formatDurationHms(
        shown.start.msecsTo(shown.stop.value_or(shown.start))));
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
    commitCommentEditor(comments);
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
    Report aligned;
    aligned.start = dtStart->dateTime();
    aligned.stop = dtStop->dateTime();
    alignStoredReport(aligned);
    const QDateTime start = aligned.start;
    const QDateTime stop = aligned.stop.value_or(start);
    if (dtStart->dateTime() != start) {
      dtStart->setDateTime(start);
    }
    if (dtStop->dateTime() != stop) {
      dtStop->setDateTime(stop);
    }
    if (start.secsTo(stop) < 60) {
      QMessageBox::warning(&dlg, QStringLiteral("Save report"),
                           QStringLiteral("Report must be at least 1 minute."));
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
