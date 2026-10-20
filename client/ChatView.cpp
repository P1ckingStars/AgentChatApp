#include "ChatView.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QJsonObject>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextFrame>

namespace {

QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

QString shortTime(const QString& iso) {
    QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return t.isValid() ? t.toLocalTime().toString("HH:mm") : QString();
}

QString firstLines(const QString& text, int lines, int maxChars) {
    QStringList parts = text.split('\n');
    QString out = parts.mid(0, lines).join('\n');
    if (out.size() > maxChars) out = out.left(maxChars) + "…";
    else if (parts.size() > lines) out += "\n…";
    return out;
}

// Start a new root-level block, reusing the trailing empty block Qt leaves after frames.
QTextCursor rootBlock(QTextDocument* doc, const QTextBlockFormat& bf) {
    QTextCursor c = doc->rootFrame()->lastCursorPosition();
    if (c.block().text().isEmpty() && c.block().length() <= 1)
        c.setBlockFormat(bf);
    else
        c.insertBlock(bf, QTextCharFormat());
    return c;
}

}  // namespace

ChatView::ChatView(QWidget* parent) : QTextBrowser(parent) {
    setOpenExternalLinks(true);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(12);
}

void ChatView::clearChat(bool truncated) {
    clear();
    lastOffset_ = -1;
    if (truncated) {
        QTextBlockFormat bf;
        bf.setAlignment(Qt::AlignHCenter);
        QTextCharFormat cf;
        cf.setForeground(mix(palette().text().color(), palette().base().color(), 0.5));
        cf.setFontItalic(true);
        rootBlock(document(), bf).insertText("earlier messages not shown", cf);
    }
}

void ChatView::appendMessages(const QJsonArray& msgs) {
    QScrollBar* sb = verticalScrollBar();
    bool atBottom = sb->value() >= sb->maximum() - 8;
    for (const QJsonValue& v : msgs) appendOne(v.toObject());
    if (atBottom) sb->setValue(sb->maximum());
}

void ChatView::appendOne(const QJsonObject& m) {
    qint64 offset = m["offset"].toInteger();
    if (offset <= lastOffset_) return;  // already shown (e.g. replay after reconnect)
    lastOffset_ = offset;

    const QString kind = m["kind"].toString();
    const QString text = m["text"].toString();
    if (kind == "title") {
        emit titleSeen(text);
        return;
    }

    const QColor base = palette().base().color();
    const QColor fg = palette().text().color();
    const QColor muted = mix(fg, base, 0.45);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    QTextCharFormat mutedFmt;
    mutedFmt.setForeground(muted);

    if (kind == "user" || kind == "assistant") {
        bool user = kind == "user";
        QTextFrameFormat ff;
        ff.setBorder(0);
        ff.setPadding(10);
        ff.setTopMargin(10);
        ff.setBackground(user ? mix(base, palette().highlight().color(), 0.16) : mix(base, fg, 0.05));
        if (user)
            ff.setLeftMargin(80);
        else
            ff.setRightMargin(40);

        QTextCursor c = document()->rootFrame()->lastCursorPosition();
        c.insertFrame(ff);
        QTextCharFormat who = mutedFmt;
        who.setFontWeight(QFont::Bold);
        c.insertText(user ? "You" : "Claude", who);
        c.insertText("   " + shortTime(m["ts"].toString()), mutedFmt);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        if (user)
            c.insertText(text, QTextCharFormat());
        else
            c.insertMarkdown(text, QTextDocument::MarkdownDialectGitHub);
        return;
    }

    QTextBlockFormat bf;
    bf.setTopMargin(3);
    if (kind == "tool_use") {
        bf.setLeftMargin(14);
        QTextCursor c = rootBlock(document(), bf);
        QTextCharFormat name = mutedFmt;
        name.setFontWeight(QFont::Bold);
        c.insertText("⚙ " + m["tool"].toString() + "  ", name);
        QTextCharFormat arg = mutedFmt;
        arg.setFont(mono);
        c.insertText(firstLines(text, 1, 200), arg);
    } else if (kind == "tool_result") {
        bf.setLeftMargin(34);
        QTextCursor c = rootBlock(document(), bf);
        QTextCharFormat out = mutedFmt;
        out.setFont(mono);
        if (m["error"].toBool()) out.setForeground(QColor(0xd0, 0x50, 0x50));
        c.insertText("↳ " + firstLines(text.trimmed(), 3, 400), out);
    } else if (kind == "command") {
        bf.setAlignment(Qt::AlignHCenter);
        bf.setTopMargin(8);
        QTextCharFormat cf = mutedFmt;
        cf.setFontItalic(true);
        rootBlock(document(), bf).insertText("— " + text + " —", cf);
    } else if (kind == "system") {
        bf.setTopMargin(8);
        QTextCharFormat cf = mutedFmt;
        cf.setFontItalic(true);
        rootBlock(document(), bf).insertText("※ " + text, cf);
    }
}
