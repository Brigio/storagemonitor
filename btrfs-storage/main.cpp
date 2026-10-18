// SPDX-License-Identifier: GPL-3.0-or-later

#include "mainwindow.h"
#include <QApplication>
#include <QGuiApplication> // Inclusione necessaria per impostare il desktop name su Wayland
#include <QMessageBox>
#include <unistd.h>
#include <QIcon>

int main(int argc, char *argv[])
{

    QApplication app(argc, argv);

    app.setApplicationName(QStringLiteral("Btrfs Storage"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));

    // Configura il nome dell'applicazione per i file desktop su Wayland
    QGuiApplication::setDesktopFileName("btrfs.storage");

    //Set application icon using the .qrc file prefix
    app.setWindowIcon(QIcon(":icons/app.svg"));

    MainWindow w;
    w.show();
    return app.exec();
}
