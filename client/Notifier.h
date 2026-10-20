#pragma once

#include <QHash>
#include <QObject>

// Desktop notifications via org.freedesktop.Notifications (dunst, mako, KDE, GNOME...).
// One notification per session id, replaced in place; clicking it emits activated(id).
class Notifier : public QObject {
    Q_OBJECT
public:
    explicit Notifier(QObject* parent = nullptr);

    void notify(const QString& id, const QString& summary, const QString& body, bool urgent);
    void close(const QString& id);

signals:
    void activated(const QString& id);

private slots:
    void onActionInvoked(uint nid, const QString& action);
    void onClosed(uint nid, uint reason);

private:
    QHash<QString, uint> byId_;
    QHash<uint, QString> byNid_;
};
