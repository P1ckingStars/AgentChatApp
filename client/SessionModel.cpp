#include "SessionModel.h"

#include <QApplication>
#include <QDateTime>
#include <QPainter>

QColor stateColor(const QString& state, bool unread) {
    if (state == "blocked") return QColor(0xe5, 0x53, 0x4b);
    if (state == "waiting" && unread) return QColor(0xd4, 0xa7, 0x2c);
    if (state == "running" || state == "starting") return QColor(0x57, 0xab, 0x5a);
    if (state == "exited") return QColor(0x76, 0x76, 0x76, 0x90);
    return QColor(0x76, 0x83, 0x90);
}

QString stateLabel(const QString& state) {
    if (state == "blocked") return "needs permission";
    if (state == "waiting") return "waiting for you";
    if (state == "running") return "running";
    if (state == "starting") return "starting";
    if (state == "idle") return "idle";
    if (state == "exited") return "exited";
    return state;
}

namespace {

int rank(const QString& state, bool unread) {
    if (state == "blocked") return 0;
    if (unread) return 1;
    if (state == "running" || state == "starting") return 2;
    if (state == "exited") return 4;
    return 3;
}

QString relTime(qint64 ms) {
    qint64 s = (QDateTime::currentMSecsSinceEpoch() - ms) / 1000;
    if (s < 60) return "now";
    if (s < 3600) return QString("%1m").arg(s / 60);
    if (s < 86400) return QString("%1h").arg(s / 3600);
    return QString("%1d").arg(s / 86400);
}

}  // namespace

int SessionModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : rows_.size(); }

QVariant SessionModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const Session& s = rows_[index.row()];
    switch (role) {
        case Qt::DisplayRole: return s.name().isEmpty() ? s.key() : s.name();
        case Qt::ToolTipRole:
            return QString("%1 — %2\n%3\n%4").arg(s.name(), stateLabel(s.state()), s.cwd(), s.title());
        case IdRole: return s.id();
        case HostRole: return s.host;
        case StateRole: return s.state();
        case UnreadRole: return s.unread();
        case DetailRole: return s.detail();
        case TitleRole: return s.title();
        case UpdatedRole: return s.data["updated"].toInteger();
        case RankRole: return rank(s.state(), s.unread());
        case OnlineRole: return hostOnline(s.host);
        default: return {};
    }
}

int SessionModel::rowOf(const QString& id) const {
    for (int i = 0; i < rows_.size(); ++i)
        if (rows_[i].id() == id) return i;
    return -1;
}

void SessionModel::replaceHost(const QString& host, const QList<QJsonObject>& sessions) {
    QSet<QString> incoming;
    for (const QJsonObject& s : sessions) incoming.insert(Session::makeId(host, s["key"].toString()));
    for (int i = rows_.size() - 1; i >= 0; --i) {
        if (rows_[i].host == host && !incoming.contains(rows_[i].id())) {
            beginRemoveRows({}, i, i);
            rows_.removeAt(i);
            endRemoveRows();
        }
    }
    for (const QJsonObject& s : sessions) upsert(host, s);
}

void SessionModel::upsert(const QString& host, const QJsonObject& session) {
    int r = rowOf(Session::makeId(host, session["key"].toString()));
    if (r >= 0) {
        rows_[r].data = session;
        emit dataChanged(index(r), index(r));
        return;
    }
    beginInsertRows({}, rows_.size(), rows_.size());
    rows_.append({host, session});
    endInsertRows();
}

void SessionModel::remove(const QString& host, const QString& key) {
    int r = rowOf(Session::makeId(host, key));
    if (r < 0) return;
    beginRemoveRows({}, r, r);
    rows_.removeAt(r);
    endRemoveRows();
}

void SessionModel::setHostOnline(const QString& host, bool online) {
    if (online == hostOnline(host)) return;
    if (online)
        offline_.remove(host);
    else
        offline_.insert(host);
    if (!rows_.isEmpty()) emit dataChanged(index(0), index(rows_.size() - 1), {OnlineRole});
}

void SessionModel::clear() {
    beginResetModel();
    rows_.clear();
    offline_.clear();
    endResetModel();
}

const Session* SessionModel::find(const QString& id) const {
    int r = rowOf(id);
    return r >= 0 ? &rows_[r] : nullptr;
}

const Session* SessionModel::at(int row) const {
    return row >= 0 && row < rows_.size() ? &rows_[row] : nullptr;
}

QModelIndex SessionModel::indexOf(const QString& id) const {
    int r = rowOf(id);
    return r >= 0 ? index(r) : QModelIndex();
}

int SessionModel::attentionCount() const {
    int n = 0;
    for (const Session& s : rows_)
        if (s.needsAttention()) ++n;
    return n;
}

bool SessionSortProxy::lessThan(const QModelIndex& l, const QModelIndex& r) const {
    int rl = l.data(SessionModel::RankRole).toInt(), rr = r.data(SessionModel::RankRole).toInt();
    if (rl != rr) return rl < rr;
    return l.data(SessionModel::UpdatedRole).toLongLong() > r.data(SessionModel::UpdatedRole).toLongLong();
}

QSize SessionDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
    QFontMetrics fm(option.font);
    return {200, fm.height() * 2 + 18};
}

void SessionDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& idx) const {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, idx);
    QString name = opt.text;
    opt.text.clear();
    const QWidget* w = opt.widget;
    QStyle* style = w ? w->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (!idx.data(SessionModel::OnlineRole).toBool()) p->setOpacity(0.45);

    QString state = idx.data(SessionModel::StateRole).toString();
    bool unread = idx.data(SessionModel::UnreadRole).toBool();
    bool selected = opt.state & QStyle::State_Selected;
    QColor fg = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    QColor muted = fg;
    muted.setAlphaF(0.62);

    QFont bold = opt.font;
    bold.setBold(true);
    QFontMetrics fm(opt.font), bfm(bold);
    QRect r = opt.rect.adjusted(10, 8, -10, -8);
    int lh = fm.height();
    int line1 = r.top(), line2 = r.top() + lh + 2;

    p->setPen(Qt::NoPen);
    p->setBrush(stateColor(state, unread));
    p->drawEllipse(QRectF(r.left(), line1 + (lh - 9) / 2.0, 9, 9));
    int x = r.left() + 17;

    QString right = idx.data(SessionModel::HostRole).toString() + " · " +
                    relTime(idx.data(SessionModel::UpdatedRole).toLongLong());
    int rw = fm.horizontalAdvance(right);
    p->setFont(opt.font);
    p->setPen(muted);
    p->drawText(QRect(r.right() - rw, line1, rw, lh), Qt::AlignRight | Qt::AlignVCenter, right);

    int nameW = r.right() - rw - 8 - x;
    p->setFont(unread || state == "blocked" ? bold : opt.font);
    p->setPen(fg);
    p->drawText(QRect(x, line1, nameW, lh), Qt::AlignLeft | Qt::AlignVCenter,
                bfm.elidedText(name, Qt::ElideRight, nameW));

    // Preview line, like the last message in a chat app.
    QString preview = idx.data(SessionModel::DetailRole).toString().simplified();
    if (preview.isEmpty()) preview = idx.data(SessionModel::TitleRole).toString();
    if (preview.isEmpty()) preview = stateLabel(state);
    bool badge = unread || state == "blocked";
    int pw = r.right() - x - (badge ? 18 : 0);
    p->setFont(opt.font);
    p->setPen(muted);
    p->drawText(QRect(x, line2, pw, lh), Qt::AlignLeft | Qt::AlignVCenter,
                fm.elidedText(preview, Qt::ElideRight, pw));
    if (badge) {
        p->setPen(Qt::NoPen);
        p->setBrush(stateColor(state, true));
        p->drawEllipse(QRectF(r.right() - 10, line2 + (lh - 10) / 2.0, 10, 10));
    }
    p->restore();
}
