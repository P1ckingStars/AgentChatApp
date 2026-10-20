#pragma once

#include <QAbstractListModel>
#include <QJsonObject>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>

struct Session {
    QString host;
    QJsonObject data;  // row as sent by `ccbus serve`

    static QString makeId(const QString& host, const QString& key) { return host + QChar(0x1f) + key; }
    QString id() const { return makeId(host, key()); }
    QString key() const { return data["key"].toString(); }
    QString name() const { return data["name"].toString(); }
    QString state() const { return data["state"].toString(); }
    QString pane() const { return data["pane"].toString(); }
    QString title() const { return data["title"].toString(); }
    QString detail() const { return data["detail"].toString(); }
    QString cwd() const { return data["cwd"].toString(); }
    bool unread() const { return data["unread"].toBool(); }
    bool needsAttention() const { return state() == "blocked" || unread(); }
};

QColor stateColor(const QString& state, bool unread);
QString stateLabel(const QString& state);

class SessionModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        HostRole,
        StateRole,
        UnreadRole,
        DetailRole,
        TitleRole,
        UpdatedRole,
        RankRole,
        OnlineRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void replaceHost(const QString& host, const QList<QJsonObject>& sessions);
    void upsert(const QString& host, const QJsonObject& session);
    void remove(const QString& host, const QString& key);
    void setHostOnline(const QString& host, bool online);
    void clear();

    const Session* find(const QString& id) const;
    const Session* at(int row) const;
    QModelIndex indexOf(const QString& id) const;
    int attentionCount() const;
    bool hostOnline(const QString& host) const { return !offline_.contains(host); }

private:
    int rowOf(const QString& id) const;

    QList<Session> rows_;
    QSet<QString> offline_;
};

// Chat-app ordering: needs-you first, then running, then quiet, then exited; newest first.
class SessionSortProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
};

class SessionDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
