#include "MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QToolButton>
#include <QVBoxLayout>

#include "ChatView.h"
#include "Dialogs.h"
#include "Notifier.h"

namespace {

QIcon makeIcon(int badge) {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xd9, 0x77, 0x57));
    p.drawRoundedRect(QRectF(4, 4, 56, 56), 14, 14);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(26);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(4, 4, 56, 56), Qt::AlignCenter, "cc");
    if (badge > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe5, 0x53, 0x4b));
        p.drawEllipse(QRectF(34, 0, 30, 30));
        f.setPixelSize(18);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(34, 0, 30, 30), Qt::AlignCenter, badge > 9 ? "9+" : QString::number(badge));
    }
    return QIcon(pm);
}

QPushButton* smallButton(const QString& text, const QString& tip) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    b->setAutoDefault(false);
    return b;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), model_(new SessionModel(this)), proxy_(new SessionSortProxy(this)),
      notifier_(new Notifier(this)) {
    proxy_->setSourceModel(model_);
    proxy_->setDynamicSortFilter(true);
    proxy_->sort(0);
    terminal_ = loadTerminal();

    buildUi();
    buildTray();

    connect(notifier_, &Notifier::activated, this, [this](const QString& id) {
        showWindow();
        selectId(id);
    });
    connect(model_, &QAbstractItemModel::dataChanged, this, [this] {
        refreshHeader();
        refreshAttention();
    });
    connect(model_, &QAbstractItemModel::rowsRemoved, this, &MainWindow::refreshAttention);
    connect(model_, &QAbstractItemModel::rowsInserted, this, &MainWindow::refreshAttention);

    screenTimer_.setInterval(1500);
    connect(&screenTimer_, &QTimer::timeout, this, &MainWindow::refreshScreen);
    clockTimer_.setInterval(30000);  // relative timestamps in the list
    connect(&clockTimer_, &QTimer::timeout, this, [this] { list_->viewport()->update(); });
    clockTimer_.start();

    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());
    if (auto* split = findChild<QSplitter*>()) split->restoreState(s.value("splitter").toByteArray());

    setupHosts();
    refreshHeader();
    refreshAttention();
}

MainWindow::~MainWindow() { teardownHosts(); }

void MainWindow::buildUi() {
    setWindowTitle("ccbus");
    setWindowIcon(makeIcon(0));
    resize(1100, 720);

    // Left: session list.
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(6, 6, 0, 6);
    auto* topRow = new QHBoxLayout;
    auto* newBtn = smallButton("+ New session", "Start claude in a new tmux session (Ctrl+N)");
    newBtn->setShortcut(QKeySequence::New);
    connect(newBtn, &QPushButton::clicked, this, &MainWindow::newSession);
    auto* settingsBtn = new QToolButton;
    settingsBtn->setText("⚙");
    settingsBtn->setToolTip("Hosts and settings");
    connect(settingsBtn, &QToolButton::clicked, this, &MainWindow::openSettings);
    topRow->addWidget(newBtn, 1);
    topRow->addWidget(settingsBtn);
    leftLayout->addLayout(topRow);

    list_ = new QListView;
    list_->setModel(proxy_);
    list_->setItemDelegate(new SessionDelegate(list_));
    list_->setUniformItemSizes(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumWidth(240);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onCurrentChanged);
    connect(list_, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!list_->indexAt(pos).isValid()) return;
        QMenu menu;
        menu.addAction("Attach in terminal", this, &MainWindow::attach);
        menu.addAction("Resume", this, &MainWindow::resumeSession);
        menu.addSeparator();
        menu.addAction("Kill", this, &MainWindow::killSession);
        menu.addAction("Remove from list", this, &MainWindow::removeSession);
        menu.exec(list_->viewport()->mapToGlobal(pos));
    });
    leftLayout->addWidget(list_, 1);
    hostStatus_ = new QLabel;
    hostStatus_->setWordWrap(true);
    hostStatus_->setTextFormat(Qt::RichText);
    leftLayout->addWidget(hostStatus_);

    // Right: header, terminal peek / permission panel, chat, input.
    right_ = new QWidget;
    auto* rightLayout = new QVBoxLayout(right_);
    rightLayout->setContentsMargins(6, 6, 6, 6);

    auto* header = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    title_ = new QLabel;
    title_->setTextFormat(Qt::RichText);
    QFont tf = title_->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.2);
    title_->setFont(tf);
    subtitle_ = new QLabel;
    subtitle_->setTextFormat(Qt::RichText);
    subtitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Long paths must clip rather than widen the window.
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    subtitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titles->addWidget(title_);
    titles->addWidget(subtitle_);
    header->addLayout(titles, 1);

    attachBtn_ = smallButton("Attach", "Open the real Claude UI in a terminal (Ctrl+T)");
    attachBtn_->setShortcut(QKeySequence("Ctrl+T"));
    peekBtn_ = smallButton("Screen", "Show the live tmux screen (Ctrl+P)");
    peekBtn_->setCheckable(true);
    peekBtn_->setShortcut(QKeySequence("Ctrl+P"));
    resumeBtn_ = smallButton("Resume", "Restart claude with --resume in this session");
    killBtn_ = smallButton("Kill", "Kill the tmux pane");
    removeBtn_ = smallButton("Remove", "Remove from the list");
    connect(attachBtn_, &QPushButton::clicked, this, &MainWindow::attach);
    connect(peekBtn_, &QPushButton::toggled, this, &MainWindow::refreshHeader);
    connect(resumeBtn_, &QPushButton::clicked, this, &MainWindow::resumeSession);
    connect(killBtn_, &QPushButton::clicked, this, &MainWindow::killSession);
    connect(removeBtn_, &QPushButton::clicked, this, &MainWindow::removeSession);
    for (auto* b : {attachBtn_, peekBtn_, resumeBtn_, killBtn_, removeBtn_}) header->addWidget(b);
    rightLayout->addLayout(header);

    screenPanel_ = new QFrame;
    screenPanel_->setFrameShape(QFrame::StyledPanel);
    auto* screenLayout = new QVBoxLayout(screenPanel_);
    screenLabel_ = new QLabel;
    screenLabel_->setWordWrap(true);
    screen_ = new QPlainTextEdit;
    screen_->setReadOnly(true);
    screen_->setLineWrapMode(QPlainTextEdit::NoWrap);
    screen_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    screen_->setMinimumHeight(180);
    auto* keys = new QHBoxLayout;
    keys->addWidget(new QLabel("Send key:"));
    const QList<QPair<QString, QStringList>> keyButtons = {
        {"1", {"1"}}, {"2", {"2"}}, {"3", {"3"}}, {"↑", {"Up"}}, {"↓", {"Down"}},
        {"Enter", {"Enter"}}, {"Esc", {"Escape"}}, {"Ctrl+C", {"C-c"}},
    };
    for (const auto& [label, k] : keyButtons) {
        auto* b = smallButton(label, "tmux send-keys " + k.join(' '));
        connect(b, &QPushButton::clicked, this, [this, k = k] { sendKeys(k); });
        keys->addWidget(b);
    }
    keys->addStretch();
    screenLayout->addWidget(screenLabel_);
    screenLayout->addWidget(screen_, 1);
    screenLayout->addLayout(keys);
    rightLayout->addWidget(screenPanel_, 2);

    chat_ = new ChatView;
    connect(chat_, &ChatView::titleSeen, this, [this](const QString&) { refreshHeader(); });
    rightLayout->addWidget(chat_, 5);

    auto* inputRow = new QHBoxLayout;
    input_ = new QPlainTextEdit;
    input_->setPlaceholderText("Message Claude — Enter to send, Shift+Enter for newline");
    input_->setMaximumHeight(110);
    input_->installEventFilter(this);
    sendBtn_ = smallButton("Send", "Send to the session (Enter)");
    connect(sendBtn_, &QPushButton::clicked, this, &MainWindow::sendInput);
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(sendBtn_, 0, Qt::AlignBottom);
    rightLayout->addLayout(inputRow);

    auto* split = new QSplitter;
    split->addWidget(left);
    split->addWidget(right_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({320, 780});
    setCentralWidget(split);
    statusBar();
}

void MainWindow::buildTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_ = new QSystemTrayIcon(makeIcon(0), this);
    auto* menu = new QMenu(this);
    menu->addAction("Show", this, &MainWindow::showWindow);
    menu->addAction("New session…", this, [this] {
        showWindow();
        newSession();
    });
    menu->addSeparator();
    menu->addAction("Quit", qApp, &QApplication::quit);
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger) return;
        if (isVisible() && isActiveWindow())
            hide();
        else
            showWindow();
    });
    tray_->show();
    qApp->setQuitOnLastWindowClosed(false);
}

void MainWindow::setupHosts() {
    for (const HostConfig& cfg : loadHosts()) {
        auto* h = new HostConnection(cfg, this);
        connect(h, &HostConnection::connectionChanged, this, [this, h](bool up) {
            model_->setHostOnline(h->name(), up);
            refreshHostStatus();
            refreshHeader();
        });
        connect(h, &HostConnection::snapshot, this,
                [this, h](const QList<QJsonObject>& s, qint64 seq) { onSnapshot(h, s, seq); });
        connect(h, &HostConnection::sessionUpdated, this,
                [this, h](const QString& kind, const QJsonObject& s, qint64 seq) { onSessionUpdated(h, kind, s, seq); });
        connect(h, &HostConnection::sessionRemoved, this,
                [this, h](const QString& key) { model_->remove(h->name(), key); });
        connect(h, &HostConnection::transcript, this,
                [this, h](const QString& key, bool reset, bool truncated, const QJsonArray& msgs) {
                    onTranscript(h, key, reset, truncated, msgs);
                });
        connect(h, &HostConnection::requestFailed, this, [this, h](const QString& op, const QString& err) {
            if (op != "ping") showError(QString("%1: %2 failed: %3").arg(h->name(), op, err));
        });
        hosts_.append(h);
        h->start();
    }
    refreshHostStatus();
}

void MainWindow::teardownHosts() {
    qDeleteAll(hosts_);
    hosts_.clear();
}

HostConnection* MainWindow::hostNamed(const QString& name) const {
    for (HostConnection* h : hosts_)
        if (h->name() == name) return h;
    return nullptr;
}

const Session* MainWindow::current() const {
    if (currentKey_.isEmpty()) return nullptr;
    return model_->find(Session::makeId(currentHost_, currentKey_));
}

void MainWindow::onCurrentChanged() {
    QModelIndex idx = proxy_->mapToSource(list_->currentIndex());
    const Session* s = model_->at(idx.row());
    QString host = s ? s->host : QString(), key = s ? s->key() : QString();
    if (host == currentHost_ && key == currentKey_) return;

    if (HostConnection* old = hostNamed(currentHost_); old && old->name() != host) old->unwatch();
    currentHost_ = host;
    currentKey_ = key;
    chat_->clearChat();
    screen_->clear();
    peekBtn_->setChecked(false);
    if (HostConnection* h = hostNamed(host)) h->watch(key);
    refreshHeader();
    maybeMarkRead();
    input_->setFocus();
}

void MainWindow::onSnapshot(HostConnection* h, const QList<QJsonObject>& sessions, qint64 seq) {
    model_->replaceHost(h->name(), sessions);
    // Tell the user about anything that finished while we were disconnected.
    qint64 seen = lastNotified(h->name());
    if (seen > seq) seen = 0;  // server database was reset
    QList<const Session*> missed;
    for (const QJsonObject& o : sessions) {
        const Session* s = model_->find(Session::makeId(h->name(), o["key"].toString()));
        if (s && s->needsAttention() && o["attention_seq"].toInteger() > seen) missed << s;
    }
    if (missed.size() == 1)
        notifyFor(*missed.first(), missed.first()->state() == "blocked" ? "blocked" : "stop");
    else if (missed.size() > 1)
        notifier_->notify("summary:" + h->name(), QString("%1 Claude sessions need you").arg(missed.size()),
                          "on " + h->name(), false);
    setLastNotified(h->name(), seq);
    if (!pendingSelect_.isEmpty() && model_->find(pendingSelect_)) selectId(pendingSelect_);
}

void MainWindow::onSessionUpdated(HostConnection* h, const QString& kind, const QJsonObject& session, qint64 seq) {
    model_->upsert(h->name(), session);
    QString id = Session::makeId(h->name(), session["key"].toString());
    const Session* s = model_->find(id);
    if (!s) return;

    if ((kind == "stop" || kind == "blocked") && seq > lastNotified(h->name())) {
        bool looking = isActiveWindow() && id == Session::makeId(currentHost_, currentKey_);
        if (looking)
            maybeMarkRead();
        else
            notifyFor(*s, kind);
    } else if (kind == "read" || kind == "prompt" || kind == "resumed" || kind == "gone" || kind == "end") {
        notifier_->close(id);
    }
    setLastNotified(h->name(), qMax(seq, lastNotified(h->name())));

    if (id == pendingSelect_) selectId(id);
}

void MainWindow::onTranscript(HostConnection* h, const QString& key, bool reset, bool truncated,
                              const QJsonArray& msgs) {
    if (h->name() != currentHost_ || key != currentKey_) return;
    if (reset) chat_->clearChat(truncated);
    chat_->appendMessages(msgs);
    if (reset) chat_->verticalScrollBar()->setValue(chat_->verticalScrollBar()->maximum());
}

void MainWindow::notifyFor(const Session& s, const QString& kind) {
    bool blocked = kind == "blocked";
    QString summary = s.name() + (blocked ? " needs permission" : " is waiting for you");
    QString body = s.detail().isEmpty() ? s.title() : s.detail();
    if (hosts_.size() > 1) body = "[" + s.host + "] " + body;
    notifier_->notify(s.id(), summary, body, blocked);
    QApplication::alert(this);  // urgency hint for the window manager
}

void MainWindow::refreshHeader() {
    const Session* s = current();
    right_->setEnabled(s != nullptr);
    if (!s) {
        title_->setText("<i>No session selected</i>");
        subtitle_->clear();
        screenPanel_->hide();
        screenTimer_.stop();
        setWindowTitle(windowTitle());
        return;
    }
    HostConnection* h = hostNamed(s->host);
    bool online = h && h->isConnected();
    QString state = s->state();
    bool hasPane = !s->pane().isEmpty();
    bool alive = hasPane && state != "exited";

    QString t = "<b>" + s->name().toHtmlEscaped() + "</b>";
    if (!s->title().isEmpty()) t += " &nbsp;<span style='color:gray'>" + s->title().toHtmlEscaped() + "</span>";
    title_->setText(t);
    QColor c = stateColor(state, s->unread());
    QString cwd = s->cwd();
    if (h && h->config().isLocal() && cwd.startsWith(QDir::homePath())) cwd = "~" + cwd.mid(QDir::homePath().size());
    if (cwd.size() > 60) cwd = "…/" + cwd.section('/', -2);
    subtitle_->setToolTip(s->cwd());
    subtitle_->setText(QString("<span style='color:%1'>●</span> %2 &nbsp;·&nbsp; %3 &nbsp;·&nbsp; %4%5")
                           .arg(c.name(), stateLabel(state), s->host.toHtmlEscaped(), cwd.toHtmlEscaped(),
                                hasPane ? " &nbsp;·&nbsp; tmux " + s->pane() : QString(" &nbsp;·&nbsp; not in tmux")) +
                       (online ? QString() : " &nbsp;<b>(host offline)</b>"));

    attachBtn_->setEnabled(hasPane && online);
    peekBtn_->setEnabled(hasPane && online);
    resumeBtn_->setEnabled(online && state == "exited" && !s->data["claude_session"].toString().isEmpty());
    killBtn_->setEnabled(online && alive);
    removeBtn_->setEnabled(online);
    input_->setEnabled(online && alive);
    sendBtn_->setEnabled(online && alive);

    // Startup can stop at prompts no hook reports (e.g. "trust this folder?"), so show the screen.
    bool showScreen = online && hasPane && (state == "blocked" || state == "starting" || peekBtn_->isChecked());
    if (state == "blocked")
        screenLabel_->setText("<b style='color:#e5534b'>Permission needed:</b> " + s->detail().toHtmlEscaped());
    else if (state == "starting")
        screenLabel_->setText("<b>Starting…</b> answer any startup prompt with the keys below");
    else
        screenLabel_->setText("<b>Live screen</b>");
    if (showScreen != screenPanel_->isVisible()) {
        screenPanel_->setVisible(showScreen);
        if (showScreen) {
            refreshScreen();
            screenTimer_.start();
        } else {
            screenTimer_.stop();
        }
    }
}

void MainWindow::refreshScreen() {
    const Session* s = current();
    HostConnection* h = s ? hostNamed(s->host) : nullptr;
    if (!h || !h->isConnected() || s->pane().isEmpty()) return;
    QString id = s->id();
    h->request("capture", {{"key", s->key()}}, [this, id](const QJsonObject& r) {
        if (id != Session::makeId(currentHost_, currentKey_)) return;
        QStringList lines = r["text"].toString().split('\n');
        for (QString& l : lines) {
            qsizetype n = l.size();
            while (n > 0 && l[n - 1].isSpace()) --n;
            l.truncate(n);
        }
        while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
        QString text = lines.join('\n');
        if (text == screen_->toPlainText()) return;
        screen_->setPlainText(text);
        // Prompts live at the bottom-left of the pane.
        screen_->horizontalScrollBar()->setValue(0);
        screen_->verticalScrollBar()->setValue(screen_->verticalScrollBar()->maximum());
    });
}

void MainWindow::refreshHostStatus() {
    QStringList parts;
    for (HostConnection* h : hosts_) {
        if (h->isConnected())
            parts << "<span style='color:#57ab5a'>●</span> " + h->name().toHtmlEscaped();
        else
            parts << "<span style='color:#e5534b'>●</span> " + h->name().toHtmlEscaped() +
                         " <small style='color:gray'>" + h->lastError().toHtmlEscaped() + "</small>";
    }
    hostStatus_->setText(parts.join("<br>"));
}

void MainWindow::refreshAttention() {
    int n = model_->attentionCount();
    if (n == lastAttention_) return;
    lastAttention_ = n;
    setWindowTitle(n ? QString("(%1) ccbus").arg(n) : QString("ccbus"));
    QIcon icon = makeIcon(n);
    setWindowIcon(icon);
    if (tray_) {
        tray_->setIcon(icon);
        tray_->setToolTip(n ? QString("%1 Claude session(s) need you").arg(n) : QString("ccbus"));
    }
}

void MainWindow::maybeMarkRead() {
    const Session* s = current();
    if (!s || !s->unread() || !isActiveWindow() || isMinimized()) return;
    if (HostConnection* h = hostNamed(s->host); h && h->isConnected()) h->request("mark_read", {{"key", s->key()}});
    notifier_->close(s->id());
}

void MainWindow::selectId(const QString& id) {
    QModelIndex src = model_->indexOf(id);
    if (!src.isValid()) {
        pendingSelect_ = id;  // not arrived yet
        return;
    }
    pendingSelect_.clear();
    list_->setCurrentIndex(proxy_->mapFromSource(src));
}

void MainWindow::showWindow() {
    show();
    if (isMinimized()) showNormal();
    raise();
    activateWindow();
}

void MainWindow::sendInput() {
    const Session* s = current();
    HostConnection* h = s ? hostNamed(s->host) : nullptr;
    QString text = input_->toPlainText();
    if (!h || text.trimmed().isEmpty()) return;
    h->request("send", {{"key", s->key()}, {"text", text}});
    input_->clear();
}

void MainWindow::sendKeys(const QStringList& keys) {
    const Session* s = current();
    HostConnection* h = s ? hostNamed(s->host) : nullptr;
    if (!h) return;
    h->request("keys", {{"key", s->key()}, {"keys", QJsonArray::fromStringList(keys)}},
               [this](const QJsonObject&) { QTimer::singleShot(300, this, &MainWindow::refreshScreen); });
}

void MainWindow::attach() {
    const Session* s = current();
    HostConnection* h = s ? hostNamed(s->host) : nullptr;
    if (!h || s->pane().isEmpty()) return;
    QStringList argv = QProcess::splitCommand(terminal_) + h->attachArgv(s->pane());
    QProcess p;
    p.setProgram(argv.takeFirst());
    p.setArguments(argv);
    // A tmux client refuses to start nested inside another tmux session.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("TMUX");
    env.remove("TMUX_PANE");
    p.setProcessEnvironment(env);
    if (!p.startDetached()) showError("Could not start terminal: " + terminal_ + " (change it in settings)");
}

void MainWindow::resumeSession() {
    const Session* s = current();
    HostConnection* h = s ? hostNamed(s->host) : nullptr;
    if (!h) return;
    QString host = h->name();
    h->request("resume", {{"key", s->key()}}, [this, host](const QJsonObject& r) {
        selectId(Session::makeId(host, r["key"].toString()));
        statusBar()->showMessage("Resumed " + r["key"].toString(), 4000);
    });
}

void MainWindow::killSession() {
    const Session* s = current();
    HostConnection* h = s ? hostNamed(s->host) : nullptr;
    if (!h) return;
    if (QMessageBox::question(this, "Kill session", "Kill tmux pane for " + s->name() + "? Claude will be stopped.") !=
        QMessageBox::Yes)
        return;
    h->request("kill", {{"key", s->key()}});
}

void MainWindow::removeSession() {
    const Session* s = current();
    HostConnection* h = s ? hostNamed(s->host) : nullptr;
    if (!h) return;
    if (s->state() != "exited" &&
        QMessageBox::question(this, "Remove session",
                              s->name() + " is still running. Remove it from the list anyway? "
                                          "(It reappears on its next hook event.)") != QMessageBox::Yes)
        return;
    h->request("remove", {{"key", s->key()}});
}

void MainWindow::newSession() {
    QStringList names;
    for (HostConnection* h : hosts_) names << h->name();
    NewSessionDialog dlg(names, currentHost_.isEmpty() ? names.value(0) : currentHost_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    HostConnection* h = hostNamed(dlg.host());
    if (!h) return;
    QString host = h->name();
    h->request("new", {{"name", dlg.name()}, {"cwd", dlg.cwd()}, {"args", QJsonArray::fromStringList(dlg.args())}},
               [this, host](const QJsonObject& r) { selectId(Session::makeId(host, r["key"].toString())); });
}

void MainWindow::openSettings() {
    SettingsDialog dlg(loadHosts(), terminal_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    saveHosts(dlg.hosts());
    terminal_ = dlg.terminal();
    saveTerminal(terminal_);
    // Simplest correct thing: reconnect everything from scratch.
    if (HostConnection* h = hostNamed(currentHost_)) h->unwatch();
    currentHost_.clear();
    currentKey_.clear();
    chat_->clearChat();
    teardownHosts();
    model_->clear();
    setupHosts();
    refreshHeader();
}

void MainWindow::showError(const QString& msg) { statusBar()->showMessage(msg, 8000); }

qint64 MainWindow::lastNotified(const QString& host) const {
    return QSettings().value("notified/" + host, 0).toLongLong();
}

void MainWindow::setLastNotified(const QString& host, qint64 seq) { QSettings().setValue("notified/" + host, seq); }

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    if (obj == input_ && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && !(ke->modifiers() & Qt::ShiftModifier)) {
            sendInput();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::changeEvent(QEvent* e) {
    if (e->type() == QEvent::ActivationChange && isActiveWindow()) maybeMarkRead();
    QMainWindow::changeEvent(e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s;
    s.setValue("geometry", saveGeometry());
    if (auto* split = findChild<QSplitter*>()) s.setValue("splitter", split->saveState());
    if (tray_ && tray_->isVisible()) {
        hide();  // keep running in the tray so notifications still arrive
        e->ignore();
        return;
    }
    e->accept();
}
