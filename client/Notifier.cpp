#include "Notifier.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

namespace {
const char* kService = "org.freedesktop.Notifications";
const char* kPath = "/org/freedesktop/Notifications";
const char* kIface = "org.freedesktop.Notifications";
}  // namespace

Notifier::Notifier(QObject* parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kIface, "ActionInvoked", this, SLOT(onActionInvoked(uint, QString)));
    bus.connect(kService, kPath, kIface, "NotificationClosed", this, SLOT(onClosed(uint, uint)));
}

void Notifier::notify(const QString& id, const QString& summary, const QString& body, bool urgent) {
    QVariantMap hints;
    hints["urgency"] = QVariant::fromValue<uchar>(urgent ? 2 : 1);
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface, "Notify");
    msg << QString("ccbus") << byId_.value(id, 0u) << QString("utilities-terminal") << summary
        << body.toHtmlEscaped() << QStringList{"default", "Open"} << hints << int(urgent ? 0 : 15000);

    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, id](QDBusPendingCallWatcher* w) {
        QDBusPendingReply<uint> reply = *w;
        if (reply.isValid()) {
            byNid_.remove(byId_.value(id));
            byId_[id] = reply.value();
            byNid_[reply.value()] = id;
        }
        w->deleteLater();
    });
}

void Notifier::close(const QString& id) {
    auto it = byId_.find(id);
    if (it == byId_.end()) return;
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kIface, "CloseNotification");
    msg << it.value();
    QDBusConnection::sessionBus().asyncCall(msg);
    byNid_.remove(it.value());
    byId_.erase(it);
}

void Notifier::onActionInvoked(uint nid, const QString& action) {
    Q_UNUSED(action);
    if (auto it = byNid_.find(nid); it != byNid_.end()) emit activated(it.value());
}

void Notifier::onClosed(uint nid, uint reason) {
    Q_UNUSED(reason);
    if (auto it = byNid_.find(nid); it != byNid_.end()) {
        byId_.remove(it.value());
        byNid_.erase(it);
    }
}
