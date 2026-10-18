// SPDX-License-Identifier: GPL-3.0-or-later

#include "agentdbusclient.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QVariant>
#include <QDBusServiceWatcher>

namespace
{
constexpr auto kService = "org.storage_monitor.Agent";

constexpr auto kPath = "/org/storage_monitor/Agent";

constexpr auto kInterface = "org.storage_monitor.Agent";
}

AgentDbusClient::AgentDbusClient(QObject *parent) : QObject(parent)
{
    const bool connected = QDBusConnection::systemBus().connect(kService, kPath, kInterface, QStringLiteral("BalanceStateChanged"), this, SLOT(onBalanceStateChanged(QString,QString,QString))); //trasformare il nome della funzione in una stringa di testo normalizzata

    if (!connected)
    {
        qWarning() << "Cannot connect to" << "BalanceStateChanged D-Bus signal";
    }

    const bool removeProgressConnected = QDBusConnection::systemBus().connect(kService, kPath, kInterface, QStringLiteral("RemoveDeviceProgress"), this, SLOT(onRemoveDeviceProgress(QString,qulonglong,int,qulonglong)));

    if (!removeProgressConnected)
    {
        qWarning() << "Cannot connect to" << "RemoveDeviceProgress D-Bus signal";
    }

    const bool progressConnected = QDBusConnection::systemBus().connect(kService, kPath, kInterface, QStringLiteral("BalanceProgressChanged"), this, SLOT(onBalanceProgressChanged(QString,int,int)));

    if (!progressConnected)
    {
        qWarning() << "Cannot connect to" << "BalanceProgressChanged D-Bus signal";
    }

    const bool scrubStateConnected = QDBusConnection::systemBus().connect(kService, kPath, kInterface, QStringLiteral("ScrubStateChanged"), this, SLOT(onScrubStateChanged(QString,QString,QString)));

    // ============================================================
    // WATCH STORAGE MONITOR AGENT D-BUS SERVICE
    // ============================================================

    auto *serviceWatcher = new QDBusServiceWatcher(QString::fromLatin1(kService), QDBusConnection::systemBus(), QDBusServiceWatcher::WatchForRegistration, this);

    connect(serviceWatcher, &QDBusServiceWatcher::serviceRegistered, this, [this](const QString &serviceName)
        {
            if (serviceName != QString::fromLatin1(kService))
            {
                return;
            }

            qInfo().noquote() << "Storage Monitor Agent D-Bus service registered:" << serviceName;

            emit agentServiceRegistered();
        });

    if (!scrubStateConnected)
    {
        qWarning() << "Cannot connect to" << "ScrubStateChanged D-Bus signal";
    }

    const bool scrubProgressConnected = QDBusConnection::systemBus().connect(kService, kPath, kInterface, QStringLiteral("ScrubProgressChanged"), this, SLOT(onScrubProgressChanged(QString,int,qulonglong,qulonglong,qulonglong,int)));

    if (!scrubProgressConnected)
    {
        qWarning() << "Cannot connect to" << "ScrubProgressChanged D-Bus signal";
    }

    const bool replaceStateConnected = QDBusConnection::systemBus().connect(kService, kPath, kInterface, QStringLiteral("ReplaceStateChanged"), this, SLOT(onReplaceStateChanged(QString,QString,QString)));
    if (!replaceStateConnected)
    { qWarning() << "Cannot connect to" << "ReplaceStateChanged D-Bus signal"; }

    const bool replaceProgressConnected = QDBusConnection::systemBus().connect(kService, kPath, kInterface, QStringLiteral("ReplaceProgressChanged"), this, SLOT(onReplaceProgressChanged(QString,int)));
    if (!replaceProgressConnected)
    { qWarning() << "Cannot connect to" << "ReplaceProgressChanged D-Bus signal"; }

}

void AgentDbusClient::startBalance(const QString &mountPoint)
{
    if (mountPoint.trimmed().isEmpty())
    {
        emit errorOccurred(QStringLiteral("StartBalance"), mountPoint, QStringLiteral("Invalid mount point"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("StartBalance"));
    message.setArguments({ mountPoint.trimmed() });

    /*
     * Necessario affinché il daemon possa
     * chiedere a Polkit l'autenticazione GUI.
     */
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, mountPoint](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("StartBalance"), mountPoint, reply.error().message());
        }
        else
        {
            emit startBalanceFinished(mountPoint, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::startScrub(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty())
    {
        emit errorOccurred(QStringLiteral("StartScrub"), path, QStringLiteral("Invalid mount point"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("StartScrub"));
    message.setArguments({path});

    /*
     * Necessario affinché il daemon possa
     * chiedere a Polkit l'autenticazione.
     */
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("StartScrub"), path, reply.error().message());
        }
        else
        {
            emit startScrubFinished(path, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::cancelBalance(const QString &mountPoint)
{
    if (mountPoint.trimmed().isEmpty())
    {
        emit errorOccurred(QStringLiteral("CancelBalance"), mountPoint, QStringLiteral("Invalid mount point"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("CancelBalance"));
    message.setArguments({ mountPoint.trimmed() });

    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, mountPoint](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("CancelBalance"), mountPoint, reply.error().message());
        }
        else
        {
            emit cancelBalanceFinished(mountPoint, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::cancelScrub(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty())
    {
        emit errorOccurred(QStringLiteral("CancelScrub"), path, QStringLiteral("Invalid mount point"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("CancelScrub"));
    message.setArguments({path});
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("CancelScrub"), path, reply.error().message());
        }
        else
        {
            emit cancelScrubFinished(path, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::requestBalanceStatus(const QString &mountPoint)
{
    if (mountPoint.trimmed().isEmpty())
        return;

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("BalanceStatus"));
    message.setArguments({ mountPoint.trimmed() });

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 5000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, mountPoint](QDBusPendingCallWatcher *) {
        QDBusPendingReply<QString> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("BalanceStatus"), mountPoint, reply.error().message());
        }
        else
        {
            emit balanceStatusReceived(mountPoint, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::requestScrubStatus(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();
    if (path.isEmpty()) return;

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("ScrubStatus"));
    message.setArguments({path});

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 5000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<QString> reply = *watcher;
        if (reply.isError()) {
            emit errorOccurred(QStringLiteral("ScrubStatus"), path, reply.error().message());
        } else {
            emit scrubStatusReceived(path, reply.value());
        }
        watcher->deleteLater();
    });
}

void AgentDbusClient::addDevice(const QString &mountPoint, const QString &devicePath)
{
    const QString path = mountPoint.trimmed();
    const QString device = devicePath.trimmed();

    if (path.isEmpty() || device.isEmpty()) { emit errorOccurred(QStringLiteral("AddDevice"), path, QStringLiteral("Invalid mount point or device path")); return; }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("AddDevice"));
    message.setArguments({path, device});
    message.setInteractiveAuthorizationAllowed(true); /* Permette al daemon di chiedere a Polkit l'autenticazione GUI. */

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path, device](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) emit errorOccurred(QStringLiteral("AddDevice"), path, reply.error().message());
        else emit addDeviceFinished(path, device, reply.value());
        watcher->deleteLater();
    });
}

void AgentDbusClient::requestAutoScrubConfig(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty()) return;

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("AutoScrubConfig"));
    message.setArguments({ path });

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 5000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<QStringList> reply = *watcher;

        if (reply.isError()) {
            emit errorOccurred(QStringLiteral("AutoScrubConfig"), path, reply.error().message());
            watcher->deleteLater();
            return;
        }

        const QStringList values = reply.value();

        if (values.size() != 2) {
            emit errorOccurred(QStringLiteral("AutoScrubConfig"), path, QStringLiteral("Invalid automatic scrub configuration reply"));
            watcher->deleteLater();
            return;
        }

        const QString enabledValue = values.at(0).trimmed();

        if (enabledValue != QStringLiteral("0") && enabledValue != QStringLiteral("1")) {
            emit errorOccurred(QStringLiteral("AutoScrubConfig"), path, QStringLiteral("Invalid automatic scrub enabled value"));
            watcher->deleteLater();
            return;
        }

        bool intervalOk = false;
        const int intervalWeeks = values.at(1).toInt(&intervalOk);

        if (!intervalOk || intervalWeeks < 1 || intervalWeeks > 6) {
            emit errorOccurred(QStringLiteral("AutoScrubConfig"), path, QStringLiteral("Invalid automatic scrub interval"));
            watcher->deleteLater();
            return;
        }

        emit autoScrubConfigReceived(path, enabledValue == QStringLiteral("1"), intervalWeeks);
        watcher->deleteLater();
    });
}

void AgentDbusClient::requestAutoScrubSchedule(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();
    if (path.isEmpty()) return;

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("AutoScrubSchedule"));
    message.setArguments({ path });

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 5000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<QStringList> reply = *watcher;

        if (reply.isError()) return emit errorOccurred(QStringLiteral("AutoScrubSchedule"), path, reply.error().message()), watcher->deleteLater();

        const QStringList values = reply.value();
        if (values.size() != 2) return emit errorOccurred(QStringLiteral("AutoScrubSchedule"), path, QStringLiteral("Invalid automatic scrub schedule reply")), watcher->deleteLater();

        bool lastOk = false, nextOk = false;
        const qint64 lastSuccessfulEpoch = values.at(0).toLongLong(&lastOk);
        const qint64 nextDueEpoch = values.at(1).toLongLong(&nextOk);

        if (!lastOk || !nextOk || lastSuccessfulEpoch < 0 || nextDueEpoch < 0) {
            return emit errorOccurred(QStringLiteral("AutoScrubSchedule"), path, QStringLiteral("Invalid automatic scrub schedule values")), watcher->deleteLater();
        }

        emit autoScrubScheduleReceived(path, lastSuccessfulEpoch, nextDueEpoch);
        watcher->deleteLater();
    });
}

void AgentDbusClient::setAutoScrubConfig(const QString &mountPoint, bool enabled, int intervalWeeks)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty()) { emit errorOccurred(QStringLiteral("SetAutoScrubConfig"), path, QStringLiteral("Invalid mount point")); return; }

    if (intervalWeeks < 1 || intervalWeeks > 6) { emit errorOccurred(QStringLiteral("SetAutoScrubConfig"), path, QStringLiteral("Invalid automatic scrub interval")); return; }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("SetAutoScrubConfig"));
    message.setArguments({path, enabled, intervalWeeks});

    /* Il daemon protegge questa operazione attraverso Polkit. */
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) emit errorOccurred(QStringLiteral("SetAutoScrubConfig"), path, reply.error().message());
        else emit setAutoScrubConfigFinished(path, reply.value());
        watcher->deleteLater();
    });
}

void AgentDbusClient::removeDevice(const QString &mountPoint, qulonglong deviceId)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty() || deviceId == 0)
    {
        emit errorOccurred(QStringLiteral("RemoveDevice"), path, QStringLiteral("Invalid mount point or device ID"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("RemoveDevice"));
    message.setArguments({path, QVariant::fromValue(deviceId)});

    /* Permette al daemon di chiedere a Polkit l'autenticazione interattiva. */
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path, deviceId](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("RemoveDevice"), path, reply.error().message());
        }
        else
        {
            emit removeDeviceFinished(path, deviceId, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::startReplace(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath)
{
    const QString path = mountPoint.trimmed();
    const QString target = targetDevicePath.trimmed();

    if (path.isEmpty() || sourceDeviceId == 0 || target.isEmpty())
    {
        emit errorOccurred(QStringLiteral("StartReplace"), path, QStringLiteral("Invalid mount point, source device ID or target device"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("StartReplace"));
    message.setArguments({path, QVariant::fromValue(sourceDeviceId), target});

    /* Il daemon protegge Device Replace tramite Polkit. */
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path, sourceDeviceId, target](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("StartReplace"), path, reply.error().message());
        }
        else
        {
            emit startReplaceFinished(path, sourceDeviceId, target, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::cancelReplace(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty())
    {
        emit errorOccurred(QStringLiteral("CancelReplace"), path, QStringLiteral("Invalid mount point"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("CancelReplace"));
    message.setArguments({ path });
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit errorOccurred(QStringLiteral("CancelReplace"), path, reply.error().message());
        }
        else
        {
            emit cancelReplaceFinished(path, reply.value());
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::requestReplaceStatus(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty()) return;

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("ReplaceStatus"));
    message.setArguments({ path });

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 5000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path](QDBusPendingCallWatcher *) {
        QDBusPendingReply<QStringList> reply = *watcher;

        if (reply.isError()) {
            emit errorOccurred(QStringLiteral("ReplaceStatus"), path, reply.error().message());
            watcher->deleteLater();
            return;
        }

        const QStringList values = reply.value();

        if (values.size() != 3) {
            emit errorOccurred(QStringLiteral("ReplaceStatus"), path, QStringLiteral("Invalid device replace status reply"));
            watcher->deleteLater();
            return;
        }

        bool percentOk = false;
        const int completedPercent = values.at(1).toInt(&percentOk);

        if (!percentOk || completedPercent < -1 || completedPercent > 100) {
            emit errorOccurred(QStringLiteral("ReplaceStatus"), path, QStringLiteral("Invalid device replace progress value"));
            watcher->deleteLater();
            return;
        }

        emit replaceStatusReceived(path, values.at(0).trimmed(), completedPercent, values.at(2).trimmed());
        watcher->deleteLater();
    });
}

void AgentDbusClient::acknowledgeEvent(const QString &source, qulonglong eventId)
{
    const QString normalizedSource = source.trimmed().toLower();

    if ((normalizedSource != QStringLiteral("btrfs") && normalizedSource != QStringLiteral("smart")) || eventId == 0)
    {
        emit acknowledgeEventFinished(normalizedSource, eventId, false, QStringLiteral("Invalid event source or event ID"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("AcknowledgeEvent"));

    message.setArguments({
        normalizedSource, QVariant::fromValue(eventId)
    });

    /*
     * Necessario affinché Polkit possa mostrare
     * la richiesta di autenticazione interattiva.
     */
    message.setInteractiveAuthorizationAllowed(
        true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, normalizedSource, eventId](QDBusPendingCallWatcher *)
        {
            QDBusPendingReply<bool> reply = *watcher;

            if (reply.isError())
            {
                emit acknowledgeEventFinished(normalizedSource, eventId, false, reply.error().message());
            }
            else
            {
                const bool accepted = reply.value();

                emit acknowledgeEventFinished(normalizedSource, eventId, accepted, accepted ? QString() : QStringLiteral("The agent rejected the acknowledge request"));

            }

            watcher->deleteLater();
        });
}

void AgentDbusClient::resolveEvent(const QString &source, qulonglong eventId)
{
    const QString normalizedSource = source.trimmed().toLower();

    if ((normalizedSource != QStringLiteral("btrfs") && normalizedSource != QStringLiteral("smart")) || eventId == 0)
    {
        emit resolveEventFinished(normalizedSource, eventId, false, QStringLiteral("Invalid event source or event ID"));
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("ResolveEvent"));
    message.setArguments({ normalizedSource, QVariant::fromValue(eventId) });

    /*
     * Necessario affinché Polkit possa mostrare
     * la richiesta di autenticazione interattiva.
     */
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, normalizedSource, eventId](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError())
        {
            emit resolveEventFinished(normalizedSource, eventId, false, reply.error().message());
        }
        else
        {
            const bool accepted = reply.value();
            emit resolveEventFinished(normalizedSource, eventId, accepted, accepted ? QString() : QStringLiteral("The agent rejected the resolve request"));
        }

        watcher->deleteLater();
    });
}

void AgentDbusClient::clearEventHistory()
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("ClearEventHistory"));

    /*
     * Necessario affinché Polkit possa mostrare
     * la richiesta di autenticazione interattiva.
     */
    message.setInteractiveAuthorizationAllowed(true);

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message, 120000), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher](QDBusPendingCallWatcher *) {
        QDBusPendingReply<bool> reply = *watcher;

        if (reply.isError()) {
            emit clearEventHistoryFinished(false, reply.error().message()); }
        else {
            const bool accepted = reply.value();
            emit clearEventHistoryFinished(accepted, accepted ? QString() : QStringLiteral("The agent rejected the clear history request")); }

        watcher->deleteLater();
    });
}

void AgentDbusClient::onBalanceStateChanged(const QString &mountPoint, const QString &state, const QString &message)
{
    emit balanceStateChanged(mountPoint, state, message);
}

void AgentDbusClient::onBalanceProgressChanged(const QString &mountPoint, int completedPercent, int leftPercent)
{
    emit balanceProgressChanged(mountPoint, completedPercent, leftPercent);
}

void AgentDbusClient::onScrubStateChanged(const QString &mountPoint, const QString &state, const QString &message)
{
    emit scrubStateChanged(mountPoint, state, message);
}

void AgentDbusClient::onScrubProgressChanged(const QString &mountPoint, int completedPercent, qulonglong processedBytes, qulonglong totalBytes, qulonglong speedBytesPerSec, int errorCount)
{
    emit scrubProgressChanged(mountPoint, completedPercent, processedBytes, totalBytes, speedBytesPerSec, errorCount);
}

void AgentDbusClient::onRemoveDeviceProgress(const QString &mountPoint, qulonglong deviceId, int completedPercent, qulonglong remainingBytes)
{
    emit removeDeviceProgress(mountPoint, deviceId, completedPercent, remainingBytes);
}

void AgentDbusClient::onReplaceStateChanged(const QString &mountPoint, const QString &state, const QString &message)
{
    emit replaceStateChanged(mountPoint, state, message);
}

void AgentDbusClient::onReplaceProgressChanged(const QString &mountPoint, int completedPercent)
{
    emit replaceProgressChanged(mountPoint, completedPercent);
}