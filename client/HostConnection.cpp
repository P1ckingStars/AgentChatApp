#include "HostConnection.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>

namespace {

constexpr int kProtoVersion = 1;
constexpr int kPingMs = 15000;
constexpr int kDeadMs = 45000;
constexpr int kMaxBackoffMs = 30000;

QString localCcbus() {
    QString sibling = QCoreApplication::applicationDirPath() + "/ccbus";
    return QFileInfo(sibling).isExecutable() ? sibling : QStringLiteral("ccbus");
}

}  // namespace

HostConnection::HostConnection(const HostConfig& cfg, QObject* parent) : QObject(parent), cfg_(cfg) {
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &HostConnection::launch);
    pingTimer_.setInterval(kPingMs);
    connect(&pingTimer_, &QTimer::timeout, this, [this] {
        if (!proc_) return;
        if (lastRx_.isValid() && lastRx_.elapsed() > kDeadMs) {
            lastError_ = "connection timed out";
            proc_->kill();
            return;
        }
        request("ping");
    });
}

HostConnection::~HostConnection() {
    if (proc_) {
        proc_->disconnect(this);
        proc_->kill();
        proc_->waitForFinished(1000);
    }
}

void HostConnection::start() { launch(); }

void HostConnection::launch() {
    if (proc_) return;
    QString program;
    QStringList args;
    if (cfg_.isLocal()) {
        if (cfg_.command.isEmpty()) {
            program = localCcbus();
            args = {"serve", "--stdio"};
        } else {
            args = QProcess::splitCommand(cfg_.command);
            program = args.isEmpty() ? localCcbus() : args.takeFirst();
        }
    } else {
        // BatchMode: never hang on a password prompt we can't show; use keys or an agent.
        program = "ssh";
        args = {"-T", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
                cfg_.ssh,
                cfg_.command.isEmpty() ? QStringLiteral("~/.local/bin/ccbus serve --stdio") : cfg_.command};
    }

    proc_ = new QProcess(this);
    buf_.clear();
    connect(proc_, &QProcess::started, this, &HostConnection::onStarted);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &HostConnection::onReadyRead);
    connect(proc_, &QProcess::readyReadStandardError, this, [this] {
        const QList<QByteArray> lines = proc_->readAllStandardError().trimmed().split('\n');
        if (!lines.isEmpty() && !lines.last().isEmpty()) lastError_ = QString::fromUtf8(lines.last());
    });
    connect(proc_, &QProcess::finished, this, &HostConnection::onFinished);
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        lastError_ = "failed to start " + proc_->program();
        onFinished();  // finished() is not emitted for processes that never started
    });
    proc_->start(program, args);
}

void HostConnection::onStarted() {
    lastRx_.start();
    pingTimer_.start();
    request("hello", {{"proto", kProtoVersion}}, [this](const QJsonObject& hello) {
        if (hello["proto"].toInt() != kProtoVersion) {
            lastError_ = QString("protocol mismatch (server %1, client %2): update ccbus on %3")
                             .arg(hello["proto"].toInt())
                             .arg(kProtoVersion)
                             .arg(cfg_.name);
            proc_->kill();
            return;
        }
        request("list_sessions", {}, [this](const QJsonObject& r) {
            QList<QJsonObject> sessions;
            for (const QJsonValue& v : r["sessions"].toArray()) sessions << v.toObject();
            qint64 seq = r["seq"].toInteger();
            emit snapshot(sessions, seq);
            request("subscribe", {{"since_seq", seq}});
            if (!watchKey_.isEmpty()) request("watch", {{"key", watchKey_}});
            backoffMs_ = 1000;
            lastError_.clear();
            connected_ = true;
            emit connectionChanged(true);
        });
    });
}

void HostConnection::onFinished() {
    if (!proc_) return;
    if (lastError_.isEmpty()) lastError_ = "disconnected";
    proc_->deleteLater();
    proc_ = nullptr;
    pending_.clear();
    pingTimer_.stop();
    connected_ = false;
    emit connectionChanged(false);
    reconnectTimer_.start(backoffMs_);
    backoffMs_ = qMin(backoffMs_ * 2, kMaxBackoffMs);
}

void HostConnection::request(const QString& op, QJsonObject params, Callback onOk) {
    if (!proc_ || proc_->state() != QProcess::Running) {
        emit requestFailed(op, "not connected to " + cfg_.name);
        return;
    }
    int id = nextId_++;
    params["id"] = id;
    params["op"] = op;
    pending_.insert(id, {op, std::move(onOk)});
    proc_->write(QJsonDocument(params).toJson(QJsonDocument::Compact) + '\n');
}

void HostConnection::watch(const QString& key) {
    watchKey_ = key;
    if (connected_) request("watch", {{"key", key}});
}

void HostConnection::unwatch() {
    if (watchKey_.isEmpty()) return;
    watchKey_.clear();
    if (connected_) request("unwatch");
}

QStringList HostConnection::attachArgv(const QString& pane) const {
    QStringList tmux = {"tmux", "attach-session", "-t", pane};
    if (cfg_.isLocal()) return tmux;
    return QStringList{"ssh", "-t", cfg_.ssh} + tmux;
}

void HostConnection::onReadyRead() {
    buf_ += proc_->readAllStandardOutput();
    lastRx_.restart();
    qsizetype nl;
    while (proc_ && (nl = buf_.indexOf('\n')) >= 0) {
        QByteArray line = buf_.left(nl);
        buf_.remove(0, nl + 1);
        if (!line.isEmpty()) handleLine(line);
    }
}

void HostConnection::handleLine(const QByteArray& line) {
    QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) return;
    QJsonObject o = doc.object();

    if (o.contains("re")) {
        auto it = pending_.find(o["re"].toInt());
        if (it == pending_.end()) return;
        Pending p = it.value();
        pending_.erase(it);
        if (o["ok"].toBool()) {
            if (p.cb) p.cb(o);
        } else {
            emit requestFailed(p.op, o["error"].toString());
        }
        return;
    }

    QString ev = o["ev"].toString();
    if (ev == "session")
        emit sessionUpdated(o["kind"].toString(), o["session"].toObject(), o["seq"].toInteger());
    else if (ev == "removed")
        emit sessionRemoved(o["key"].toString());
    else if (ev == "transcript")
        emit transcript(o["key"].toString(), o["reset"].toBool(), o["truncated"].toBool(),
                        o["msgs"].toArray());
}
