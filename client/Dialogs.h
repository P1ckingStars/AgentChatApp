#pragma once

#include <QDialog>

#include "HostConnection.h"

class QComboBox;
class QLineEdit;
class QTableWidget;

// Persistent settings (QSettings).
QList<HostConfig> loadHosts();
void saveHosts(const QList<HostConfig>& hosts);
QString loadTerminal();
void saveTerminal(const QString& terminal);
QString defaultTerminal();

class NewSessionDialog : public QDialog {
    Q_OBJECT
public:
    NewSessionDialog(const QStringList& hosts, const QString& defaultHost, QWidget* parent = nullptr);

    QString host() const;
    QString name() const;
    QString cwd() const;
    QStringList args() const;

    void accept() override;

private:
    QComboBox* host_;
    QLineEdit* name_;
    QComboBox* cwd_;
    QLineEdit* args_;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const QList<HostConfig>& hosts, const QString& terminal, QWidget* parent = nullptr);

    QList<HostConfig> hosts() const;
    QString terminal() const;

    void accept() override;

private:
    QTableWidget* table_;
    QLineEdit* terminal_;
};
