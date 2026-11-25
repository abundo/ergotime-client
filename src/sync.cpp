#include "sync.hpp"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

// Spoken desktop sync contract. Widen the range when this client can
// still talk to an older or newer server. Bump it only for a break:
// a removed or redefined field, a new timestamp format, or a new auth scheme.
constexpr int kMinSyncProtocol = 3;
constexpr int kMaxSyncProtocol = 3;

QString syncProtocolRange() {
  if (kMinSyncProtocol == kMaxSyncProtocol) {
    return QStringLiteral("protocol %1").arg(kMinSyncProtocol);
  }
  return QStringLiteral("protocols %1-%2")
      .arg(kMinSyncProtocol)
      .arg(kMaxSyncProtocol);
}

void requireSyncProtocol(const QJsonDocument &doc) {
  const QJsonObject o = doc.object();
  const QJsonValue v = o.value(QStringLiteral("protocol"));
  if (!o.contains(QStringLiteral("protocol")) || !v.isDouble()) {
    throw std::runtime_error(
        QStringLiteral(
            "server did not advertise a sync protocol; this client speaks %1")
            .arg(syncProtocolRange())
            .toStdString());
  }
  const int protocol = v.toInt();
  if (protocol < kMinSyncProtocol || protocol > kMaxSyncProtocol) {
    throw std::runtime_error(
        QStringLiteral("server protocol %1; this client speaks %2")
            .arg(protocol)
            .arg(syncProtocolRange())
            .toStdString());
  }
}

struct RemoteReport {
  qint64 id = 0;
  qint64 userId = 0;
  qint64 seq = 0;
  QDateTime modified;
  QString clientUuid;
};

class HttpError : public std::runtime_error {
public:
  HttpError(int status, const QString &message)
      : std::runtime_error(message.toStdString()), status(status) {}
  int status;
};

class Api {
public:
  Api(QString base, QString user, QString password, int timeoutMs)
      : base_(base.endsWith('/') ? base.chopped(1) : base),
        user_(std::move(user)), password_(std::move(password)),
        timeoutMs_(timeoutMs) {}

  void health() {
    requireSyncProtocol(request(QStringLiteral("GET"),
                                QStringLiteral("/healthz"), QByteArray()));
  }

  std::vector<Customer> listCustomers() {
    const QJsonDocument doc = request(
        QStringLiteral("GET"), QStringLiteral("/api/customers"), QByteArray());
    std::vector<Customer> out;
    for (const auto &v : doc.object().value(QStringLiteral("data")).toArray()) {
      const QJsonObject o = v.toObject();
      Customer c;
      c.serverId =
          static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
      c.name = o.value(QStringLiteral("name")).toString();
      c.description = o.value(QStringLiteral("description")).toString();
      c.active = o.value(QStringLiteral("active")).toBool();
      out.push_back(c);
    }
    return out;
  }

  std::vector<Project> listProjects() {
    const QJsonDocument doc = request(
        QStringLiteral("GET"), QStringLiteral("/api/projects"), QByteArray());
    std::vector<Project> out;
    for (const auto &v : doc.object().value(QStringLiteral("data")).toArray()) {
      const QJsonObject o = v.toObject();
      Project p;
      p.serverId =
          static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
      p.customerId = static_cast<qint64>(
          o.value(QStringLiteral("customer_id")).toDouble());
      p.name = o.value(QStringLiteral("name")).toString();
      p.costCenter = o.value(QStringLiteral("cost_center")).toString();
      if (!o.value(QStringLiteral("hourly_rate")).isNull()) {
        p.hourlyRate = o.value(QStringLiteral("hourly_rate")).toDouble();
      }
      if (!o.value(QStringLiteral("km_rate")).isNull()) {
        p.kmRate = o.value(QStringLiteral("km_rate")).toDouble();
      }
      p.active = o.value(QStringLiteral("active")).toBool();
      p.isDefault = o.value(QStringLiteral("is_default")).toBool();
      out.push_back(p);
    }
    return out;
  }

  QJsonDocument batchPush(const QJsonObject &body) {
    return request(QStringLiteral("POST"), QStringLiteral("/api/reports/batch"),
                   QJsonDocument(body).toJson(QJsonDocument::Compact));
  }

  RemoteReport createReport(const Report &r) {
    return readRemote(request(QStringLiteral("POST"),
                              QStringLiteral("/api/reports"), payload(r)));
  }

  RemoteReport updateReport(qint64 serverId, const Report &r) {
    return readRemote(request(QStringLiteral("PUT"),
                              QStringLiteral("/api/reports/%1").arg(serverId),
                              payload(r)));
  }

  void deleteReport(qint64 serverId) {
    request(QStringLiteral("DELETE"),
            QStringLiteral("/api/reports/%1").arg(serverId), QByteArray());
  }

  struct Legacy {
    qint64 id = 0;
    qint64 userId = 0;
    qint64 projectId = 0;
    QDateTime start;
    std::optional<QDateTime> stop;
    QString comment;
    std::vector<ReportLine> lines;
    QDateTime modified;
    qint64 seq = 0;
    int deleted = 0;
    QString clientUuid;
  };

  std::vector<Legacy> pullReports(qint64 afterSeq, int limit, int maxAgeDays) {
    const QString path =
        QStringLiteral("/api/report/sync/%1?limit=%2&maxage=%3")
            .arg(afterSeq)
            .arg(limit)
            .arg(maxAgeDays);
    const QJsonDocument doc =
        request(QStringLiteral("GET"), path, QByteArray());
    std::vector<Legacy> out;
    for (const auto &v : doc.object().value(QStringLiteral("data")).toArray()) {
      const QJsonObject o = v.toObject();
      Legacy r;
      r.id = static_cast<qint64>(o.value(QStringLiteral("_id")).toDouble());
      if (r.id == 0) {
        r.id = static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
      }
      r.userId =
          static_cast<qint64>(o.value(QStringLiteral("user_id")).toDouble());
      r.projectId =
          static_cast<qint64>(o.value(QStringLiteral("project_id")).toDouble());
      r.start = parseTime(o.value(QStringLiteral("start")).toString());
      const QString stop = o.value(QStringLiteral("stop")).toString();
      if (!stop.isEmpty()) {
        r.stop = parseTime(stop);
      }
      r.comment = o.value(QStringLiteral("comment")).toString();
      const QJsonArray lines = o.value(QStringLiteral("lines")).toArray();
      for (const auto &lv : lines) {
        const QJsonObject lo = lv.toObject();
        ReportLine ln;
        ln.projectId = static_cast<qint64>(
            lo.value(QStringLiteral("project_id")).toDouble());
        ln.comment = lo.value(QStringLiteral("comment")).toString();
        if (ln.projectId > 0) {
          r.lines.push_back(ln);
        }
      }
      if (r.lines.empty() && r.projectId > 0) {
        ReportLine ln;
        ln.projectId = r.projectId;
        ln.comment = r.comment;
        r.lines.push_back(ln);
      }
      r.modified = parseTime(o.value(QStringLiteral("modified")).toString());
      r.seq = static_cast<qint64>(o.value(QStringLiteral("seq")).toDouble());
      r.deleted = o.value(QStringLiteral("deleted")).toInt();
      r.clientUuid = o.value(QStringLiteral("client_uuid")).toString();
      out.push_back(r);
    }
    return out;
  }

private:
  static QByteArray payload(const Report &r) {
    QJsonObject o;
    o.insert(QStringLiteral("project_id"), static_cast<double>(r.projectId));
    o.insert(QStringLiteral("start"), formatTime(r.start));
    o.insert(QStringLiteral("comment"), r.comment);
    QJsonArray lines;
    const std::vector<ReportLine> stored =
        r.lines.empty() && r.projectId > 0
            ? std::vector<ReportLine>{{r.projectId, r.comment}}
            : r.lines;
    for (const ReportLine &ln : stored) {
      QJsonObject lo;
      lo.insert(QStringLiteral("project_id"),
                static_cast<double>(ln.projectId));
      lo.insert(QStringLiteral("comment"), ln.comment);
      lines.append(lo);
    }
    o.insert(QStringLiteral("lines"), lines);
    o.insert(QStringLiteral("deleted"), r.deleted);
    o.insert(QStringLiteral("stop"), r.stop ? formatTime(*r.stop) : QString());
    if (r.userId > 0) {
      o.insert(QStringLiteral("user_id"), static_cast<double>(r.userId));
    }
    if (!r.clientUuid.isEmpty()) {
      o.insert(QStringLiteral("client_uuid"), r.clientUuid);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
  }

  static RemoteReport readRemote(const QJsonDocument &doc) {
    const QJsonObject o = doc.object();
    RemoteReport r;
    r.id = static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
    r.userId =
        static_cast<qint64>(o.value(QStringLiteral("user_id")).toDouble());
    r.seq = static_cast<qint64>(o.value(QStringLiteral("seq")).toDouble());
    r.modified = parseTime(o.value(QStringLiteral("modified")).toString());
    r.clientUuid = o.value(QStringLiteral("client_uuid")).toString();
    return r;
  }

  QJsonDocument request(const QString &method, const QString &path,
                        const QByteArray &body) {
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(base_ + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    if (!user_.isEmpty()) {
      const QByteArray token =
          (user_ + QLatin1Char(':') + password_).toUtf8().toBase64();
      req.setRawHeader("Authorization", QByteArray("Basic ") + token);
    }
    QNetworkReply *reply = nam.sendCustomRequest(req, method.toUtf8(), body);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs_);
    loop.exec();
    if (!reply->isFinished()) {
      reply->abort();
      reply->deleteLater();
      throw std::runtime_error("network timeout");
    }
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = reply->readAll();
    const auto err = reply->error();
    const QString errStr = reply->errorString();
    reply->deleteLater();
    if (err != QNetworkReply::NoError && status == 0) {
      throw std::runtime_error(errStr.toStdString());
    }
    if (status >= 300) {
      throw HttpError(status, QStringLiteral("%1 %2: status %3: %4")
                                  .arg(method, path)
                                  .arg(status)
                                  .arg(QString::fromUtf8(raw).left(500)));
    }
    if (raw.isEmpty()) {
      return {};
    }
    return QJsonDocument::fromJson(raw);
  }

  QString base_;
  QString user_;
  QString password_;
  int timeoutMs_;
};

bool batchUnsupported(const std::exception &ex) {
  const QString s = QString::fromUtf8(ex.what());
  return s.contains(QStringLiteral("status 404")) ||
         s.contains(QStringLiteral("status 405"));
}

} // namespace

Engine::Engine(Store &store, Settings &settings, QObject *parent)
    : QObject(parent), store_(store), settings_(settings) {}

Engine::~Engine() { stopPeriodic(); }

void Engine::startPeriodic() {
  std::lock_guard lock(lifeMu_);
  if (periodic_.joinable()) {
    return;
  }
  stop_ = false;
  const int sec = settings_.get().reportSyncIntervalSec;
  const int baseMs = (sec > 0 ? sec : 600) * 1000;
  periodic_ = std::thread([this, baseMs] {
    int waitMs = baseMs;
    while (!stop_.load()) {
      for (int slept = 0; slept < waitMs && !stop_.load(); slept += 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (stop_.load()) {
        return;
      }
      SyncResult res;
      try {
        res = sync();
      } catch (const std::exception &ex) {
        res.ok = false;
        res.message = QString::fromUtf8(ex.what());
      }
      std::lock_guard life(lifeMu_);
      if (!res.ok && !res.skipped) {
        failStreak_ = std::min(failStreak_ + 1, 6);
        waitMs = std::min(baseMs * (1 << failStreak_), 3600000);
        qWarning() << "sync failed" << res.message << "backoff" << waitMs;
      } else if (!res.skipped) {
        failStreak_ = 0;
        waitMs = baseMs;
        qInfo() << "sync finished" << res.message;
      }
      emit finished(res);
    }
  });
}

void Engine::stopPeriodic() {
  stop_ = true;
  std::thread t;
  {
    std::lock_guard lock(lifeMu_);
    if (periodic_.joinable()) {
      t = std::move(periodic_);
    }
  }
  if (t.joinable()) {
    t.join();
  }
}

void Engine::restartPeriodic() {
  stopPeriodic();
  startPeriodic();
}

SyncResult Engine::sync() {
  std::unique_lock lock(syncMu_, std::try_to_lock);
  if (!lock.owns_lock()) {
    SyncResult skipped;
    skipped.skipped = true;
    skipped.message = QStringLiteral("sync already in progress");
    return skipped;
  }
  const Config cfg = settings_.get();
  auto record = [&](SyncResult res) {
    try {
      store_.recordSyncResult(res.ok, res.message);
    } catch (...) {
    }
    return res;
  };
  SyncResult res;
  try {
    res.unsyncedLeft = store_.countUnsynced();
    res.conflictsLeft = store_.countConflicts();
  } catch (const std::exception &ex) {
    res.message = QString::fromUtf8(ex.what());
    return res;
  }
  if (cfg.username.isEmpty()) {
    res.message = QStringLiteral("username not configured");
    return record(res);
  }
  Api api(cfg.serverUrl, cfg.username, cfg.password, cfg.networkTimeoutMs());
  try {
    api.health();
    pullCatalog();
  } catch (const std::exception &ex) {
    res.message = QString::fromUtf8(ex.what());
    return record(res);
  }
  PushStats push;
  try {
    push = pushOps();
  } catch (const std::exception &ex) {
    res.message = QStringLiteral("push: %1").arg(QString::fromUtf8(ex.what()));
    res.pushedCreates = push.creates;
    res.pushedUpdates = push.updates;
    res.pushedDeletes = push.deletes;
    try {
      res.unsyncedLeft = store_.countUnsynced();
      res.conflictsLeft = store_.countConflicts();
    } catch (...) {
    }
    return record(res);
  }
  PullStats pull;
  try {
    pull = pullReports();
  } catch (const std::exception &ex) {
    res.message = QStringLiteral("pull: %1").arg(QString::fromUtf8(ex.what()));
    res.pushedCreates = push.creates;
    res.pushedUpdates = push.updates;
    res.pushedDeletes = push.deletes;
    res.pulled = pull.pulled;
    res.conflicts = pull.conflicts;
    res.linkedOrphans = pull.linked;
    return record(res);
  }
  res.ok = true;
  res.pushedCreates = push.creates;
  res.pushedUpdates = push.updates;
  res.pushedDeletes = push.deletes;
  res.pulled = pull.pulled;
  res.conflicts = pull.conflicts;
  res.linkedOrphans = pull.linked;
  try {
    res.unsyncedLeft = store_.countUnsynced();
    res.conflictsLeft = store_.countConflicts();
  } catch (...) {
  }
  res.message = QStringLiteral("ok; +%1 ~%2 -%3 pull %4")
                    .arg(push.creates)
                    .arg(push.updates)
                    .arg(push.deletes)
                    .arg(pull.pulled);
  if (pull.linked > 0) {
    res.message += QStringLiteral(" (link %1)").arg(pull.linked);
  }
  if (res.unsyncedLeft > 0) {
    res.message += QStringLiteral(", %1 pending").arg(res.unsyncedLeft);
  }
  if (res.conflictsLeft > 0) {
    res.message += QStringLiteral(", %1 conflict(s)").arg(res.conflictsLeft);
  }
  return record(res);
}

SyncResult Engine::syncCatalog() {
  std::unique_lock lock(syncMu_, std::try_to_lock);
  if (!lock.owns_lock()) {
    SyncResult skipped;
    skipped.skipped = true;
    skipped.message = QStringLiteral("sync already in progress");
    return skipped;
  }
  const Config cfg = settings_.get();
  SyncResult res;
  if (cfg.username.isEmpty()) {
    res.message = QStringLiteral("username not configured");
    store_.recordSyncResult(false, res.message);
    return res;
  }
  Api api(cfg.serverUrl, cfg.username, cfg.password, cfg.networkTimeoutMs());
  try {
    api.health();
    pullCatalog();
  } catch (const std::exception &ex) {
    res.message = QString::fromUtf8(ex.what());
    store_.recordSyncResult(false, res.message);
    return res;
  }
  const auto custs = store_.listCustomers(true);
  const auto projs = store_.listProjects(0, true);
  res.ok = true;
  res.pulled = static_cast<int>(custs.size() + projs.size());
  res.message = QStringLiteral("catalog ok; %1 customers, %2 projects")
                    .arg(custs.size())
                    .arg(projs.size());
  store_.recordSyncResult(true, res.message);
  return res;
}

void Engine::pullCatalog() {
  const Config cfg = settings_.get();
  Api api(cfg.serverUrl, cfg.username, cfg.password, cfg.networkTimeoutMs());
  const auto customers = api.listCustomers();
  std::vector<qint64> customerServerIds;
  customerServerIds.reserve(customers.size());
  for (const Customer &c : customers) {
    store_.upsertCustomerByServerId(c);
    if (c.serverId > 0) {
      customerServerIds.push_back(c.serverId);
    }
  }
  const qint64 droppedCustomers =
      store_.deleteCustomersExceptServerIds(customerServerIds);
  std::vector<std::pair<qint64, qint64>> serverToLocal;
  for (const Customer &c : store_.listCustomers(false)) {
    if (c.serverId > 0) {
      serverToLocal.emplace_back(c.serverId, c.id);
    }
  }
  const auto projects = api.listProjects();
  std::vector<qint64> projectServerIds;
  projectServerIds.reserve(projects.size());
  for (Project p : projects) {
    if (p.serverId > 0) {
      projectServerIds.push_back(p.serverId);
    }
    qint64 local = 0;
    for (const auto &[sid, lid] : serverToLocal) {
      if (sid == p.customerId) {
        local = lid;
        break;
      }
    }
    if (local == 0) {
      qWarning() << "project references unknown customer" << p.name
                 << p.customerId;
      continue;
    }
    p.customerId = local;
    store_.upsertProjectByServerId(p);
  }
  const qint64 droppedProjects =
      store_.deleteProjectsExceptServerIds(projectServerIds);
  if (droppedCustomers > 0 || droppedProjects > 0) {
    qInfo() << "catalog removed" << droppedCustomers << "customers and"
            << droppedProjects << "projects the server no longer has";
  }
}

Engine::PushStats Engine::pushOps() {
  const Config cfg = settings_.get();
  Api api(cfg.serverUrl, cfg.username, cfg.password, cfg.networkTimeoutMs());
  const auto ops = store_.listPendingOps();
  PushStats stats;
  if (ops.empty()) {
    return stats;
  }
  QJsonArray deletes, creates, updates;
  // Local modified time actually pushed, after a possible align write.
  // Marking synced must not clear a row saved again while the push was out.
  std::vector<std::pair<qint64, QDateTime>> pushedModified;
  for (const PendingOp &op : ops) {
    if (op.kind == OpKind::Delete) {
      deletes.append(static_cast<double>(op.report.serverId));
      continue;
    }
    // Pending rows saved before this rule can still carry seconds. Align them
    // before the push so the server accepts the batch, and keep the local row.
    Report report = op.report;
    alignStoredReport(report);
    const bool sameStop =
        (!report.stop && !op.report.stop) ||
        (report.stop && op.report.stop && *report.stop == *op.report.stop);
    if (report.start != op.report.start || !sameStop) {
      store_.saveReport(report);
    }
    pushedModified.emplace_back(report.id, report.modified);
    const Report mapped = serverReport(report);
    QJsonObject item;
    item.insert(QStringLiteral("project_id"),
                static_cast<double>(mapped.projectId));
    item.insert(QStringLiteral("start"), formatTime(mapped.start));
    item.insert(QStringLiteral("comment"), mapped.comment);
    QJsonArray lines;
    for (const ReportLine &ln : mapped.lines) {
      QJsonObject lo;
      lo.insert(QStringLiteral("project_id"),
                static_cast<double>(ln.projectId));
      lo.insert(QStringLiteral("comment"), ln.comment);
      lines.append(lo);
    }
    item.insert(QStringLiteral("lines"), lines);
    item.insert(QStringLiteral("stop"),
                mapped.stop ? formatTime(*mapped.stop) : QString());
    if (op.kind == OpKind::Create) {
      item.insert(QStringLiteral("client_uuid"), op.report.clientUuid);
      if (op.report.userId > 0) {
        item.insert(QStringLiteral("user_id"),
                    static_cast<double>(op.report.userId));
      }
      creates.append(item);
    } else {
      item.insert(QStringLiteral("id"),
                  static_cast<double>(op.report.serverId));
      if (!op.report.clientUuid.isEmpty()) {
        item.insert(QStringLiteral("client_uuid"), op.report.clientUuid);
      }
      updates.append(item);
    }
  }
  QJsonObject body;
  body.insert(QStringLiteral("deletes"), deletes);
  body.insert(QStringLiteral("creates"), creates);
  body.insert(QStringLiteral("updates"), updates);
  QJsonDocument batch;
  try {
    batch = api.batchPush(body);
  } catch (const std::exception &ex) {
    if (!batchUnsupported(ex)) {
      throw;
    }
    qInfo() << "batch push unsupported; falling back to per-op";
    for (const PendingOp &op : store_.listPendingOps()) {
      if (op.kind == OpKind::Delete) {
        api.deleteReport(op.report.serverId);
        store_.hardDeleteReport(op.report.id);
        stats.deletes++;
      } else if (op.kind == OpKind::Create) {
        const auto out = api.createReport(serverReport(op.report));
        store_.markReportSynced(op.report.id, out.id, out.seq, out.userId,
                                out.modified, op.report.modified);
        stats.creates++;
      } else {
        const auto out =
            api.updateReport(op.report.serverId, serverReport(op.report));
        const qint64 seq = out.seq == 0 ? op.report.seq : out.seq;
        const qint64 user = out.userId == 0 ? op.report.userId : out.userId;
        const QDateTime mod = out.modified.isValid()
                                  ? out.modified
                                  : QDateTime::currentDateTimeUtc();
        store_.markReportSynced(op.report.id, op.report.serverId, seq, user,
                                mod, op.report.modified);
        stats.updates++;
      }
    }
    return stats;
  }
  const auto pushedAt = [&pushedModified](qint64 localId) {
    for (const auto &row : pushedModified) {
      if (row.first == localId) {
        return row.second;
      }
    }
    return QDateTime();
  };
  const QJsonObject root = batch.object();
  for (const auto &v : root.value(QStringLiteral("deleted")).toArray()) {
    const qint64 sid = static_cast<qint64>(
        v.toObject().value(QStringLiteral("id")).toDouble());
    for (const PendingOp &op : ops) {
      if (op.kind == OpKind::Delete && op.report.serverId == sid) {
        store_.hardDeleteReport(op.report.id);
        stats.deletes++;
      }
    }
  }
  for (const auto &v : root.value(QStringLiteral("created")).toArray()) {
    const QJsonObject o = v.toObject();
    const QString uuid = o.value(QStringLiteral("client_uuid")).toString();
    // An empty id matches every other empty id. Only a real client_uuid may
    // attach a server row, otherwise one create is marked synced as another.
    if (uuid.isEmpty()) {
      qWarning() << "batch create response has no client_uuid; leaving the "
                    "local report dirty";
      continue;
    }
    for (const PendingOp &op : ops) {
      if (op.kind == OpKind::Create && op.report.clientUuid == uuid) {
        store_.markReportSynced(
            op.report.id,
            static_cast<qint64>(o.value(QStringLiteral("id")).toDouble()),
            static_cast<qint64>(o.value(QStringLiteral("seq")).toDouble()),
            static_cast<qint64>(o.value(QStringLiteral("user_id")).toDouble()),
            parseTime(o.value(QStringLiteral("modified")).toString()),
            pushedAt(op.report.id));
        stats.creates++;
      }
    }
  }
  for (const auto &v : root.value(QStringLiteral("updated")).toArray()) {
    const QJsonObject o = v.toObject();
    const qint64 sid =
        static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
    for (const PendingOp &op : ops) {
      if (op.kind == OpKind::Update && op.report.serverId == sid) {
        store_.markReportSynced(
            op.report.id, sid,
            static_cast<qint64>(o.value(QStringLiteral("seq")).toDouble()),
            static_cast<qint64>(o.value(QStringLiteral("user_id")).toDouble()),
            parseTime(o.value(QStringLiteral("modified")).toString()),
            pushedAt(op.report.id));
        stats.updates++;
      }
    }
  }
  return stats;
}

Engine::PullStats Engine::pullReports() {
  const Config cfg = settings_.get();
  Api api(cfg.serverUrl, cfg.username, cfg.password, cfg.networkTimeoutMs());
  PullStats stats;
  qint64 after = store_.lastSeq();
  constexpr int step = 50;
  qint64 high = after;
  for (;;) {
    const auto page = api.pullReports(after, step, 180);
    if (page.empty()) {
      break;
    }
    qint64 pageHigh = 0;
    for (const auto &srv : page) {
      high = std::max(high, srv.seq);
      pageHigh = std::max(pageHigh, srv.seq);
      Report r;
      r.serverId = srv.id;
      r.userId = srv.userId;
      r.projectId = mapServerProject(srv.projectId);
      for (const ReportLine &ln : srv.lines) {
        ReportLine local = ln;
        local.projectId = mapServerProject(ln.projectId);
        if (local.projectId > 0) {
          r.lines.push_back(local);
        }
      }
      if (r.lines.empty() && r.projectId > 0) {
        r.lines.push_back(ReportLine{r.projectId, srv.comment});
      }
      const qint64 lineProject =
          r.lines.empty() ? r.projectId : r.lines.front().projectId;
      if (auto p = store_.getProject(lineProject)) {
        r.customerId = p->customerId;
      }
      r.start = srv.start;
      r.stop = srv.stop;
      r.comment = srv.comment;
      r.modified = srv.modified;
      r.seq = srv.seq;
      r.deleted = srv.deleted != 0;
      r.clientUuid = srv.clientUuid;
      const ApplyResult applied = store_.applyServerReport(r);
      stats.pulled++;
      if (applied.conflict) {
        stats.conflicts++;
      }
      if (applied.linked) {
        stats.linked++;
      }
    }
    if (pageHigh > after) {
      store_.setLastSeq(pageHigh);
      after = pageHigh;
    }
    if (static_cast<int>(page.size()) < step || pageHigh <= 0) {
      break;
    }
  }
  if (high > 0) {
    store_.setLastSeq(high);
  }
  return stats;
}

Report Engine::serverReport(const Report &local) const {
  Report out = local;
  if (out.projectId > 0) {
    out.projectId = store_.serverProjectId(out.projectId);
  }
  if (out.lines.empty() && local.projectId > 0) {
    out.lines.push_back(ReportLine{out.projectId, out.comment});
  }
  for (ReportLine &ln : out.lines) {
    ln.projectId = store_.serverProjectId(ln.projectId);
  }
  return out;
}

qint64 Engine::mapServerProject(qint64 serverProjectId) {
  if (serverProjectId <= 0) {
    return 0;
  }
  if (auto p = store_.getProjectByServerId(serverProjectId)) {
    return p->id;
  }
  qWarning() << "unknown server project on pull" << serverProjectId;
  return serverProjectId;
}
