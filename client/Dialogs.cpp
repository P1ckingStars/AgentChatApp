#include "Dialogs.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

QList<HostConfig> loadHosts() {
    QSettings s;
    QList<HostConfig> hosts;
    int n = s.beginReadArray("hosts");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        hosts.append({s.value("name").toString(), s.value("ssh").toString(), s.value("command").toString()});
    }
    s.endArray();
    if (hosts.isEmpty()) hosts.append({"local", "", ""});
    return hosts;
}

void saveHosts(const QList<HostConfig>& hosts) {
    QSettings s;
    s.beginWriteArray("hosts", hosts.size());
    for (int i = 0; i < hosts.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", hosts[i].name);
        s.setValue("ssh", hosts[i].ssh);
        s.setValue("command", hosts[i].command);
    }
    s.endArray();
}

QString defaultTerminal() {
    // How each terminal wants "run this command": kitty/foot take it directly.
    static const QList<QPair<QString, QString>> known = {
        {"kitty", "kitty"},           {"alacritty", "alacritty -e"}, {"foot", "foot"},
        {"wezterm", "wezterm start --"}, {"ghostty", "ghostty -e"},  {"konsole", "konsole -e"},
        {"gnome-terminal", "gnome-terminal --"}, {"xterm", "xterm -e"},
    };
    QString env = qEnvironmentVariable("TERMINAL");
    if (!env.isEmpty()) {
        QString base = QFileInfo(env).fileName();
        for (const auto& [name, cmd] : known)
            if (name == base) return cmd;
        return env + " -e";
    }
    for (const auto& [name, cmd] : known)
        if (!QStandardPaths::findExecutable(name).isEmpty()) return cmd;
    return "xterm -e";
}

QString loadTerminal() { return QSettings().value("terminal", defaultTerminal()).toString(); }

void saveTerminal(const QString& terminal) { QSettings().setValue("terminal", terminal); }

NewSessionDialog::NewSessionDialog(const QStringList& hosts, const QString& defaultHost, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("New Claude session");
    host_ = new QComboBox;
    host_->addItems(hosts);
    host_->setCurrentText(defaultHost);

    name_ = new QLineEdit;
    name_->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9_-]{1,64}"), name_));
    name_->setPlaceholderText("e.g. api-refactor (tmux session cc-<name>)");

    cwd_ = new QComboBox;
    cwd_->setEditable(true);
    cwd_->addItems(QSettings().value("cwdHistory").toStringList());
    if (cwd_->count() == 0) cwd_->addItem("~");
    cwd_->setMinimumWidth(380);

    args_ = new QLineEdit;
    args_->setPlaceholderText("optional claude flags, e.g. --model opus");

    // Suggest a name from the directory until the user types one.
    auto suggest = [this](const QString& dir) {
        if (name_->isModified()) return;
        QString base = QFileInfo(dir).fileName();
        base.replace(QRegularExpression("[^A-Za-z0-9_-]"), "-");
        name_->setText(base.left(64));
    };
    connect(cwd_, &QComboBox::currentTextChanged, this, suggest);
    suggest(cwd_->currentText());

    auto* form = new QFormLayout;
    form->addRow("Host", host_);
    form->addRow("Directory", cwd_);
    form->addRow("Name", name_);
    form->addRow("Claude args", args_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Start");
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

QString NewSessionDialog::host() const { return host_->currentText(); }
QString NewSessionDialog::name() const { return name_->text().trimmed(); }
QString NewSessionDialog::cwd() const { return cwd_->currentText().trimmed(); }
QStringList NewSessionDialog::args() const { return QProcess::splitCommand(args_->text()); }

void NewSessionDialog::accept() {
    if (name().isEmpty() || cwd().isEmpty()) {
        QMessageBox::warning(this, "New session", "Name and directory are required.");
        return;
    }
    QSettings s;
    QStringList history = s.value("cwdHistory").toStringList();
    history.removeAll(cwd());
    history.prepend(cwd());
    s.setValue("cwdHistory", history.mid(0, 15));
    QDialog::accept();
}

SettingsDialog::SettingsDialog(const QList<HostConfig>& hosts, const QString& terminal, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("ccbus settings");
    resize(720, 360);

    table_ = new QTableWidget(0, 3);
    table_->setHorizontalHeaderLabels({"Name", "SSH destination (empty = this machine)", "Command override"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->verticalHeader()->hide();
    for (const HostConfig& h : hosts) {
        int r = table_->rowCount();
        table_->insertRow(r);
        table_->setItem(r, 0, new QTableWidgetItem(h.name));
        table_->setItem(r, 1, new QTableWidgetItem(h.ssh));
        table_->setItem(r, 2, new QTableWidgetItem(h.command));
    }

    auto* add = new QPushButton("Add host");
    auto* remove = new QPushButton("Remove");
    connect(add, &QPushButton::clicked, this, [this] {
        int r = table_->rowCount();
        table_->insertRow(r);
        table_->setItem(r, 0, new QTableWidgetItem("server"));
        table_->setItem(r, 1, new QTableWidgetItem("user@server"));
        table_->setItem(r, 2, new QTableWidgetItem(""));
        table_->editItem(table_->item(r, 1));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (table_->currentRow() >= 0) table_->removeRow(table_->currentRow());
    });
    auto* rowButtons = new QHBoxLayout;
    rowButtons->addWidget(add);
    rowButtons->addWidget(remove);
    rowButtons->addStretch();

    auto* help = new QLabel(
        "Remote hosts run <code>ssh -T &lt;dest&gt; ~/.local/bin/ccbus serve --stdio</code> "
        "(key/agent auth required). Use the override if ccbus lives elsewhere.");
    help->setWordWrap(true);

    terminal_ = new QLineEdit(terminal);
    terminal_->setPlaceholderText(defaultTerminal());
    auto* form = new QFormLayout;
    form->addRow("Terminal for Attach", terminal_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("<b>Hosts</b>"));
    layout->addWidget(table_);
    layout->addLayout(rowButtons);
    layout->addWidget(help);
    layout->addSpacing(8);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

QList<HostConfig> SettingsDialog::hosts() const {
    QList<HostConfig> out;
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto text = [&](int c) { return table_->item(r, c) ? table_->item(r, c)->text().trimmed() : QString(); };
        if (!text(0).isEmpty()) out.append({text(0), text(1), text(2)});
    }
    return out;
}

QString SettingsDialog::terminal() const {
    return terminal_->text().trimmed().isEmpty() ? defaultTerminal() : terminal_->text().trimmed();
}

void SettingsDialog::accept() {
    QSet<QString> names;
    for (const HostConfig& h : hosts()) {
        if (names.contains(h.name)) {
            QMessageBox::warning(this, "Settings", "Host names must be unique: " + h.name);
            return;
        }
        names.insert(h.name);
    }
    if (names.isEmpty()) {
        QMessageBox::warning(this, "Settings", "Configure at least one host.");
        return;
    }
    QDialog::accept();
}
