#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ccbus");
    QApplication::setOrganizationName("ccbus");
    QApplication::setDesktopFileName("ccbus");

    MainWindow w;
    w.show();
    return QApplication::exec();
}
