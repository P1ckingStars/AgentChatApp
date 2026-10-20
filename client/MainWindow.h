#pragma once

#include <QMainWindow>
#include <QTimer>

#include "HostConnection.h"
#include "SessionModel.h"

class ChatView;
class Notifier;
class QFrame;
class QLabel;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QSystemTrayIcon;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void buildUi();
    void buildTray();
    void setupHosts();
    void teardownHosts();

    HostConnection* hostNamed(const QString& name) const;
    const Session* current() const;

    void onCurrentChanged();
    void onSnapshot(HostConnection* h, const QList<QJsonObject>& sessions, qint64 seq);
    void onSessionUpdated(HostConnection* h, const QString& kind, const QJsonObject& session, qint64 seq);
    void onTranscript(HostConnection* h, const QString& key, bool reset, bool truncated, const QJsonArray& msgs);

    void refreshHeader();
    void refreshHostStatus();
    void refreshAttention();
    void refreshScreen();
    void maybeMarkRead();
    void notifyFor(const Session& s, const QString& kind);
    void selectId(const QString& id);
    void showWindow();

    void sendInput();
    void sendKeys(const QStringList& keys);
    void attach();
    void resumeSession();
    void killSession();
    void removeSession();
    void newSession();
    void openSettings();
    void showError(const QString& msg);

    qint64 lastNotified(const QString& host) const;
    void setLastNotified(const QString& host, qint64 seq);

    SessionModel* model_;
    SessionSortProxy* proxy_;
    QListView* list_ = nullptr;
    QLabel* hostStatus_ = nullptr;

    QWidget* right_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QPushButton* attachBtn_ = nullptr;
    QPushButton* peekBtn_ = nullptr;
    QPushButton* resumeBtn_ = nullptr;
    QPushButton* killBtn_ = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QFrame* screenPanel_ = nullptr;
    QLabel* screenLabel_ = nullptr;
    QPlainTextEdit* screen_ = nullptr;
    ChatView* chat_ = nullptr;
    QPlainTextEdit* input_ = nullptr;
    QPushButton* sendBtn_ = nullptr;

    QSystemTrayIcon* tray_ = nullptr;
    Notifier* notifier_;
    QList<HostConnection*> hosts_;
    QString terminal_;

    QString currentHost_, currentKey_;
    QString pendingSelect_;
    QTimer screenTimer_;
    QTimer clockTimer_;
    int lastAttention_ = -1;
};
