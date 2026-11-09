#pragma once

#include "domain.hpp"

#include <QString>

#include <mutex>
#include <optional>
#include <vector>

struct SyncStatus {
  qint64 lastSeq = 0;
  bool lastOk = false;
  QString lastMessage;
  QDateTime lastSyncAt;
};

enum class OpKind { Create, Update, Delete };

struct PendingOp {
  OpKind kind = OpKind::Create;
  Report report;
};

struct ApplyResult {
  QString action; // insert, update, link, skip_dirty, delete, noop
  bool conflict = false;
  bool linked = false;
};

// SQLite store. Schema matches the Go client so both can share
// ~/.ergotime2/ergotime.db. Each thread opens its own QSQLITE connection.
class Store {
public:
  explicit Store(const QString &path);
  ~Store();

  std::vector<Customer> listCustomers(bool activeOnly);
  std::optional<Customer> getCustomer(qint64 id);
  std::optional<Customer> getCustomerByServerId(qint64 serverId);
  void upsertCustomerByServerId(const Customer &c);
  // keep is the server ids just returned. Any other customer row is removed.
  qint64 deleteCustomersExceptServerIds(const std::vector<qint64> &keep);

  std::vector<Project> listProjects(qint64 customerId, bool activeOnly);
  std::optional<Project> getProject(qint64 id);
  std::optional<Project> getProjectByServerId(qint64 serverId);
  void upsertProjectByServerId(const Project &p);
  // keep is the server ids just returned. Any other project row is removed.
  // Report lines on a removed project are folded onto a project that remains.
  qint64 deleteProjectsExceptServerIds(const std::vector<qint64> &keep);
  qint64 serverProjectId(qint64 localId);
  std::vector<std::pair<qint64, QString>> projectNames();
  std::vector<std::pair<qint64, QString>> customerNames();
  qint64 projectCustomerLocalId(qint64 projectId);

  std::optional<Report> getReport(qint64 id);
  void deleteReport(qint64 id);
  void saveReport(Report &r);
  std::vector<Report> listReportsBetween(const QDateTime &from,
                                         const QDateTime &to);
  std::vector<PendingOp> listPendingOps();
  int countUnsynced();
  std::optional<Report> getReportByServerId(qint64 serverId);
  std::optional<Report> getReportByClientUuid(const QString &uuid);
  void hardDeleteReport(qint64 id);
  void markReportSynced(qint64 localId, qint64 serverId, qint64 seq,
                        qint64 userId, const QDateTime &modified);
  ApplyResult applyServerReport(const Report &r);

  qint64 lastSeq();
  void setLastSeq(qint64 seq);
  SyncStatus syncStatus();
  void recordSyncResult(bool ok, const QString &message);
  int countConflicts();

private:
  class Conn;
  Conn connection();
  void migrate();
  bool needsSchemaCutover(Conn &c);

  QString path_;
  std::once_flag migrated_;
};
