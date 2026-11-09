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
    request(QStringLiteral("GET"), QStringLiteral("/healthz"), QByteArray());
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
      out.push_back(p);
    }
    return out;
  }

  QJsonDocument batchPush(const QJsonObject &body) {
    return request(QStringLiteral("POST"), QStringLiteral("/api/reports/batch"),
                   QJsonDocument(body).toJson(QJsonDocument::Compact));
  }

  RemoteReport createReport(const Report &r, qint64 serverProjectId) {
    return readRemote(request(QStringLiteral("POST"),
                              QStringLiteral("/api/reports"),
                              payload(r, serverProjectId)));
  }

  RemoteReport updateReport(qint64 serverId, const Report &r,
                            qint64 serverProjectId) {
    return readRemote(request(QStringLiteral("PUT"),
                              QStringLiteral("/api/reports/%1").arg(serverId),
                              payload(r, serverProjectId)));
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
      r.modified = parseTime(o.value(QStringLiteral("modified")).toString());
      r.seq = static_cast<qint64>(o.value(QStringLiteral("seq")).toDouble());
      r.deleted = o.value(QStringLiteral("deleted")).toInt();
      r.clientUuid = o.value(QStringLiteral("client_uuid")).toString();
      out.push_back(r);
    }
    return out;
  }

private:
  static QByteArray payload(const Report &r, qint64 serverProjectId) {
    QJsonObject o;
    o.insert(QStringLiteral("project_id"),
             static_cast<double>(serverProjectId));
    o.insert(QStringLiteral("start"), formatTime(r.start));
    o.insert(QStringLiteral("comment"), r.comment);
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
  for (const Customer &c : api.listCustomers()) {
    store_.upsertCustomerByServerId(c);
  }
  store_.deleteLocalOnlyCustomers();
  std::vector<std::pair<qint64, qint64>> serverToLocal;
  for (const Customer &c : store_.listCustomers(false)) {
    if (c.serverId > 0) {
      serverToLocal.emplace_back(c.serverId, c.id);
    }
  }
  for (Project p : api.listProjects()) {
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
  store_.deleteLocalOnlyProjects();
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
  for (const PendingOp &op : ops) {
    if (op.kind == OpKind::Delete) {
      deletes.append(static_cast<double>(op.report.serverId));
      continue;
    }
    const qint64 srvProj = store_.serverProjectId(op.report.projectId);
    QJsonObject item;
    item.insert(QStringLiteral("project_id"), static_cast<double>(srvProj));
    item.insert(QStringLiteral("start"), formatTime(op.report.start));
    item.insert(QStringLiteral("comment"), op.report.comment);
    item.insert(QStringLiteral("stop"),
                op.report.stop ? formatTime(*op.report.stop) : QString());
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
        const auto out = api.createReport(
            op.report, store_.serverProjectId(op.report.projectId));
        store_.markReportSynced(op.report.id, out.id, out.seq, out.userId,
                                out.modified);
        stats.creates++;
      } else {
        const auto out =
            api.updateReport(op.report.serverId, op.report,
                             store_.serverProjectId(op.report.projectId));
        const qint64 seq = out.seq == 0 ? op.report.seq : out.seq;
        const qint64 user = out.userId == 0 ? op.report.userId : out.userId;
        const QDateTime mod = out.modified.isValid()
                                  ? out.modified
                                  : QDateTime::currentDateTimeUtc();
        store_.markReportSynced(op.report.id, op.report.serverId, seq, user,
                                mod);
        stats.updates++;
      }
    }
    return stats;
  }
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
    for (const PendingOp &op : ops) {
      if (op.kind == OpKind::Create && op.report.clientUuid == uuid) {
        store_.markReportSynced(
            op.report.id,
            static_cast<qint64>(o.value(QStringLiteral("id")).toDouble()),
            static_cast<qint64>(o.value(QStringLiteral("seq")).toDouble()),
            static_cast<qint64>(o.value(QStringLiteral("user_id")).toDouble()),
            parseTime(o.value(QStringLiteral("modified")).toString()));
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
            parseTime(o.value(QStringLiteral("modified")).toString()));
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
