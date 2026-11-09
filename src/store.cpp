#include "store.hpp"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <stdexcept>

namespace {

constexpr const char *kReportCols =
    "_id, server_id, user_id, project_id, start, stop, comment, modified, seq, "
    "deleted, updated, client_uuid, conflict";

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
  return r;
}

QString newClientUuid() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
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
                     "active INTEGER NOT NULL DEFAULT 0)"),
      QStringLiteral("CREATE TABLE IF NOT EXISTS report ("
                     "_id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL "
                     "DEFAULT -1, project_id INTEGER NOT NULL DEFAULT -1, "
                     "start TEXT NOT NULL, stop TEXT, comment TEXT NOT NULL "
                     "DEFAULT '', modified TEXT NOT NULL, "
                     "seq INTEGER NOT NULL DEFAULT -1, deleted INTEGER NOT "
                     "NULL DEFAULT 0, server_id INTEGER NOT NULL DEFAULT -1, "
                     "updated INTEGER NOT NULL DEFAULT 0, client_uuid TEXT NOT "
                     "NULL DEFAULT '', conflict INTEGER NOT NULL DEFAULT 0)"),
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
  };
  for (const QString &sql : stmts) {
    exec(sql);
  }

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

qint64 Store::deleteLocalOnlyCustomers() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral(
          "DELETE FROM customer WHERE server_id IS NULL OR server_id <= 0"))) {
    fail(q, QStringLiteral("prune customers"));
  }
  return q.numRowsAffected();
}

std::vector<Project> Store::listProjects(qint64 customerId, bool activeOnly) {
  Conn c = connection();
  QString sql =
      QStringLiteral("SELECT _id, server_id, customer_id, name, costcenter, "
                     "hourly_rate, km_rate, active FROM project WHERE 1=1");
  if (customerId > 0) {
    sql += QStringLiteral(" AND customer_id = ?");
  }
  if (activeOnly) {
    sql += QStringLiteral(" AND active = 1");
  }
  sql += QStringLiteral(" ORDER BY name COLLATE NOCASE");
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
      "km_rate, active FROM project WHERE _id = ?"));
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
      "km_rate, active FROM project WHERE server_id = ?"));
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
        "hourly_rate, km_rate, active) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    ins.addBindValue(p.serverId);
    ins.addBindValue(p.customerId);
    ins.addBindValue(p.name);
    ins.addBindValue(p.costCenter);
    ins.addBindValue(optDouble(p.hourlyRate));
    ins.addBindValue(optDouble(p.kmRate));
    ins.addBindValue(b(p.active));
    if (!ins.exec()) {
      fail(ins, QStringLiteral("insert project"));
    }
    return;
  }
  QSqlQuery up(c.db);
  up.prepare(
      QStringLiteral("UPDATE project SET customer_id=?, name=?, costcenter=?, "
                     "hourly_rate=?, km_rate=?, active=? WHERE _id=?"));
  up.addBindValue(p.customerId);
  up.addBindValue(p.name);
  up.addBindValue(p.costCenter);
  up.addBindValue(optDouble(p.hourlyRate));
  up.addBindValue(optDouble(p.kmRate));
  up.addBindValue(b(p.active));
  up.addBindValue(find.value(0).toLongLong());
  if (!up.exec()) {
    fail(up, QStringLiteral("update project"));
  }
}

qint64 Store::deleteLocalOnlyProjects() {
  Conn c = connection();
  QSqlQuery q(c.db);
  if (!q.exec(QStringLiteral(
          "DELETE FROM project WHERE server_id IS NULL OR server_id <= 0"))) {
    fail(q, QStringLiteral("prune projects"));
  }
  return q.numRowsAffected();
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
  return readReport(q);
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
        "modified, seq, deleted, server_id, updated, client_uuid, conflict) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
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
    if (!q.exec()) {
      fail(q, QStringLiteral("insert report"));
    }
    r.id = q.lastInsertId().toLongLong();
    return;
  }
  r.modified = QDateTime::currentDateTimeUtc();
  r.updated = true;
  r.conflict = false;
  r.comment = sqlText(r.comment);
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral(
      "UPDATE report SET user_id=?, project_id=?, start=?, stop=?, comment=?, "
      "modified=?, seq=?, deleted=?, server_id=?, updated=?, "
      "client_uuid=?, conflict=? WHERE _id=?"));
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
  q.addBindValue(r.id);
  if (!q.exec()) {
    fail(q, QStringLiteral("update report"));
  }
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
  std::vector<PendingOp> deletes, creates, updates;
  while (q.next()) {
    Report r = readReport(q);
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
  return readReport(q);
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
  return readReport(q);
}

void Store::hardDeleteReport(qint64 id) {
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral("DELETE FROM report WHERE _id = ?"));
  q.addBindValue(id);
  if (!q.exec()) {
    fail(q, QStringLiteral("delete report"));
  }
}

void Store::markReportSynced(qint64 localId, qint64 serverId, qint64 seq,
                             qint64 userId, const QDateTime &modified) {
  if (localId == 0) {
    throw std::runtime_error("local id required");
  }
  QDateTime mod =
      modified.isValid() ? modified : QDateTime::currentDateTimeUtc();
  Conn c = connection();
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral(
      "UPDATE report SET server_id=?, seq=?, user_id=?, modified=?, updated=0, "
      "deleted=0, conflict=0 WHERE _id=?"));
  q.addBindValue(serverId);
  q.addBindValue(seq);
  q.addBindValue(userId);
  q.addBindValue(formatTime(mod));
  q.addBindValue(localId);
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
        "server_id=?, client_uuid=?, conflict=0 WHERE _id=?"));
    q.addBindValue(r.userId);
    q.addBindValue(r.projectId);
    q.addBindValue(formatTime(r.start));
    q.addBindValue(stopBind(r.stop));
    q.addBindValue(sqlText(r.comment));
    q.addBindValue(formatTime(r.modified));
    q.addBindValue(r.seq);
    q.addBindValue(r.serverId);
    q.addBindValue(sqlText(cu));
    q.addBindValue(existing->id);
    if (!q.exec()) {
      fail(q, QStringLiteral("apply update"));
    }
    res.action = res.linked ? QStringLiteral("link") : QStringLiteral("update");
    return res;
  }
  QSqlQuery q(c.db);
  q.prepare(QStringLiteral(
      "INSERT INTO report (user_id, project_id, start, stop, comment, "
      "modified, seq, deleted, server_id, updated, client_uuid, conflict) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, 0, ?, 0)"));
  q.addBindValue(r.userId);
  q.addBindValue(r.projectId);
  q.addBindValue(formatTime(r.start));
  q.addBindValue(stopBind(r.stop));
  q.addBindValue(sqlText(r.comment));
  q.addBindValue(formatTime(r.modified));
  q.addBindValue(r.seq);
  q.addBindValue(r.serverId);
  q.addBindValue(sqlText(r.clientUuid));
  if (!q.exec()) {
    fail(q, QStringLiteral("apply insert"));
  }
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
