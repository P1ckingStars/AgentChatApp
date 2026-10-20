#pragma once

#include <QJsonArray>
#include <QTextBrowser>

// Renders normalized transcript messages as chat bubbles; tool calls are compact one-liners.
class ChatView : public QTextBrowser {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);

    void clearChat(bool truncated = false);
    void appendMessages(const QJsonArray& msgs);

signals:
    void titleSeen(const QString& title);

private:
    void appendOne(const QJsonObject& m);

    qint64 lastOffset_ = -1;
};
