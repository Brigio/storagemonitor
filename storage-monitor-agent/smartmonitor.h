// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SMARTMONITOR_H
#define SMARTMONITOR_H

//DIRETTIVI #INCLUDE INDISPENSABILI
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <QByteArray>
#include <QList>

//LE FORWARD DECLARATIONS (Nel Global Scope)
class QSqlDatabase;
class QTimer;
class SmartEventManager;

enum class SmartHealthState : int
{
    Unknown = 0,
    Ok      = 1,
    Warning = 2,
    Error   = 3
};

struct SmartDeviceState
{
    QString devicePath;
    QString deviceType;
    QString protocol;

    QString modelName;
    QString serialNumber;
    QString firmwareVersion;

    quint64 capacityBytes = 0;

    bool present = true;
    bool readOk = false;

    // -1 = sconosciuto, 0 = no/fail, 1 = sì/pass
    int smartSupported = -1;
    int smartEnabled = -1;
    int smartPassed = -1;

    int temperatureCelsius = -1;

    quint64 powerOnHours = 0;
    quint64 powerCycleCount = 0;

    // ATA/SATA: valori memorizzati se presenti.
    quint64 reallocatedSectors = 0;
    quint64 pendingSectors = 0;
    quint64 offlineUncorrectable = 0;
    quint64 crcErrors = 0;

    // NVMe
    int nvmeCriticalWarning = -1;
    int nvmeAvailableSpare = -1;
    int nvmeAvailableSpareThreshold = -1;
    int nvmePercentageUsed = -1;

    quint64 nvmeMediaErrors = 0;
    quint64 nvmeUnsafeShutdowns = 0;

    int smartctlExitStatus = 0;

    SmartHealthState health = SmartHealthState::Unknown;
    QString healthReason;
    QString lastError;
};

class SmartMonitor : public QObject
{
    Q_OBJECT

public:
    explicit SmartMonitor(const QSqlDatabase &database, QObject *parent = nullptr);

    ~SmartMonitor() override = default;

    bool isReady() const;
    bool isAvailable() const;

    void start(int intervalMs = 10 * 60 * 1000);
    void stop();

    bool acknowledgeEvent(qint64 eventId);
    bool resolveEvent(qint64 eventId);

    bool clearResolvedEvents();

public slots:
    void scanNow();

signals:
    void deviceUpdated(const QString &devicePath);
    void scanCompleted(int discoveredDevices, int readableDevices);

private:
    struct DeviceCandidate
    {
        QString path;
        QString type;
    };

    bool processSmartEvents(const SmartDeviceState &state);

    bool ensureSchema();

    bool discoverDevices(QList<DeviceCandidate> &devices, QString &errorMessage) const;

    bool readDevice(const DeviceCandidate &candidate, SmartDeviceState &state) const;

    bool parseSmartJson(const QByteArray &jsonData, SmartDeviceState &state, QString &errorMessage) const;

    void evaluateHealth(SmartDeviceState &state) const;

    bool markAllDevicesNotPresent();
    bool saveDeviceState(const SmartDeviceState &state);

private:

    SmartEventManager *m_eventManager = nullptr;
    QString m_connectionName;
    QString m_smartctlPath;
    QTimer *m_timer = nullptr;
    bool m_ready = false;
};

#endif // SMARTMONITOR_H
