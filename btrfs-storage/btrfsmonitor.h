// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef BTRFSMONITOR_H
#define BTRFSMONITOR_H

#include <QObject>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "btrfseventmanager.h"

class QSqlQuery;
class QTimer;


/*
 * ============================================================
 * BtrfsMonitor
 * ============================================================
 *
 * ATTENZIONE:
 *
 * In btrfs-storage questa classe NON monitora più direttamente
 * Btrfs.
 *
 * Non legge:
 *
 *     /sys/fs/btrfs
 *
 * Non esegue:
 *
 *     btrfs
 *     findmnt
 *     lsblk
 *
 * Legge esclusivamente il database prodotto da:
 *
 *     storage-monitor-agent
 */

class BtrfsMonitor : public QObject
{
    Q_OBJECT

public:

    /*
     * Deve rimanere coerente con storage-monitor-agent:
     *
     * 0 = Unknown
     * 1 = Ok
     * 2 = Warning
     * 3 = Error
     */
    enum class HealthState
    {
        Unknown = 0,
        Ok      = 1,
        Warning = 2,
        Error   = 3
    };


    /*
     * ========================================================
     * FILESYSTEM STATE
     * ========================================================
     *
     * Rappresentazione GUI della tabella:
     *
     *     filesystems
     */

    struct FilesystemState
    {
        QString fsUuid;
        QString mountPoint;

        int deviceCount = 0;

        QString dataProfile;
        QString metadataProfile;

        quint64 totalBytes = 0;
        quint64 usedBytes = 0;
        quint64 unallocatedBytes = 0;

        QString scrubStatus;
        QString balanceStatus;

        HealthState health =
            HealthState::Unknown;

        QDateTime lastSeen;


        bool operator==(const FilesystemState &other) const
        {
            return
                fsUuid == other.fsUuid &&
                mountPoint == other.mountPoint &&
                deviceCount == other.deviceCount &&
                dataProfile == other.dataProfile &&
                metadataProfile == other.metadataProfile &&
                totalBytes == other.totalBytes &&
                usedBytes == other.usedBytes &&
                unallocatedBytes == other.unallocatedBytes &&
                scrubStatus == other.scrubStatus &&
                balanceStatus == other.balanceStatus &&
                health == other.health &&
                lastSeen == other.lastSeen;
        }


        bool operator!=(const FilesystemState &other) const
        {
            return !(*this == other);
        }
    };

    /*
    * ========================================================
    * BTRFS DEVICE STATE
    * ========================================================
    *
    * Rappresentazione GUI della tabella:
    *
    *     btrfs_devices
    */

    struct DeviceState
    {
        QString fsUuid;
        QString deviceId;
        QString devicePath;

        bool present = false;

        quint64 totalBytes = 0;
        quint64 allocatedBytes = 0;
        quint64 unallocatedBytes = 0;

        quint64 readErrors = 0;
        quint64 writeErrors = 0;
        quint64 flushErrors = 0;
        quint64 corruptionErrors = 0;
        quint64 generationErrors = 0;

        QDateTime lastSeen;

        bool operator==(const DeviceState &other) const
        {
            return
                fsUuid == other.fsUuid &&
                deviceId == other.deviceId &&
                devicePath == other.devicePath &&
                present == other.present &&
                totalBytes == other.totalBytes &&
                readErrors == other.readErrors &&
                writeErrors == other.writeErrors &&
                flushErrors == other.flushErrors &&
                corruptionErrors == other.corruptionErrors &&
                generationErrors == other.generationErrors;
        }

        bool operator!=(const DeviceState &other) const
        {
            return !(*this == other);
        }
    };

    /*
     * ========================================================
     * SMART DEVICE STATE
     * ========================================================
     *
     * Rappresentazione GUI della tabella:
     *
     *     smart_devices
     */

    struct SmartDeviceState
    {
        QString devicePath;
        QString deviceType;
        QString protocol;

        QString modelName;
        QString serialNumber;
        QString firmwareVersion;

        quint64 capacityBytes = 0;

        bool present = false;
        bool readOk = false;

        int smartSupported = -1;
        int smartEnabled = -1;
        int smartPassed = -1;

        int temperatureCelsius = -1;

        quint64 powerOnHours = 0;
        quint64 powerCycleCount = 0;

        quint64 reallocatedSectors = 0;
        quint64 pendingSectors = 0;
        quint64 offlineUncorrectable = 0;
        quint64 crcErrors = 0;

        int nvmeCriticalWarning = -1;
        int nvmeAvailableSpare = -1;
        int nvmeAvailableSpareThreshold = -1;
        int nvmePercentageUsed = -1;

        quint64 nvmeMediaErrors = 0;
        quint64 nvmeUnsafeShutdowns = 0;

        int smartctlExitStatus = 0;

        HealthState health =
            HealthState::Unknown;

        QString healthReason;
        QString lastError;

        QDateTime lastSeen;
        QDateTime lastSmartRead;


        bool operator==(const SmartDeviceState &other) const
        {
            return
                devicePath == other.devicePath &&
                present == other.present &&
                readOk == other.readOk &&
                smartSupported == other.smartSupported &&
                health == other.health;
        }

        bool operator!=(
            const SmartDeviceState &other) const
        {
            return !(*this == other);
        }
    };

    /*
     * ========================================================
     * COSTRUTTORE / DISTRUTTORE
     * ========================================================
     */

    explicit BtrfsMonitor(QObject *parent = nullptr);

    ~BtrfsMonitor() override;


    /*
     * ========================================================
     * DATABASE
     * ========================================================
     */

    bool openDatabase(const QString &databasePath = QStringLiteral("/var/lib/storage-monitor-agent/btrfs.db"));

    void closeDatabase();

    bool isOpen() const;

    QString databasePath() const;

    bool hasFilesystemsTable() const;


    /*
     * ========================================================
     * ACCESSO DATI
     * ========================================================
     */

    HealthState healthState(const QString &fsUuid) const;

    bool filesystemState(const QString &fsUuid, FilesystemState &state) const;

    QList<FilesystemState>
    filesystems() const;

    bool deviceState(const QString &fsUuid, const QString &deviceId, DeviceState &state) const;

    QList<DeviceState>
    devicesForFilesystem(const QString &fsUuid) const;

    bool smartDeviceState(const QString &devicePath, SmartDeviceState &state) const;

    QList<SmartDeviceState>
    smartDevices() const;

    /*
     * Conserviamo questa API per compatibilità con
     * la schermata eventi esistente.
     */

    BtrfsEventManager *eventManager() const;


    /*
     * ========================================================
     * UTILITY
     * ========================================================
     */

    static QString healthStateToString(HealthState state);


public slots:

    /*
     * Forza manualmente una rilettura dal DB.
     */
    void refresh();


signals:

    /*
     * Conserviamo il signal esistente.
     *
     * Ora viene emesso quando storage-monitor-agent modifica
     * health_state nel database.
     *
     * Al momento il DB non contiene health_reason, quindi
     * reason sarà vuoto.
     */
    void healthChanged(const QString &fsUuid, BtrfsMonitor::HealthState state, const QString &reason);


    /*
     * Nuovo signal utile alla GUI.
     *
     * Viene emesso se cambia una qualsiasi informazione
     * della tabella filesystems.
     */
    void filesystemsChanged();

    void devicesChanged();

    void smartDevicesChanged();

private slots:

    void checkForDatabaseChanges();


private:

    /*
     * ========================================================
     * DATABASE READER
     * ========================================================
     */

    bool readDataVersion(qint64 &version) const;

    void reloadFilesystems(bool emitSignals);

    void reloadDevices(bool emitSignals);

    DeviceState deviceFromQuery(const QSqlQuery &query) const;

    void reloadSmartDevices(bool emitSignals);

    SmartDeviceState smartDeviceFromQuery(const QSqlQuery &query) const;

    FilesystemState filesystemFromQuery(const QSqlQuery &query) const;


    /*
     * ========================================================
     * CONVERSIONI
     * ========================================================
     */

    static HealthState healthStateFromDatabase(const QVariant &value);

    static QString profileFromDatabase(const QVariant &value);


private:

    QSqlDatabase m_db;

    QString m_connectionName;
    QString m_databasePath;

    QTimer *m_refreshTimer = nullptr;

    qint64 m_lastDataVersion = -1;


    /*
     * Cache dei dati letti dal database.
     */
    QHash<QString, FilesystemState> m_filesystems;

    /*
    * fs_uuid -> (device_id -> DeviceState)
    */
    QHash<QString, QHash<QString, DeviceState>> m_devices;

    /*
     * device_path -> SmartDeviceState
    */
    QHash<QString, SmartDeviceState>
        m_smartDevices;

    /*
     * Reader dello storico eventi.
     */
    BtrfsEventManager *m_eventManager = nullptr;
};

#endif // BTRFSMONITOR_H