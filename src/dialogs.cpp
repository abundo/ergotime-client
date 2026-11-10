#include "dialogs.hpp"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QVBoxLayout>

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
  table->setWordWrap(false);
  table->setTextElideMode(Qt::ElideRight);
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
  dlg.setWindowTitle(QStringLiteral("ErgoTime Settings"));
  dlg.resize(520, 360);
  auto *root = new QVBoxLayout(&dlg);
  auto *tabs = new QTabWidget;
  root->addWidget(tabs);

  auto *gen = new QWidget;
  auto *genForm = new QFormLayout(gen);
  auto *username = new QLineEdit(cfg.username);
  auto *password = new QLineEdit(cfg.password);
  password->setEchoMode(QLineEdit::Password);
  password->setPlaceholderText(QStringLiteral("Server password"));
  auto *idle = new QSpinBox;
  idle->setRange(0, 86400);
  idle->setValue(cfg.idleTimeoutSec);
  idle->setSuffix(QStringLiteral(" sec"));
  auto *dataDir = new QLineEdit(cfg.dataDir);
  dataDir->setReadOnly(true);
  genForm->addRow(QStringLiteral("Username"), username);
  genForm->addRow(QStringLiteral("Password"), password);
  genForm->addRow(QStringLiteral("Idle timeout"), idle);
  genForm->addRow(QStringLiteral("Data directory"), dataDir);
  tabs->addTab(gen, QStringLiteral("General"));

  auto *cat = new QWidget;
  auto *catForm = new QFormLayout(cat);
  auto *actSync = new QSpinBox;
  actSync->setRange(0, 86400);
  actSync->setValue(cfg.activitySyncIntervalSec);
  actSync->setSuffix(QStringLiteral(" sec"));
  actSync->setSpecialValueText(QStringLiteral("disabled"));
  catForm->addRow(QStringLiteral("Sync interval"), actSync);
  tabs->addTab(cat, QStringLiteral("Catalog"));

  auto *rep = new QWidget;
  auto *repForm = new QFormLayout(rep);
  auto *repSync = new QSpinBox;
  repSync->setRange(0, 86400);
  repSync->setValue(cfg.reportSyncIntervalSec);
  repSync->setSuffix(QStringLiteral(" sec"));
  repSync->setSpecialValueText(QStringLiteral("disabled"));
  repForm->addRow(QStringLiteral("Sync interval"), repSync);
  tabs->addTab(rep, QStringLiteral("Report"));

  auto *net = new QWidget;
  auto *netForm = new QFormLayout(net);
  auto *server = new QLineEdit(cfg.serverUrl);
  auto *netTo = new QSpinBox;
  netTo->setRange(1, 600);
  netTo->setValue(cfg.networkTimeoutSec);
  netTo->setSuffix(QStringLiteral(" sec"));
  netForm->addRow(QStringLiteral("Server URL"), server);
  netForm->addRow(QStringLiteral("Network timeout"), netTo);
  tabs->addTab(net, QStringLiteral("Network"));

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                   &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                   &QDialog::reject);
  root->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) {
    return std::nullopt;
  }
  Config out = cfg;
  out.username = username->text();
  out.password = password->text();
  out.idleTimeoutSec = idle->value();
  out.activitySyncIntervalSec = actSync->value();
  out.reportSyncIntervalSec = repSync->value();
  out.serverUrl = server->text();
  out.networkTimeoutSec = netTo->value();
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
  dlg.setWindowTitle(isNew ? QStringLiteral("Create report")
                           : QStringLiteral("Edit report"));
  dlg.resize(640, 560);
  auto *root = new QVBoxLayout(&dlg);
  auto *form = new QFormLayout;
  form->addRow(QStringLiteral("Sync state"), new QLabel(syncFlags(r)));

  auto *custCombo = new QComboBox;
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
  auto *comments = new QTableWidget(0, 2);
  comments->setHorizontalHeaderLabels(
      {QStringLiteral("Project"), QStringLiteral("Comment")});
  comments->horizontalHeader()->setSectionResizeMode(
      0, QHeaderView::ResizeToContents);
  comments->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  comments->verticalHeader()->setVisible(false);
  comments->setSelectionBehavior(QAbstractItemView::SelectRows);
  comments->setSelectionMode(QAbstractItemView::SingleSelection);
  comments->setMinimumHeight(160);
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
  form->addRow(QStringLiteral("Customer"), custCombo);

  auto *dtStart = new QDateTimeEdit(r.start.toLocalTime());
  auto *dtStop = new QDateTimeEdit(
      (r.stop ? *r.stop : r.start.addSecs(1800)).toLocalTime());
  for (auto *dt : {dtStart, dtStop}) {
    dt->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    dt->setCalendarPopup(true);
  }
  auto *len = new QLabel;
  auto updateLen = [&] {
    if (dtStop->dateTime() < dtStart->dateTime()) {
      dtStop->setDateTime(dtStart->dateTime());
    }
    if (dtStart->dateTime().secsTo(dtStop->dateTime()) > 24 * 3600) {
      dtStop->setDateTime(dtStart->dateTime().addSecs(24 * 3600));
    }
    len->setText(
        formatDurationHms(dtStart->dateTime().msecsTo(dtStop->dateTime())));
  };
  QObject::connect(dtStart, &QDateTimeEdit::dateTimeChanged, &dlg, updateLen);
  QObject::connect(dtStop, &QDateTimeEdit::dateTimeChanged, &dlg, updateLen);
  updateLen();

  auto nudgeRow = [&](QDateTimeEdit *primary, bool shiftBoth) {
    auto *row = new QHBoxLayout;
    row->addWidget(primary);
    const struct {
      const char *label;
      int secs;
    } buttons[] = {{"-H", -3600}, {"+H", 3600}, {"-M", -60}, {"+M", 60}};
    for (const auto &b : buttons) {
      auto *btn = new QPushButton(QString::fromUtf8(b.label));
      // Copy the editor pointers. primary is a parameter of this lambda, so a
      // reference capture would dangle after nudgeRow returns and crash on
      // click.
      QObject::connect(
          btn, &QPushButton::clicked, &dlg,
          [dtStart, dtStop, primary, secs = b.secs, shiftBoth, &updateLen] {
            const QDateTime start = dtStart->dateTime();
            const QDateTime stop = dtStop->dateTime();
            {
              const QSignalBlocker blockStart(dtStart);
              const QSignalBlocker blockStop(dtStop);
              if (shiftBoth) {
                dtStart->setDateTime(start.addSecs(secs));
                dtStop->setDateTime(stop.addSecs(secs));
              } else {
                primary->setDateTime(primary->dateTime().addSecs(secs));
              }
            }
            updateLen();
          });
      row->addWidget(btn);
    }
    return row;
  };
  form->addRow(QStringLiteral("Start"), nudgeRow(dtStart, true));
  form->addRow(QStringLiteral("Stop"), nudgeRow(dtStop, false));
  form->addRow(QStringLiteral("Length"), len);

  form->addRow(QStringLiteral("Comment"), comments);
  root->addLayout(form, 1);

  auto *save = new QPushButton(QStringLiteral("Save"));
  auto *del = new QPushButton(QStringLiteral("Delete"));
  auto *cancel = new QPushButton(QStringLiteral("Cancel"));
  del->setEnabled(!isNew);
  auto *btnRow = new QHBoxLayout;
  btnRow->addWidget(save);
  btnRow->addWidget(del);
  btnRow->addStretch();
  btnRow->addWidget(cancel);
  root->addLayout(btnRow);

  bool saved = false;
  QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
  QObject::connect(save, &QPushButton::clicked, &dlg, [&] {
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
    if (start.secsTo(stop) > 24 * 3600) {
      QMessageBox::warning(
          &dlg, QStringLiteral("Save report"),
          QStringLiteral("Report cannot be longer than 24 hours."));
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
  QObject::connect(del, &QPushButton::clicked, &dlg, [&] {
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
