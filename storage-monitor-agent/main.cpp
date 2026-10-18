// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCoreApplication>
#include <QDebug>

#include <unistd.h>
#include <cstdlib>

#include "btrfsmonitor.h"

#include <QDBusConnection>
#include <QDBusError>

#include "agentdbusservice.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QCoreApplication::setApplicationName(QStringLiteral("storage-monitor-agent"));

    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    /*La funzione geteuid() (Get Effective User ID) restituisce
     * l'ID dell'utente che sta eseguendo il processo.
     * Nei sistemi Unix-like (come Linux o macOS), l'utente root ha sempre l'ID 0.
     * Se il risultato è diverso da 0, significa che l'utente non ha i permessi di superutente.
    */
    if (geteuid() != 0) {
        qCritical().noquote() << "storage-monitor-agent must be run as root";
        return EXIT_FAILURE;
    }

    qInfo().noquote() << "storage-monitor-agent started" << "PID:" << QCoreApplication::applicationPid() << "UID:" << geteuid();

    BtrfsMonitor monitor;

    AgentDbusService dbusService(&monitor);

    QDBusConnection systemBus = QDBusConnection::systemBus();

    if (!systemBus.isConnected())
    {
        qCritical().noquote() << "Cannot connect to system D-Bus:" << systemBus.lastError().message();

        return EXIT_FAILURE;
    }

    if (!systemBus.registerObject(
            QStringLiteral(
                "/org/storage_monitor/Agent"),
            &dbusService,
            QDBusConnection::ExportAllSlots |
                QDBusConnection::ExportAllSignals))
    {
        qCritical().noquote() << "Cannot register D-Bus object:" << systemBus.lastError().message();

        return EXIT_FAILURE;
    }

    if (!systemBus.registerService(
            QStringLiteral(
                "org.storage_monitor.Agent")))
    {
        qCritical().noquote() << "Cannot register D-Bus service:" << systemBus.lastError().message();

        return EXIT_FAILURE;
    }

    qInfo().noquote()
        << "D-Bus service registered:"
        << "org.storage_monitor.Agent"
        << "/org/storage_monitor/Agent";

    QObject::connect(&monitor, &BtrfsMonitor::healthChanged, [](const QString &fsUuid, BtrfsMonitor::HealthState state, const QString &reason)
        {
            switch (state)
            {
            case BtrfsMonitor::HealthState::Ok:
                qInfo().noquote()<< "[BTRFS OK]" << fsUuid << reason;
                break;

            case BtrfsMonitor::HealthState::Warning:
                qWarning().noquote() << "[BTRFS WARNING]" << fsUuid << reason;
                break;

            case BtrfsMonitor::HealthState::Error:
                qCritical().noquote() << "[BTRFS ERROR]" << fsUuid << reason;
                break;

            case BtrfsMonitor::HealthState::Unknown:
                break;
            }
        });

    return app.exec();
}