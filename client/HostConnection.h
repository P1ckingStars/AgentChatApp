#pragma once

#include <functional>

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

struct HostConfig {
    QString name;     // display name, unique
    QString ssh;      // ssh destination (alias from ~/.ssh/config works); empty = this machine
    QString command;  // overrides the ccbus serve command line
    bool isLocal() const { return ssh.isEmpty(); }
};

// One `ccbus serve --stdio` child process (local, or through ssh) speaking JSON lines.
// Reconnects with backoff and re-establishes subscriptions/watches after each reconnect.
class HostConnection : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const QJsonObject&)>;

    explicit HostConnection(const HostConfig& cfg, QObject* parent = nullptr);
    ~HostConnection() override;

    const HostConfig& config() const { return cfg_; }
    QString name() const { return cfg_.name; }
    bool isConnected() const { return connected_; }
    QString lastError() const { return lastError_; }

    void start();
    // onOk runs only for successful responses; failures emit requestFailed.
    void request(const QString& op, QJsonObject params = {}, Callback onOk = {});
    void watch(const QString& key);
    void unwatch();
    // Command (to run inside a terminal) that attaches to a tmux pane on this host.
    QStringList attachArgv(const QString& pane) const;

signals:
    void connectionChanged(bool connected);
    void snapshot(const QList<QJsonObject>& sessions, qint64 seq);
    void sessionUpdated(const QString& kind, const QJsonObject& session, qint64 seq);
    void sessionRemoved(const QString& key);
    void transcript(const QString& key, bool reset, bool truncated, const QJsonArray& msgs);
    void requestFailed(const QString& op, const QString& error);

private:
    void launch();
    void onStarted();
    void onReadyRead();
    void onFinished();
    void handleLine(const QByteArray& line);

    struct Pending {
        QString op;
        Callback cb;
    };

    HostConfig cfg_;
    QProcess* proc_ = nullptr;
    QByteArray buf_;
    int nextId_ = 1;
    QHash<int, Pending> pending_;
    bool connected_ = false;
    int backoffMs_ = 1000;
    QTimer reconnectTimer_;
    QTimer pingTimer_;
    QElapsedTimer lastRx_;
    QString watchKey_;
    QString lastError_;
};
