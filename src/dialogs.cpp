#include "dialogs.hpp"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {

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

void fillProjects(QComboBox *proj, Store &store, qint64 customerId,
                  qint64 keepProject) {
  proj->clear();
  if (customerId <= 0) {
    return;
  }
  bool have = false;
  for (const Project &p : store.listProjects(customerId, true)) {
    proj->addItem(p.name, static_cast<qlonglong>(p.id));
    if (p.id == keepProject) {
      have = true;
    }
  }
  if (keepProject > 0 && !have) {
    if (auto p = store.getProject(keepProject);
        p && p->customerId == customerId) {
      proj->addItem(p->name, static_cast<qlonglong>(p->id));
    }
  }
  const int idx = proj->findData(static_cast<qlonglong>(keepProject));
  if (idx >= 0) {
    proj->setCurrentIndex(idx);
  }
}

} // namespace

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
  dlg.resize(480, 480);
  auto *root = new QVBoxLayout(&dlg);
  auto *form = new QFormLayout;
  form->addRow(QStringLiteral("Sync state"), new QLabel(syncFlags(r)));

  auto *custCombo = new QComboBox;
  auto *projCombo = new QComboBox;
  qint64 selectedCust = 0;
  if (r.projectId > 0) {
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
  auto reloadProjects = [&] {
    const qint64 custId = custCombo->currentData().toLongLong();
    fillProjects(projCombo, store, custId, r.projectId);
  };
  reloadProjects();
  QObject::connect(custCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                   &dlg, [&](int) {
                     r.projectId = 0;
                     reloadProjects();
                   });
  form->addRow(QStringLiteral("Customer"), custCombo);
  form->addRow(QStringLiteral("Project"), projCombo);

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

  auto nudgeRow = [&](QDateTimeEdit *primary, bool both) {
    auto *row = new QHBoxLayout;
    row->addWidget(primary);
    const struct {
      const char *label;
      int secs;
    } buttons[] = {{"-H", -3600}, {"+H", 3600}, {"-M", -60}, {"+M", 60}};
    for (const auto &b : buttons) {
      auto *btn = new QPushButton(QString::fromUtf8(b.label));
      QObject::connect(
          btn, &QPushButton::clicked, &dlg, [&, secs = b.secs, both] {
            dtStart->setDateTime(both ? dtStart->dateTime().addSecs(secs)
                                      : dtStart->dateTime());
            if (both) {
              dtStop->setDateTime(dtStop->dateTime().addSecs(secs));
            } else {
              primary->setDateTime(primary->dateTime().addSecs(secs));
            }
          });
      row->addWidget(btn);
    }
    return row;
  };
  form->addRow(QStringLiteral("Start"), nudgeRow(dtStart, true));
  form->addRow(QStringLiteral("Stop"), nudgeRow(dtStop, false));
  form->addRow(QStringLiteral("Length"), len);

  auto *comment = new QTextEdit;
  comment->setAcceptRichText(false);
  comment->setPlainText(r.comment);
  comment->setMinimumHeight(60);
  form->addRow(QStringLiteral("Comment"), comment);
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
    const qint64 projId = projCombo->currentData().toLongLong();
    if (projId <= 0) {
      QMessageBox::warning(&dlg, QStringLiteral("Save report"),
                           QStringLiteral("Please select a project."));
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
    r.projectId = projId;
    r.start = start;
    r.stop = stop;
    r.comment = comment->toPlainText();
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
