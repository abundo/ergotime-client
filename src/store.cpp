#include "store.hpp"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <stdexcept>

namespace {

constexpr const char *kReportCols =
    "_id, server_id, user_id, project_id, start, stop, comment, modified, seq, "
    "deleted, updated, client_uuid, conflict, customer_id";

[[noreturn]] void fail(const QSqlQuery &q, const QString &what) {
  throw std::runtime_error(
      (what + QStringLiteral(": ") + q.lastError().text()).toStdString());
}

int b(bool v) { return v ? 1 : 0; }

QVariant stopBind(const std::optional<QDateTime> &stop) {
  if (!stop || !stop->isValid()) {
    return QVariant();
  }
  return formatTime(*stop);
}

QVariant optDouble(const std::optional<double> &v) {
  if (!v) {
    return QVariant();
  }
  return *v;
}

std::optional<double> readDouble(const QVariant &v) {
  if (!v.isValid() || v.isNull()) {
    return std::nullopt;
  }
  return v.toDouble();
}

Customer readCustomer(QSqlQuery &q) {
  Customer c;
  c.id = q.value(0).toLongLong();
  c.serverId = q.value(1).toLongLong();
  c.name = q.value(2).toString();
  c.description = q.value(3).toString();
  c.active = q.value(4).toInt() != 0;
  return c;
}

Project readProject(QSqlQuery &q) {
  Project p;
  p.id = q.value(0).toLongLong();
  p.serverId = q.value(1).toLongLong();
  p.customerId = q.value(2).toLongLong();
  p.name = q.value(3).toString();
  p.costCenter = q.value(4).toString();
  p.hourlyRate = readDouble(q.value(5));
  p.kmRate = readDouble(q.value(6));
  p.active = q.value(7).toInt() != 0;
  p.isDefault = q.value(8).toInt() != 0;
  return p;
}

Report readReport(QSqlQuery &q) {
  Report r;
  r.id = q.value(0).toLongLong();
  r.serverId = q.value(1).toLongLong();
  r.userId = q.value(2).toLongLong();
  r.projectId = q.value(3).toLongLong();
  r.start = parseTime(q.value(4).toString());
  const QString stop = q.value(5).toString();
  if (!stop.isEmpty()) {
    r.stop = parseTime(stop);
  }
  r.comment = q.value(6).toString();
  r.modified = parseTime(q.value(7).toString());
  r.seq = q.value(8).toLongLong();
  r.deleted = q.value(9).toInt() != 0;
  r.updated = q.value(10).toInt() != 0;
  r.clientUuid = q.value(11).toString();
  r.conflict = q.value(12).toInt() != 0;
  r.customerId = q.value(13).toLongLong();
  return r;
}

QString sqlText(const QString &s);

void loadLines(QSqlDatabase db, Report &r) {
  r.lines.clear();
  if (r.id > 0) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT project_id, comment FROM report_line WHERE report_id = ? "
        "ORDER BY position, _id"));
    q.addBindValue(r.id);
    if (!q.exec()) {
      fail(q, QStringLiteral("load report lines"));
    }
    while (q.next()) {
      ReportLine ln;
      ln.projectId = q.value(0).toLongLong();
      ln.comment = q.value(1).toString();
      r.lines.push_back(ln);
    }
  }
  if (r.lines.empty() && r.projectId > 0) {
    ReportLine ln;
    ln.projectId = r.projectId;
    ln.comment = r.comment;
    r.lines.push_back(ln);
  }
}

void writeLines(QSqlDatabase db, const Report &r) {
  if (r.id <= 0) {
    return;
  }
  QSqlQuery del(db);
  del.prepare(QStringLiteral("DELETE FROM report_line WHERE report_id = ?"));
  del.addBindValue(r.id);
  if (!del.exec()) {
    fail(del, QStringLiteral("clear report lines"));
  }
  std::vector<ReportLine> lines = r.lines;
  if (lines.empty() && r.projectId > 0) {
    ReportLine ln;
    ln.projectId = r.projectId;
    ln.comment = r.comment;
    lines.push_back(ln);
  }
  for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO report_line (report_id, project_id, comment, position) "
        "VALUES (?, ?, ?, ?)"));
    ins.addBindValue(r.id);
    ins.addBindValue(lines[static_cast<size_t>(i)].projectId);
    ins.addBindValue(sqlText(lines[static_cast<size_t>(i)].comment));
    ins.addBindValue(i);
    if (!ins.exec()) {
      fail(ins, QStringLiteral("insert report line"));
    }
  }
}

QString newClientUuid() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void ensureKeepTable(QSqlDatabase db) {
  QSqlQuery q(db);
  if (!q.exec(
          QStringLiteral("CREATE TEMP TABLE IF NOT EXISTS sync_keep_server_id ("
                         "id INTEGER PRIMARY KEY)"))) {
    fail(q, QStringLiteral("keep server ids"));
  }
}

void fillKeepTable(QSqlDatabase db, const std::vector<qint64> &keep) {
  ensureKeepTable(db);
  QSqlQuery clear(db);
  if (!clear.exec(QStringLiteral("DELETE FROM sync_keep_server_id"))) {
    fail(clear, QStringLiteral("clear server ids"));
  }
  QSqlQuery ins(db);
  ins.prepare(QStringLiteral("INSERT OR IGNORE INTO sync_keep_server_id (id) "
                             "VALUES (?)"));
  for (qint64 id : keep) {
    if (id <= 0) {
      continue;
    }
    ins.bindValue(0, id);
    if (!ins.exec()) {
      fail(ins, QStringLiteral("insert server id"));
    }
  }
}

struct DoomedProject {
  qint64 id = 0;
  qint64 customerId = 0;
  QString name;
};

QString doomedName(const std::vector<DoomedProject> &doomed, qint64 id) {
  for (const DoomedProject &d : doomed) {
    if (d.id == id && !d.name.isEmpty()) {
      return d.name;
    }
  }
  return QStringLiteral("Project #%1").arg(id);
}

bool idIn(const std::vector<qint64> &ids, qint64 id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Lines on a project the server no longer has cannot be pushed. Keep the
// note on a project that remains, so the interval still syncs.
void retargetReportsOffProjects(QSqlDatabase db,
                                const std::vector<DoomedProject> &doomed) {
  if (doomed.empty()) {
    return;
  }
  std::vector<qint64> doomedIds;
  doomedIds.reserve(doomed.size());
  for (const DoomedProject &d : doomed) {
    doomedIds.push_back(d.id);
  }
  QSqlQuery ids(db);
  if (!ids.exec(QStringLiteral(
          "SELECT DISTINCT r._id FROM report r "
          "WHERE r.project_id IN (SELECT _id FROM sync_drop_project) "
          "OR r._id IN (SELECT report_id FROM report_line "
          "WHERE project_id IN (SELECT _id FROM sync_drop_project))"))) {
    fail(ids, QStringLiteral("reports on removed projects"));
  }
  std::vector<qint64> reportIds;
  while (ids.next()) {
    reportIds.push_back(ids.value(0).toLongLong());
  }
  std::vector<Project> projects;
  QSqlQuery all(db);
  if (!all.exec(QStringLiteral(
          "SELECT _id, server_id, customer_id, name, costcenter, hourly_rate, "
          "km_rate, active, is_default FROM project"))) {
    fail(all, QStringLiteral("projects for retarget"));
  }
  while (all.next()) {
    projects.push_back(readProject(all));
  }
  for (qint64 reportId : reportIds) {
    QSqlQuery one(db);
    one.prepare(QStringLiteral("SELECT ") + QLatin1String(kReportCols) +
                QStringLiteral(" FROM report WHERE _id = ?"));
    one.addBindValue(reportId);
    if (!one.exec()) {
      fail(one, QStringLiteral("load report for retarget"));
    }
    if (!one.next()) {
      continue;
    }
    Report r = readReport(one);
    loadLines(db, r);
    std::vector<ReportLine> kept;
    QString extra;
    for (const ReportLine &ln : r.lines) {
      if (!idIn(doomedIds, ln.projectId)) {
        kept.push_back(ln);
        continue;
      }
      if (!extra.isEmpty()) {
        extra += QStringLiteral("\n\n");
      }
      extra += doomedName(doomed, ln.projectId);
      if (!ln.comment.trimmed().isEmpty()) {
        extra += QLatin1Char('\n');
        extra += ln.comment;
      }
    }
    qint64 customerId = r.customerId;
    if (kept.empty()) {
      if (customerId <= 0) {
        for (const ReportLine &ln : r.lines) {
          for (const DoomedProject &d : doomed) {
            if (d.id == ln.projectId && d.customerId > 0) {
              customerId = d.customerId;
            }
          }
        }
      }
      QSqlQuery fallback(db);
      fallback.prepare(QStringLiteral(
          "SELECT _id, customer_id FROM project "
          "WHERE customer_id = ? AND _id NOT IN (SELECT _id FROM "
          "sync_drop_project) "
          "ORDER BY is_default DESC, active DESC, name COLLATE NOCASE "
          "LIMIT 1"));
      fallback.addBindValue(customerId);
      if (!fallback.exec()) {
        fail(fallback, QStringLiteral("fallback project"));
      }
      if (fallback.next()) {
        ReportLine ln;
        ln.projectId = fallback.value(0).toLongLong();
        ln.comment = r.comment;
        kept.push_back(ln);
        customerId = fallback.value(1).toLongLong();
        extra.clear();
      }
    }
    if (!extra.isEmpty() && !kept.empty()) {
      if (kept[0].comment.trimmed().isEmpty()) {
        kept[0].comment = extra;
      } else {
        kept[0].comment += QStringLiteral("\n\n");
        kept[0].comment += extra;
      }
    }
    if (kept.empty()) {
      QSqlQuery clear(db);
      clear.prepare(
          QStringLiteral("DELETE FROM report_line WHERE report_id = ?"));
      clear.addBindValue(r.id);
      if (!clear.exec()) {
        fail(clear, QStringLiteral("clear orphan report lines"));
      }
      QSqlQuery up(db);
      up.prepare(QStringLiteral(
          "UPDATE report SET project_id = -1, updated = CASE WHEN deleted = 0 "
          "THEN 1 ELSE updated END WHERE _id = ?"));
      up.addBindValue(r.id);
      if (!up.exec()) {
        fail(up, QStringLiteral("clear orphan report project"));
      }
      continue;
    }
    bool anchorKept = false;
    for (const ReportLine &ln : kept) {
      if (ln.projectId == r.projectId) {
        anchorKept = true;
        break;
      }
    }
    if (!anchorKept) {
      r.projectId = kept[0].projectId;
    }
    for (const Project &p : projects) {
      if (p.id == r.projectId) {
        customerId = p.customerId;
        break;
      }
    }
    r.customerId = customerId;
    r.lines = kept;
    r.comment = formatLinesComment(kept, projects);
    if (!r.deleted) {
      r.updated = true;
    }
    QSqlQuery up(db);
    up.prepare(QStringLiteral(
        "UPDATE report SET project_id=?, customer_id=?, comment=?, updated=? "
        "WHERE _id=?"));
    up.addBindValue(r.projectId);
    up.addBindValue(r.customerId);
    up.addBindValue(sqlText(r.comment));
    up.addBindValue(b(r.updated));
    up.addBindValue(r.id);
    if (!up.exec()) {
      fail(up, QStringLiteral("retarget report"));
    }
    writeLines(db, r);
  }
}

// Qt binds a null QString as SQL NULL. An explicit NULL ignores DEFAULT ''
// and fails NOT NULL (blank comments, missing JSON fields).
QString sqlText(const QString &s) {
  return s.isNull() ? QStringLiteral("") : s;
}

} // namespace

class Store::Conn {
public:
  QSqlDatabase db;
  QSqlQuery exec(const QString &sql) {
    QSqlQuery q(db);
    if (!q.exec(sql)) {
      fail(q, QStringLiteral("sql"));
    }
    return q;
  }
};

Store::Store(const QString &path) : path_(path) { connection(); }

Store::~Store() {
  const QString name =
      QStringLiteral("ergotime-%1")
          .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
  if (QSqlDatabase::contains(name)) {
    QSqlDatabase::database(name).close();
    QSqlDatabase::removeDatabase(name);
  }
}

namespace {

void migrateOn(QSqlDatabase db) {
  auto exec = [&](const QString &sql) {
    QSqlQuery q(db);
    if (!q.exec(sql)) {
      fail(q, sql.left(80));
    }
  };

  QSqlQuery act(db);
  bool cutover = false;
  if (act.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE "
                              "type='table' AND name='activity'")) &&
      act.next() && act.value(0).toInt() > 0) {
    cutover = true;
  }
  if (!cutover) {
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(report)"))) {
      bool hasProject = false;
      bool hasActivity = false;
      bool any = false;
      while (info.next()) {
        any = true;
        const QString name = info.value(1).toString();
        if (name == QLatin1String("project_id")) {
          hasProject = true;
        }
        if (name == QLatin1String("activityid")) {
          hasActivity = true;
        }
      }
      if (any && hasActivity && !hasProject) {
        cutover = true;
      }
    }
  }
  if (cutover) {
    for (const char *table : {"report", "activity", "project", "customer"}) {
      exec(QStringLiteral("DROP TABLE IF EXISTS %1").arg(QLatin1String(table)));
    }
  }

  const QStringList stmts = {
      QStringLiteral("CREATE TABLE IF NOT EXISTS customer ("
                     "_id INTEGER PRIMARY KEY, server_id INTEGER NOT NULL "
                     "DEFAULT -1, name TEXT NOT NULL DEFAULT '', "
                     "description TEXT NOT NULL DEFAULT '', active INTEGER NOT "
                     "NULL DEFAULT 0)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS project ("
                     "_id INTEGER PRIMARY KEY, server_id INTEGER NOT NULL "
                     "DEFAULT -1, customer_id INTEGER NOT NULL DEFAULT -1, "
                     "name TEXT NOT NULL DEFAULT '', costcenter TEXT NOT NULL "
                     "DEFAULT '', hourly_rate REAL, km_rate REAL, "
                     "active INTEGER NOT NULL DEFAULT 0, "
                     "is_default INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS report ("
                     "_id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL "
                     "DEFAULT -1, project_id INTEGER NOT NULL DEFAULT -1, "
                     "start TEXT NOT NULL, stop TEXT, comment TEXT NOT NULL "
                     "DEFAULT '', modified TEXT NOT NULL, "
                     "seq INTEGER NOT NULL DEFAULT -1, deleted INTEGER NOT "
                     "NULL DEFAULT 0, server_id INTEGER NOT NULL DEFAULT -1, "
                     "updated INTEGER NOT NULL DEFAULT 0, client_uuid TEXT NOT "
                     "NULL DEFAULT '', conflict INTEGER NOT NULL DEFAULT 0, "
                     "customer_id INTEGER NOT NULL DEFAULT -1)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS report_line ("
                     "_id INTEGER PRIMARY KEY, report_id INTEGER NOT NULL, "
                     "project_id INTEGER NOT NULL, comment TEXT NOT NULL "
                     "DEFAULT '', position INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS sync_state ("
                     "id INTEGER PRIMARY KEY CHECK (id = 1), last_seq INTEGER "
                     "NOT NULL DEFAULT 0, last_ok INTEGER NOT NULL DEFAULT 0, "
                     "last_message TEXT NOT NULL DEFAULT '', last_sync_at TEXT "
                     "NOT NULL DEFAULT '')"),
      QStringLiteral("INSERT OR IGNORE INTO sync_state (id, last_seq, last_ok, "
                     "last_message, last_sync_at) VALUES (1, 0, 0, '', '')"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_report_start ON report(start)"),
      QStringLiteral(
          "CREATE INDEX IF NOT EXISTS idx_report_updated ON report(updated)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_report_server_id ON "
                     "report(server_id)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_project_customer ON "
                     "project(customer_id)"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_report_client_uuid ON "
                     "report(client_uuid)"),
      QStringLiteral(
          "CREATE UNIQUE INDEX IF NOT EXISTS idx_report_server_id_unique ON "
          "report(server_id) WHERE server_id > 0"),
      QStringLiteral(
          "CREATE UNIQUE INDEX IF NOT EXISTS idx_report_client_uuid_unique ON "
          "report(client_uuid) WHERE client_uuid != ''"),
      QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_customer_server_id "
                     "ON customer(server_id) WHERE server_id > 0"),
      QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_project_server_id "
                     "ON project(server_id) WHERE server_id > 0"),
      QStringLiteral("CREATE INDEX IF NOT EXISTS idx_report_line_report ON "
                     "report_line(report_id)"),
  };
  for (const QString &sql : stmts) {
    exec(sql);
  }

  auto hasColumn = [&](const QString &table, const QString &column) {
    QSqlQuery info(db);
    if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
      return false;
    }
    while (info.next()) {
      if (info.value(1).toString() == column) {
        return true;
      }
    }
    return false;
  };
  if (!hasColumn(QStringLiteral("project"), QStringLiteral("is_default"))) {
    exec(QStringLiteral("ALTER TABLE project ADD COLUMN is_default INTEGER NOT "
                        "NULL DEFAULT 0"));
  }
  if (!hasColumn(QStringLiteral("report"), QStringLiteral("customer_id"))) {
    exec(QStringLiteral("ALTER TABLE report ADD COLUMN customer_id INTEGER NOT "
                        "NULL DEFAULT -1"));
  }
  exec(QStringLiteral(
      "UPDATE report SET customer_id = (SELECT customer_id FROM project "
      "WHERE project._id = report.project_id) "
      "WHERE customer_id IS NULL OR customer_id <= 0"));
  exec(QStringLiteral(
      "INSERT INTO report_line (report_id, project_id, comment, position) "
      "SELECT r._id, r.project_id, r.comment, 0 FROM report r "
      "WHERE r.project_id > 0 AND NOT EXISTS ("
      "SELECT 1 FROM report_line l WHERE l.report_id = r._id)"));

  QSqlQuery seq(db);
  exec(QStringLiteral("SELECT last_seq FROM sync_state WHERE id = 1"));
  // last exec consumed; query again
  QSqlQuery last(db);
  if (!last.exec(
          QStringLiteral("SELECT last_seq FROM sync_state WHERE id = 1")) ||
      !last.next()) {
    return;
  }
  if (last.value(0).toLongLong() > 0) {
    return;
  }
  QSqlQuery mx(db);
  if (!mx.exec(QStringLiteral("SELECT MAX(seq) FROM report")) || !mx.next()) {
    return;
  }
  const qint64 maxSeq = mx.value(0).toLongLong();
  if (maxSeq <= 0) {
    return;
  }
  QSqlQuery up(db);
  up.prepare(QStringLiteral(
      "UPDATE sync_state SET last_seq = ? WHERE id = 1 AND last_seq < ?"));
  up.addBindValue(maxSeq);
  up.addBindValue(maxSeq);
  if (!up.exec()) {
    fail(up, QStringLiteral("bootstrap last_seq"));
  }
  Q_UNUSED(seq);
}

} // namespace

Store::Conn Store::connection() {
  const QString name =
      QStringLiteral("ergotime-%1")
          .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
  if (!QSqlDatabase::contains(name)) {
    QSqlDatabase db =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(path_);
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!db.open()) {
      throw std::runtime_error(
          ("open sqlite: " + db.lastError().text()).toStdString());
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    q.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
  }
  Conn c;
  c.db = QSqlDatabase::database(name);
  std::call_once(migrated_, [&] { migrateOn(c.db); });
  return c;
}

void Store::migrate() {}

bool Store::needsSchemaCutover(Conn &) { return false; }

std::vector<Customer> Store::listCustomers(bool activeOnly) {
  Conn c = connection();
  QString sql = QStringLiteral(
      "SELECT _id, server_id, name, description, active FROM customer");
  if (activeOnly) {
    sql += QStringLiteral(" WHERE active = 1");
  }
  sql += QStringLiteral(" ORDER BY name COLLATE NOCASE");
  QSqlQuery q(c.db);
  if (!q.exec(sql)) {
    fail(q, QStringLiteral("list customers"));
  }
  std::vector<Customer> out;
  while (q.next()) {
    out.push_back(readCustomer(q));
  }
  return out;
}

std::optional<Customer> Store::getCustomer(qint64 id) {
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT _id, server_id, name, description, active "
                           "FROM customer WHERE _id = ?"));
  q.addBindValue(id);
  if (!q.exec()) {
    fail(q, QStringLiteral("get customer"));
  }
  if (!q.next()) {
    return std::nullopt;
  }
  return readCustomer(q);
}

std::optional<Customer> Store::getCustomerByServerId(qint64 serverId) {
  if (serverId <= 0) {
    return std::nullopt;
  }
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT _id, server_id, name, description, active "
                           "FROM customer WHERE server_id = ?"));
  q.addBindValue(serverId);
  if (!q.exec()) {
    fail(q, QStringLiteral("get customer by server"));
  }
  if (!q.next()) {
    return std::nullopt;
  }
  return readCustomer(q);
}

void Store::upsertCustomerByServerId(const Customer &cust) {
  if (cust.serverId <= 0) {
    throw std::runtime_error("server_id required for upsert");
  }
  Conn c = connection();
  QSqlQuery find(c.db);
  find.prepare(QStringLiteral("SELECT _id FROM customer WHERE server_id = ?"));
  find.addBindValue(cust.serverId);
  if (!find.exec()) {
    fail(find, QStringLiteral("find customer"));
  }
  if (!find.next()) {
    QSqlQuery ins(c.db);
    ins.prepare(QStringLiteral("INSERT INTO customer (server_id, name, "
                               "description, active) VALUES (?, ?, ?, ?)"));
    ins.addBindValue(cust.serverId);
    ins.addBindValue(cust.name);
    ins.addBindValue(cust.description);
    ins.addBindValue(b(cust.active));
    if (!ins.exec()) {
      fail(ins, QStringLiteral("insert customer"));
    }
    return;
  }
  QSqlQuery up(c.db);
  up.prepare(QStringLiteral(
      "UPDATE customer SET name=?, description=?, active=? WHERE _id=?"));
  up.addBindValue(cust.name);
  up.addBindValue(cust.description);
  up.addBindValue(b(cust.active));
  up.addBindValue(find.value(0).toLongLong());
  if (!up.exec()) {
    fail(up, QStringLiteral("update customer"));
  }
}

qint64 Store::deleteCustomersExceptServerIds(const std::vector<qint64> &keep) {
  Conn c = connection();
  if (!c.db.transaction()) {
    throw std::runtime_error("begin customer prune");
  }
  try {
    fillKeepTable(c.db, keep);
    QSqlQuery q(c.db);
    if (!q.exec(QStringLiteral("DELETE FROM customer WHERE server_id NOT IN "
                               "(SELECT id FROM sync_keep_server_id)"))) {
      fail(q, QStringLiteral("prune customers"));
    }
    const qint64 n = q.numRowsAffected();
    if (!c.db.commit()) {
      throw std::runtime_error("commit customer prune");
    }
    return n;
  } catch (...) {
    c.db.rollback();
    throw;
  }
}

std::vector<Project> Store::listProjects(qint64 customerId, bool activeOnly) {
  Conn c = connection();
  QString sql =
      QStringLiteral("SELECT _id, server_id, customer_id, name, costcenter, "
                     "hourly_rate, km_rate, active, is_default FROM project "
                     "WHERE 1=1");
  if (customerId > 0) {
    sql += QStringLiteral(" AND customer_id = ?");
  }
  if (activeOnly) {
    sql += QStringLiteral(" AND active = 1");
  }
  sql += QStringLiteral(" ORDER BY is_default DESC, name COLLATE NOCASE");
  QSqlQuery q(c.db);
  q.prepare(sql);
  if (customerId > 0) {
    q.addBindValue(customerId);
  }
  if (!q.exec()) {
    fail(q, QStringLiteral("list projects"));
  }
  std::vector<Project> out;
  while (q.next()) {
    out.push_back(readProject(q));
  }
  return out;
}

std::optional<Project> Store::getProject(qint64 id) {
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral(
      "SELECT _id, server_id, customer_id, name, costcenter, hourly_rate, "
      "km_rate, active, is_default FROM project WHERE _id = ?"));
  q.addBindValue(id);
  if (!q.exec()) {
    fail(q, QStringLiteral("get project"));
  }
  if (!q.next()) {
    return std::nullopt;
  }
  return readProject(q);
}

std::optional<Project> Store::getProjectByServerId(qint64 serverId) {
  if (serverId <= 0) {
    return std::nullopt;
  }
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral(
      "SELECT _id, server_id, customer_id, name, costcenter, hourly_rate, "
      "km_rate, active, is_default FROM project WHERE server_id = ?"));
  q.addBindValue(serverId);
  if (!q.exec()) {
    fail(q, QStringLiteral("get project by server"));
  }
  if (!q.next()) {
    return std::nullopt;
  }
  return readProject(q);
}

void Store::upsertProjectByServerId(const Project &p) {
  if (p.serverId <= 0) {
    throw std::runtime_error("server_id required for upsert");
  }
  Conn c = connection();
  QSqlQuery find(c.db);
  find.prepare(QStringLiteral("SELECT _id FROM project WHERE server_id = ?"));
  find.addBindValue(p.serverId);
  if (!find.exec()) {
    fail(find, QStringLiteral("find project"));
  }
  if (!find.next()) {
    QSqlQuery ins(c.db);
    ins.prepare(QStringLiteral(
        "INSERT INTO project (server_id, customer_id, name, costcenter, "
        "hourly_rate, km_rate, active, is_default) VALUES (?, ?, ?, ?, ?, ?, "
        "?, ?)"));
    ins.addBindValue(p.serverId);
    ins.addBindValue(p.customerId);
    ins.addBindValue(p.name);
    ins.addBindValue(p.costCenter);
    ins.addBindValue(optDouble(p.hourlyRate));
    ins.addBindValue(optDouble(p.kmRate));
    ins.addBindValue(b(p.active));
    ins.addBindValue(b(p.isDefault));
    if (!ins.exec()) {
      fail(ins, QStringLiteral("insert project"));
    }
    return;
  }
  QSqlQuery up(c.db);
  up.prepare(QStringLiteral(
      "UPDATE project SET customer_id=?, name=?, costcenter=?, "
      "hourly_rate=?, km_rate=?, active=?, is_default=? WHERE _id=?"));
  up.addBindValue(p.customerId);
  up.addBindValue(p.name);
  up.addBindValue(p.costCenter);
  up.addBindValue(optDouble(p.hourlyRate));
  up.addBindValue(optDouble(p.kmRate));
  up.addBindValue(b(p.active));
  up.addBindValue(b(p.isDefault));
  up.addBindValue(find.value(0).toLongLong());
  if (!up.exec()) {
    fail(up, QStringLiteral("update project"));
  }
}

qint64 Store::deleteProjectsExceptServerIds(const std::vector<qint64> &keep) {
  Conn c = connection();
  QSqlQuery drop(c.db);
  if (!drop.exec(QStringLiteral(
          "CREATE TEMP TABLE IF NOT EXISTS sync_drop_project ("
          "_id INTEGER PRIMARY KEY, customer_id INTEGER, name TEXT)"))) {
    fail(drop, QStringLiteral("drop project table"));
  }
  if (!c.db.transaction()) {
    throw std::runtime_error("begin project prune");
  }
  try {
    fillKeepTable(c.db, keep);
    QSqlQuery clear(c.db);
    if (!clear.exec(QStringLiteral("DELETE FROM sync_drop_project"))) {
      fail(clear, QStringLiteral("clear dropped projects"));
    }
    QSqlQuery sel(c.db);
    if (!sel.exec(QStringLiteral(
            "INSERT INTO sync_drop_project (_id, customer_id, name) "
            "SELECT _id, customer_id, name FROM project "
            "WHERE server_id NOT IN (SELECT id FROM sync_keep_server_id) "
            "OR customer_id NOT IN (SELECT _id FROM customer)"))) {
      fail(sel, QStringLiteral("select removed projects"));
    }
    QSqlQuery list(c.db);
    if (!list.exec(QStringLiteral(
            "SELECT _id, customer_id, name FROM sync_drop_project"))) {
      fail(list, QStringLiteral("list removed projects"));
    }
    std::vector<DoomedProject> doomed;
    while (list.next()) {
      DoomedProject d;
      d.id = list.value(0).toLongLong();
      d.customerId = list.value(1).toLongLong();
      d.name = list.value(2).toString();
      doomed.push_back(d);
    }
    retargetReportsOffProjects(c.db, doomed);
    QSqlQuery q(c.db);
    if (!q.exec(QStringLiteral("DELETE FROM project WHERE _id IN "
                               "(SELECT _id FROM sync_drop_project)"))) {
      fail(q, QStringLiteral("prune projects"));
    }
    const qint64 n = q.numRowsAffected();
    if (!c.db.commit()) {
      throw std::runtime_error("commit project prune");
    }
    return n;
  } catch (...) {
    c.db.rollback();
    throw;
  }
}

qint64 Store::serverProjectId(qint64 localId) {
  if (localId <= 0) {
    throw std::runtime_error("invalid local project id");
  }
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT server_id FROM project WHERE _id = ?"));
  q.addBindValue(localId);
  if (!q.exec()) {
    fail(q, QStringLiteral("server project id"));
  }
  if (!q.next()) {
    throw std::runtime_error("project not found");
  }
  const qint64 serverId = q.value(0).toLongLong();
  if (serverId <= 0) {
    throw std::runtime_error("project has no server_id (sync catalog first)");
  }
  return serverId;
}

std::vector<std::pair<qint64, QString>> Store::projectNames() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral("SELECT _id, name FROM project"))) {
    fail(q, QStringLiteral("project names"));
  }
  std::vector<std::pair<qint64, QString>> out;
  while (q.next()) {
    out.emplace_back(q.value(0).toLongLong(), q.value(1).toString());
  }
  return out;
}

std::vector<std::pair<qint64, QString>> Store::customerNames() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral("SELECT _id, name FROM customer"))) {
    fail(q, QStringLiteral("customer names"));
  }
  std::vector<std::pair<qint64, QString>> out;
  while (q.next()) {
    out.emplace_back(q.value(0).toLongLong(), q.value(1).toString());
  }
  return out;
}

qint64 Store::projectCustomerLocalId(qint64 projectId) {
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT customer_id FROM project WHERE _id = ?"));
  q.addBindValue(projectId);
  if (!q.exec()) {
    fail(q, QStringLiteral("project customer"));
  }
  if (!q.next()) {
    return 0;
  }
  return q.value(0).toLongLong();
}

std::optional<Report> Store::getReport(qint64 id) {
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT ") + QLatin1String(kReportCols) +
            QStringLiteral(" FROM report WHERE _id = ?"));
  q.addBindValue(id);
  if (!q.exec()) {
    fail(q, QStringLiteral("get report"));
  }
  if (!q.next()) {
    return std::nullopt;
  }
  Report r = readReport(q);
  loadLines(c.db, r);
  return r;
}

void Store::deleteReport(qint64 id) {
  auto r = getReport(id);
  if (!r) {
    throw std::runtime_error("report not found");
  }
  if (r->serverId > 0) {
    r->deleted = true;
    saveReport(*r);
    return;
  }
  hardDeleteReport(id);
}

void Store::saveReport(Report &r) {
  Conn c = connection();
  // A delete keeps the stored instants. The server removes the row by id.
  if (!r.deleted) {
    alignStoredReport(r);
  }
  if (r.id == 0) {
    if (!r.modified.isValid()) {
      r.modified = QDateTime::currentDateTimeUtc();
    }
    if (r.serverId == 0) {
      r.serverId = -1;
    }
    if (r.clientUuid.isEmpty()) {
      r.clientUuid = newClientUuid();
    }
    r.updated = true;
    r.conflict = false;
    r.comment = sqlText(r.comment);
    QSqlQuery q(c.db);
    q.prepare(QStringLiteral(
        "INSERT INTO report (user_id, project_id, start, stop, comment, "
        "modified, seq, deleted, server_id, updated, client_uuid, conflict, "
        "customer_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(r.userId);
    q.addBindValue(r.projectId);
    q.addBindValue(formatTime(r.start));
    q.addBindValue(stopBind(r.stop));
    q.addBindValue(r.comment);
    q.addBindValue(formatTime(r.modified));
    q.addBindValue(r.seq);
    q.addBindValue(b(r.deleted));
    q.addBindValue(r.serverId);
    q.addBindValue(b(r.updated));
    q.addBindValue(r.clientUuid);
    q.addBindValue(b(r.conflict));
    q.addBindValue(r.customerId);
    if (!q.exec()) {
      fail(q, QStringLiteral("insert report"));
    }
    r.id = q.lastInsertId().toLongLong();
    writeLines(c.db, r);
    return;
  }
  r.modified = QDateTime::currentDateTimeUtc();
  r.updated = true;
  r.conflict = false;
  r.comment = sqlText(r.comment);
  QSqlQuery q(c.db);
  // A running report is saved again while sync may have just stored a server
  // id on this row. An empty identity must not wipe that link.
  q.prepare(QStringLiteral(
      "UPDATE report SET "
      "user_id=CASE WHEN ? > 0 THEN ? ELSE user_id END, "
      "project_id=?, start=?, stop=?, comment=?, modified=?, "
      "seq=CASE WHEN ? > 0 THEN ? ELSE seq END, "
      "deleted=?, "
      "server_id=CASE WHEN ? > 0 THEN ? ELSE server_id END, "
      "updated=?, "
      "client_uuid=CASE WHEN ? != '' THEN ? ELSE client_uuid END, "
      "conflict=?, customer_id=? WHERE _id=?"));
  q.addBindValue(r.userId);
  q.addBindValue(r.userId);
  q.addBindValue(r.projectId);
  q.addBindValue(formatTime(r.start));
  q.addBindValue(stopBind(r.stop));
  q.addBindValue(r.comment);
  q.addBindValue(formatTime(r.modified));
  q.addBindValue(r.seq);
  q.addBindValue(r.seq);
  q.addBindValue(b(r.deleted));
  q.addBindValue(r.serverId);
  q.addBindValue(r.serverId);
  q.addBindValue(b(r.updated));
  q.addBindValue(r.clientUuid);
  q.addBindValue(r.clientUuid);
  q.addBindValue(b(r.conflict));
  q.addBindValue(r.customerId);
  q.addBindValue(r.id);
  if (!q.exec()) {
    fail(q, QStringLiteral("update report"));
  }
  QSqlQuery back(c.db);
  back.prepare(QStringLiteral(
      "SELECT server_id, seq, user_id, client_uuid FROM report WHERE _id=?"));
  back.addBindValue(r.id);
  if (!back.exec()) {
    fail(back, QStringLiteral("read report identity"));
  }
  if (back.next()) {
    r.serverId = back.value(0).toLongLong();
    r.seq = back.value(1).toLongLong();
    r.userId = back.value(2).toLongLong();
    const QString uuid = back.value(3).toString();
    if (!uuid.isEmpty()) {
      r.clientUuid = uuid;
    }
  }
  writeLines(c.db, r);
}

std::vector<Report> Store::listReportsBetween(const QDateTime &from,
                                              const QDateTime &to) {
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT ") + QLatin1String(kReportCols) +
            QStringLiteral(" FROM report WHERE deleted = 0 AND start >= ? AND "
                           "start < ? ORDER BY start"));
  q.addBindValue(formatTime(from));
  q.addBindValue(formatTime(to));
  if (!q.exec()) {
    fail(q, QStringLiteral("list reports"));
  }
  std::vector<Report> out;
  while (q.next()) {
    out.push_back(readReport(q));
  }
  for (Report &r : out) {
    loadLines(c.db, r);
  }
  return out;
}

std::vector<PendingOp> Store::listPendingOps() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(
          QStringLiteral("SELECT ") + QLatin1String(kReportCols) +
          QStringLiteral(" FROM report WHERE (updated = 1 OR deleted = 1) AND "
                         "(stop IS NOT NULL AND stop != '' OR deleted = 1) "
                         "ORDER BY start"))) {
    fail(q, QStringLiteral("list unsynced"));
  }
  std::vector<Report> rows;
  while (q.next()) {
    rows.push_back(readReport(q));
  }
  std::vector<PendingOp> deletes, creates, updates;
  for (Report &r : rows) {
    loadLines(c.db, r);
    if (r.deleted && r.serverId > 0) {
      deletes.push_back(PendingOp{OpKind::Delete, r});
    } else if (!r.deleted && r.serverId <= 0 && r.stop) {
      creates.push_back(PendingOp{OpKind::Create, r});
    } else if (!r.deleted && r.serverId > 0 && r.updated && r.stop) {
      updates.push_back(PendingOp{OpKind::Update, r});
    }
  }
  deletes.insert(deletes.end(), creates.begin(), creates.end());
  deletes.insert(deletes.end(), updates.begin(), updates.end());
  return deletes;
}

int Store::countUnsynced() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral(
          "SELECT COUNT(*) FROM report WHERE (updated = 1 OR deleted = 1) AND "
          "(stop IS NOT NULL AND stop != '' OR deleted = 1)")) ||
      !q.next()) {
    fail(q, QStringLiteral("count unsynced"));
  }
  return q.value(0).toInt();
}

std::optional<Report> Store::getReportByServerId(qint64 serverId) {
  if (serverId <= 0) {
    return std::nullopt;
  }
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT ") + QLatin1String(kReportCols) +
            QStringLiteral(" FROM report WHERE server_id = ?"));
  q.addBindValue(serverId);
  if (!q.exec()) {
    fail(q, QStringLiteral("report by server"));
  }
  if (!q.next()) {
    return std::nullopt;
  }
  Report r = readReport(q);
  loadLines(c.db, r);
  return r;
}

std::optional<Report> Store::getReportByClientUuid(const QString &uuid) {
  if (uuid.isEmpty()) {
    return std::nullopt;
  }
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("SELECT ") + QLatin1String(kReportCols) +
            QStringLiteral(" FROM report WHERE client_uuid = ?"));
  q.addBindValue(uuid);
  if (!q.exec()) {
    fail(q, QStringLiteral("report by uuid"));
  }
  if (!q.next()) {
    return std::nullopt;
  }
  Report r = readReport(q);
  loadLines(c.db, r);
  return r;
}

void Store::hardDeleteReport(qint64 id) {
  Conn c = connection();
  QSqlQuery lines(c.db);
  lines.prepare(QStringLiteral("DELETE FROM report_line WHERE report_id = ?"));
  lines.addBindValue(id);
  if (!lines.exec()) {
    fail(lines, QStringLiteral("delete report lines"));
  }
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("DELETE FROM report WHERE _id = ?"));
  q.addBindValue(id);
  if (!q.exec()) {
    fail(q, QStringLiteral("delete report"));
  }
}

void Store::markReportSynced(qint64 localId, qint64 serverId, qint64 seq,
                             qint64 userId, const QDateTime &modified,
                             const QDateTime &pushedModified) {
  if (localId == 0) {
    throw std::runtime_error("local id required");
  }
  QDateTime mod =
      modified.isValid() ? modified : QDateTime::currentDateTimeUtc();
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!pushedModified.isValid()) {
    q.prepare(QStringLiteral(
        "UPDATE report SET server_id=?, seq=?, user_id=?, modified=?, "
        "updated=0, deleted=0, conflict=0 WHERE _id=?"));
    q.addBindValue(serverId);
    q.addBindValue(seq);
    q.addBindValue(userId);
    q.addBindValue(formatTime(mod));
    q.addBindValue(localId);
  } else {
    const QString pushed = formatTime(pushedModified);
    q.prepare(QStringLiteral(
        "UPDATE report SET server_id=?, seq=?, user_id=?, "
        "modified=CASE WHEN modified=? THEN ? ELSE modified END, "
        "updated=CASE WHEN modified=? THEN 0 ELSE updated END, "
        "deleted=0, "
        "conflict=CASE WHEN modified=? THEN 0 ELSE conflict END "
        "WHERE _id=?"));
    q.addBindValue(serverId);
    q.addBindValue(seq);
    q.addBindValue(userId);
    q.addBindValue(pushed);
    q.addBindValue(formatTime(mod));
    q.addBindValue(pushed);
    q.addBindValue(pushed);
    q.addBindValue(localId);
  }
  if (!q.exec()) {
    fail(q, QStringLiteral("mark synced"));
  }
}

ApplyResult Store::applyServerReport(const Report &r) {
  if (r.serverId <= 0) {
    throw std::runtime_error("server_id required");
  }
  ApplyResult res;
  auto existing = getReportByServerId(r.serverId);
  if (!existing && !r.clientUuid.isEmpty()) {
    existing = getReportByClientUuid(r.clientUuid);
    if (existing) {
      res.linked = true;
    }
  }
  Conn c = connection();
  if (r.deleted) {
    if (existing) {
      hardDeleteReport(existing->id);
      res.action = QStringLiteral("delete");
    } else {
      res.action = QStringLiteral("noop");
    }
    return res;
  }
  if (existing && existing->updated) {
    QString cu = existing->clientUuid;
    if (cu.isEmpty()) {
      cu = r.clientUuid;
    }
    QSqlQuery q(c.db);
    q.prepare(QStringLiteral("UPDATE report SET server_id=?, seq=?, user_id=?, "
                             "client_uuid=?, conflict=1 WHERE _id=?"));
    q.addBindValue(r.serverId);
    q.addBindValue(r.seq);
    q.addBindValue(r.userId);
    q.addBindValue(sqlText(cu));
    q.addBindValue(existing->id);
    if (!q.exec()) {
      fail(q, QStringLiteral("keep dirty"));
    }
    res.action = QStringLiteral("skip_dirty");
    res.conflict = true;
    return res;
  }
  if (existing) {
    QString cu = r.clientUuid.isEmpty() ? existing->clientUuid : r.clientUuid;
    QSqlQuery q(c.db);
    q.prepare(QStringLiteral(
        "UPDATE report SET user_id=?, project_id=?, start=?, stop=?, "
        "comment=?, modified=?, seq=?, deleted=0, updated=0, "
        "server_id=?, client_uuid=?, conflict=0, customer_id=? WHERE _id=?"));
    q.addBindValue(r.userId);
    q.addBindValue(r.projectId);
    q.addBindValue(formatTime(r.start));
    q.addBindValue(stopBind(r.stop));
    q.addBindValue(sqlText(r.comment));
    q.addBindValue(formatTime(r.modified));
    q.addBindValue(r.seq);
    q.addBindValue(r.serverId);
    q.addBindValue(sqlText(cu));
    q.addBindValue(r.customerId);
    q.addBindValue(existing->id);
    if (!q.exec()) {
      fail(q, QStringLiteral("apply update"));
    }
    Report stored = r;
    stored.id = existing->id;
    writeLines(c.db, stored);
    res.action = res.linked ? QStringLiteral("link") : QStringLiteral("update");
    return res;
  }
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral(
      "INSERT INTO report (user_id, project_id, start, stop, comment, "
      "modified, seq, deleted, server_id, updated, client_uuid, conflict, "
      "customer_id) VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, 0, ?, 0, ?)"));
  q.addBindValue(r.userId);
  q.addBindValue(r.projectId);
  q.addBindValue(formatTime(r.start));
  q.addBindValue(stopBind(r.stop));
  q.addBindValue(sqlText(r.comment));
  q.addBindValue(formatTime(r.modified));
  q.addBindValue(r.seq);
  q.addBindValue(r.serverId);
  q.addBindValue(sqlText(r.clientUuid));
  q.addBindValue(r.customerId);
  if (!q.exec()) {
    fail(q, QStringLiteral("apply insert"));
  }
  Report stored = r;
  stored.id = q.lastInsertId().toLongLong();
  writeLines(c.db, stored);
  res.action = QStringLiteral("insert");
  return res;
}

qint64 Store::lastSeq() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral("SELECT last_seq FROM sync_state WHERE id = 1"))) {
    fail(q, QStringLiteral("last seq"));
  }
  if (!q.next()) {
    return 0;
  }
  return q.value(0).toLongLong();
}

void Store::setLastSeq(qint64 seq) {
  if (seq < 0) {
    throw std::runtime_error("seq must be >= 0");
  }
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral(
      "UPDATE sync_state SET last_seq = MAX(last_seq, ?) WHERE id = 1"));
  q.addBindValue(seq);
  if (!q.exec()) {
    fail(q, QStringLiteral("set last seq"));
  }
}

SyncStatus Store::syncStatus() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral("SELECT last_seq, last_ok, last_message, "
                             "last_sync_at FROM sync_state WHERE id = 1"))) {
    fail(q, QStringLiteral("sync status"));
  }
  SyncStatus st;
  if (!q.next()) {
    return st;
  }
  st.lastSeq = q.value(0).toLongLong();
  st.lastOk = q.value(1).toInt() != 0;
  st.lastMessage = q.value(2).toString();
  st.lastSyncAt = parseTime(q.value(3).toString());
  return st;
}

void Store::recordSyncResult(bool ok, const QString &message) {
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("UPDATE sync_state SET last_ok = ?, last_message = "
                           "?, last_sync_at = ? WHERE id = 1"));
  q.addBindValue(b(ok));
  q.addBindValue(message);
  q.addBindValue(formatTime(QDateTime::currentDateTimeUtc()));
  if (!q.exec()) {
    fail(q, QStringLiteral("record sync"));
  }
}

int Store::countConflicts() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral(
          "SELECT COUNT(*) FROM report WHERE conflict = 1 AND deleted = 0")) ||
      !q.next()) {
    fail(q, QStringLiteral("count conflicts"));
  }
  return q.value(0).toInt();
}
