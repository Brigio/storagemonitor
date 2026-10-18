// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfsmonitor.h"

#include <algorithm>

#include <QDebug>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QVariant>


/*
 * ============================================================
 * COSTRUTTORE
 * ============================================================
 */

BtrfsMonitor::BtrfsMonitor(QObject *parent)  : QObject(parent)
{
    /*
     * Event reader.
     *
     * Non genera eventi.
     * Legge btrfs_events da SQLite.
     */
    m_eventManager = new BtrfsEventManager(this);


    /*
     * Questo timer NON interroga Btrfs.
     *
     * Controlla solamente se il database è stato modificato
     * da storage-monitor-agent.
     */
    m_refreshTimer =  new QTimer(this);

    m_refreshTimer->setInterval(2000);

    connect(m_refreshTimer, &QTimer::timeout, this, &BtrfsMonitor::checkForDatabaseChanges);

    /*
     * Apertura DB standard.
     */
    if (!openDatabase())
    {
        qWarning().noquote()
        << "BtrfsMonitor:"
        << "storage-monitor-agent database not available";
    }
}


/*
 * ============================================================
 * DISTRUTTORE
 * ============================================================
 */

BtrfsMonitor::~BtrfsMonitor()
{
    closeDatabase();
}


/*
 * ============================================================
 * OPEN DATABASE
 * ============================================================
 */

bool BtrfsMonitor::openDatabase(const QString &databasePath)
{
    closeDatabase();

    QFileInfo databaseInfo(databasePath);

    if (!databaseInfo.exists())
    {
        qWarning().noquote() << "Btrfs database does not exist:" << databasePath;
        return false;
    }


    if (!databaseInfo.isFile())
    {
        qWarning().noquote() << "Btrfs database path is not a file:" << databasePath;
        return false;
    }


    if (!databaseInfo.isReadable())
    {
        qWarning().noquote() << "Btrfs database is not readable:" << databasePath;
        return false;
    }

    /*
     * Connessione SQLite privata di BtrfsMonitor.
     */
    m_connectionName = QStringLiteral("btrfs-monitor-reader-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);

    m_db.setDatabaseName(databasePath);


    /*
     * btrfs-storage è reader:
     *
     * nessuna scrittura sul DB dell'agent.
     */
    m_db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;"
            "QSQLITE_BUSY_TIMEOUT=3000"));


    if (!m_db.open())
    {
        qWarning().noquote() << "Cannot open storage-monitor-agent database:" << m_db.lastError().text();

        const QString connectionName = m_connectionName;

        m_db = QSqlDatabase();


        QSqlDatabase::removeDatabase(
            connectionName);


        m_connectionName.clear();

        return false;
    }


    m_databasePath =
        databasePath;


    /*
     * Ulteriore protezione read-only.
     */
    {
        QSqlQuery query(m_db);

        if (!query.exec(
                QStringLiteral(
                    "PRAGMA query_only = ON")))
        {
            qWarning()
            << "Cannot enable SQLite query_only:"
            << query.lastError().text();
        }
    }


    /*
     * Baseline dello stato filesystem.
     *
     * Non emettiamo healthChanged() per tutti i filesystem
     * già presenti quando parte la GUI.
     */
    reloadFilesystems(false);
    reloadDevices(false);
    reloadSmartDevices(false);

    /*
     * Versione DB iniziale.
     */
    if (!readDataVersion(
            m_lastDataVersion))
    {
        m_lastDataVersion = -1;
    }


    /*
     * Assicura che anche BtrfsEventManager legga
     * esattamente lo stesso DB.
     */
    if (m_eventManager)
    {
        if (!m_eventManager->isOpen() ||
            m_eventManager->databasePath() != databasePath)
        {
            if (!m_eventManager->openDatabase(
                    databasePath))
            {
                qWarning().noquote()
                << "Cannot open event database reader";
            }
        }
    }


    m_refreshTimer->start();


    qInfo().noquote()
        << "BtrfsMonitor:"
        << "opened read-only database"
        << databasePath;


    return true;
}


/*
 * ============================================================
 * CLOSE DATABASE
 * ============================================================
 */

void BtrfsMonitor::closeDatabase()
{
    if (m_refreshTimer)
    {
        m_refreshTimer->stop();
    }


    m_filesystems.clear();
    m_devices.clear();
    m_smartDevices.clear();

    m_lastDataVersion = -1;


    /*
     * BtrfsEventManager possiede una propria connessione
     * SQLite read-only.
     */
    if (m_eventManager)
    {
        m_eventManager->closeDatabase();
    }


    if (!m_db.isValid())
    {
        m_databasePath.clear();
        return;
    }

    const QString connectionName = m_connectionName;

    if (m_db.isOpen())
    {
        m_db.close();
    }


    /*
     * Rilascia prima l'handle, poi removeDatabase().
     */
    m_db = QSqlDatabase();


    if (!connectionName.isEmpty())
    {
        QSqlDatabase::removeDatabase(
            connectionName);
    }


    m_connectionName.clear();
    m_databasePath.clear();
}


/*
 * ============================================================
 * IS OPEN
 * ============================================================
 */

bool BtrfsMonitor::isOpen() const
{
    return m_db.isValid() &&
           m_db.isOpen();
}


/*
 * ============================================================
 * DATABASE PATH
 * ============================================================
 */

QString BtrfsMonitor::databasePath() const
{
    return m_databasePath;
}


/*
 * ============================================================
 * HAS FILESYSTEMS TABLE
 * ============================================================
 */

bool BtrfsMonitor::hasFilesystemsTable() const
{
    if (!isOpen())
        return false;


    QSqlQuery query(m_db);


    query.prepare(
        QStringLiteral(
            "SELECT 1 "
            "FROM sqlite_master "
            "WHERE type = 'table' "
            "AND name = 'filesystems' "
            "LIMIT 1"));


    if (!query.exec())
    {
        qWarning()
        << "Cannot check filesystems table:"
        << query.lastError().text();

        return false;
    }


    return query.next();
}


/*
 * ============================================================
 * HEALTH STATE
 * ============================================================
 */

BtrfsMonitor::HealthState
BtrfsMonitor::healthState(const QString &fsUuid) const
{
    const auto it =
        m_filesystems.constFind(
            fsUuid);


    if (it ==
        m_filesystems.constEnd())
    {
        return HealthState::Unknown;
    }


    return it->health;
}


/*
 * ============================================================
 * FILESYSTEM STATE
 * ============================================================
 */

bool BtrfsMonitor::filesystemState(const QString &fsUuid, FilesystemState &state) const
{
    const auto it =
        m_filesystems.constFind(
            fsUuid);


    if (it ==
        m_filesystems.constEnd())
    {
        return false;
    }


    state =
        it.value();


    return true;
}


/*
 * ============================================================
 * FILESYSTEM LIST
 * ============================================================
 */

QList<BtrfsMonitor::FilesystemState>
BtrfsMonitor::filesystems() const
{
    QList<FilesystemState> result =
        m_filesystems.values();


    /*
     * Ordine stabile per la GUI.
     */
    std::sort(
        result.begin(),
        result.end(),
        [](const FilesystemState &a,
           const FilesystemState &b)
        {
            /*
             * Prima mount point, poi UUID.
             */
            if (a.mountPoint != b.mountPoint)
            {
                return a.mountPoint <
                       b.mountPoint;
            }

            return a.fsUuid <
                   b.fsUuid;
        });


    return result;
}

bool BtrfsMonitor::deviceState(const QString &fsUuid, const QString &deviceId, DeviceState &state) const
{
    const auto fsIt = m_devices.constFind(fsUuid);

    if (fsIt == m_devices.constEnd())
        return false;

    const auto deviceIt = fsIt->constFind(deviceId);

    if (deviceIt == fsIt->constEnd())
        return false;

    state = deviceIt.value();

    return true;
}

QList<BtrfsMonitor::DeviceState>
BtrfsMonitor::devicesForFilesystem(const QString &fsUuid) const
{
    const auto fsIt = m_devices.constFind(fsUuid);

    if (fsIt == m_devices.constEnd())
        return {};

    QList<DeviceState> result = fsIt->values();

    std::sort(
        result.begin(),
        result.end(),
        [](const DeviceState &a,
           const DeviceState &b)
        {
            bool aOk = false;
            bool bOk = false;

            const qulonglong aId = a.deviceId.toULongLong(&aOk);

            const qulonglong bId = b.deviceId.toULongLong(&bOk);

            if (aOk && bOk)
                return aId < bId;

            return a.deviceId < b.deviceId;
        });

    return result;
}

bool BtrfsMonitor::smartDeviceState(const QString &devicePath, SmartDeviceState &state) const
{
    const auto it = m_smartDevices.constFind(devicePath);

    if (it == m_smartDevices.constEnd()) {
        return false;
    }

    state = it.value();
    return true;
}

QList<BtrfsMonitor::SmartDeviceState> BtrfsMonitor::smartDevices() const
{
    QList<SmartDeviceState> result = m_smartDevices.values();

    /* Ordine stabile per la GUI. */
    std::sort(result.begin(), result.end(), [](const SmartDeviceState &a, const SmartDeviceState &b) {
        return a.devicePath < b.devicePath;
    });

    return result;
}

/*
 * ============================================================
 * EVENT MANAGER
 * ============================================================
 */

BtrfsEventManager *
BtrfsMonitor::eventManager() const
{
    return m_eventManager;
}

/*
 * ============================================================
 * HEALTH STATE TO STRING
 * ============================================================
 */

QString BtrfsMonitor::healthStateToString(HealthState state)
{
    switch (state)
    {
    case HealthState::Ok:
        return QStringLiteral("OK");

    case HealthState::Warning:
        return QStringLiteral("WARNING");

    case HealthState::Error:
        return QStringLiteral("ERROR");

    case HealthState::Unknown:
    default:
        return QStringLiteral("UNKNOWN");
    }
}

/*
 * ============================================================
 * HEALTH STATE FROM DATABASE
 * ============================================================
 */

BtrfsMonitor::HealthState
BtrfsMonitor::healthStateFromDatabase(const QVariant &value)
{
    /*
     * Attualmente storage-monitor-agent salva un INTEGER,
     * ma supportiamo anche eventuali versioni future che
     * usino testo.
     */

    bool numericOk = false;

    const int numeric =
        value.toString()
            .trimmed()
            .toInt(&numericOk);


    if (numericOk)
    {
        switch (numeric)
        {
        case 1:
            return HealthState::Ok;

        case 2:
            return HealthState::Warning;

        case 3:
            return HealthState::Error;

        case 0:
        default:
            return HealthState::Unknown;
        }
    }


    const QString text =
        value.toString()
            .trimmed()
            .toLower();


    if (text == QStringLiteral("ok") ||
        text == QStringLiteral("healthy"))
    {
        return HealthState::Ok;
    }


    if (text == QStringLiteral("warning"))
    {
        return HealthState::Warning;
    }


    if (text == QStringLiteral("error") ||
        text == QStringLiteral("critical"))
    {
        return HealthState::Error;
    }


    return HealthState::Unknown;
}


/*
 * ============================================================
 * PROFILE FROM DATABASE
 * ============================================================
 */

QString BtrfsMonitor::profileFromDatabase(const QVariant &value)
{
    QString profile =
        value.toString()
            .trimmed()
            .toLower();


    /*
     * storage-monitor-agent attualmente salva il valore
     * numerico dell'enum come stringa.
     *
     * Supportiamo anche la futura memorizzazione testuale.
     */

    bool ok = false;

    const int numeric =
        profile.toInt(&ok);


    if (ok)
    {
        switch (numeric)
        {
        case 1:
            return QStringLiteral("single");

        case 2:
            return QStringLiteral("raid0");

        case 3:
            return QStringLiteral("raid1");

        case 4:
            return QStringLiteral("raid1c3");

        case 5:
            return QStringLiteral("raid1c4");

        case 6:
            return QStringLiteral("raid10");

        case 7:
            return QStringLiteral("raid5");

        case 8:
            return QStringLiteral("raid6");

        case 0:
        default:
            return QStringLiteral("unknown");
        }
    }


    if (profile.isEmpty())
    {
        return QStringLiteral("unknown");
    }


    return profile;
}

/*
 * ============================================================
 * FILESYSTEM FROM QUERY
 * ============================================================
 */

BtrfsMonitor::FilesystemState
BtrfsMonitor::filesystemFromQuery(const QSqlQuery &query) const
{
    FilesystemState state;


    /*
     * SELECT order:
     *
     *  0 fs_uuid
     *  1 mount_point
     *  2 device_count
     *  3 data_profile
     *  4 metadata_profile
     *  5 total_bytes
     *  6 used_bytes
     *  7 unallocated_bytes
     *  8 scrub_status
     *  9 balance_status
     * 10 health_state
     * 11 last_seen
     */

    state.fsUuid =
        query.value(0).toString();

    state.mountPoint =
        query.value(1).toString();

    state.deviceCount =
        query.value(2).toInt();


    state.dataProfile =
        profileFromDatabase(
            query.value(3));

    state.metadataProfile =
        profileFromDatabase(
            query.value(4));


    state.totalBytes =
        query.value(5).toULongLong();

    state.usedBytes =
        query.value(6).toULongLong();

    state.unallocatedBytes =
        query.value(7).toULongLong();


    state.scrubStatus =
        query.value(8).toString();

    state.balanceStatus =
        query.value(9).toString();


    state.health =
        healthStateFromDatabase(
            query.value(10));


    const qint64 lastSeen =
        query.value(11).toLongLong();


    if (lastSeen > 0)
    {
        state.lastSeen =
            QDateTime::fromSecsSinceEpoch(
                lastSeen);
    }


    return state;
}

BtrfsMonitor::DeviceState
BtrfsMonitor::deviceFromQuery(const QSqlQuery &query) const
{
    DeviceState state;

    /*
     * SELECT order:
     *
     *  0 fs_uuid
     *  1 device_id
     *  2 device_path
     *  3 present
     *  4 total_bytes
     *  5 allocated_bytes
     *  6 unallocated_bytes
     *  7 read_errors
     *  8 write_errors
     *  9 flush_errors
     * 10 corruption_errors
     * 11 generation_errors
     * 12 last_seen
     */

    state.fsUuid = query.value(0).toString();
    state.deviceId = query.value(1).toString();
    state.devicePath = query.value(2).toString();
    state.present = query.value(3).toInt() != 0;
    state.totalBytes = query.value(4).toULongLong();
    state.allocatedBytes = query.value(5).toULongLong();
    state.unallocatedBytes = query.value(6).toULongLong();
    state.readErrors = query.value(7).toULongLong();
    state.writeErrors = query.value(8).toULongLong();
    state.flushErrors = query.value(9).toULongLong();
    state.corruptionErrors = query.value(10).toULongLong();
    state.generationErrors = query.value(11).toULongLong();

    const qint64 lastSeen =query.value(12).toLongLong();

    if (lastSeen > 0)
    {
        state.lastSeen = QDateTime::fromSecsSinceEpoch(lastSeen);
    }

    return state;
}

BtrfsMonitor::SmartDeviceState
BtrfsMonitor::smartDeviceFromQuery(const QSqlQuery &query) const
{
    SmartDeviceState state;

    /*
     * SELECT order:
     *
     *  0 device_path
     *  1 device_type
     *  2 protocol
     *  3 model_name
     *  4 serial_number
     *  5 firmware_version
     *  6 capacity_bytes
     *  7 present
     *  8 read_ok
     *  9 smart_supported
     * 10 smart_enabled
     * 11 smart_passed
     * 12 temperature_celsius
     * 13 power_on_hours
     * 14 power_cycle_count
     * 15 reallocated_sectors
     * 16 pending_sectors
     * 17 offline_uncorrectable
     * 18 crc_errors
     * 19 nvme_critical_warning
     * 20 nvme_available_spare
     * 21 nvme_available_spare_threshold
     * 22 nvme_percentage_used
     * 23 nvme_media_errors
     * 24 nvme_unsafe_shutdowns
     * 25 smartctl_exit_status
     * 26 health_state
     * 27 health_reason
     * 28 last_error
     * 29 last_seen
     * 30 last_smart_read
     */

    state.devicePath = query.value(0).toString();
    state.deviceType = query.value(1).toString();
    state.protocol = query.value(2).toString();
    state.modelName = query.value(3).toString();
    state.serialNumber = query.value(4).toString();
    state.firmwareVersion = query.value(5).toString();
    state.capacityBytes = query.value(6).toULongLong();
    state.present = query.value(7).toInt() != 0;
    state.readOk = query.value(8).toInt() != 0;
    state.smartSupported = query.value(9).toInt();
    state.smartEnabled = query.value(10).toInt();
    state.smartPassed = query.value(11).toInt();
    state.temperatureCelsius = query.value(12).toInt();
    state.powerOnHours = query.value(13).toULongLong();
    state.powerCycleCount = query.value(14).toULongLong();
    state.reallocatedSectors = query.value(15).toULongLong();
    state.pendingSectors = query.value(16).toULongLong();
    state.offlineUncorrectable = query.value(17).toULongLong();
    state.crcErrors = query.value(18).toULongLong();
    state.nvmeCriticalWarning = query.value(19).toInt();
    state.nvmeAvailableSpare = query.value(20).toInt();
    state.nvmeAvailableSpareThreshold = query.value(21).toInt();
    state.nvmePercentageUsed = query.value(22).toInt();
    state.nvmeMediaErrors = query.value(23).toULongLong();
    state.nvmeUnsafeShutdowns = query.value(24).toULongLong();
    state.smartctlExitStatus = query.value(25).toInt();
    state.health = healthStateFromDatabase(query.value(26));
    state.healthReason = query.value(27).toString();
    state.lastError = query.value(28).toString();

    const qint64 lastSeen = query.value(29).toLongLong();

    if (lastSeen > 0)
    {
        state.lastSeen = QDateTime::fromSecsSinceEpoch(lastSeen);
    }


    const qint64 lastSmartRead = query.value(30).toLongLong();

    if (lastSmartRead > 0)
    {
        state.lastSmartRead = QDateTime::fromSecsSinceEpoch(lastSmartRead);
    }


    return state;
}

/*
 * ============================================================
 * RELOAD FILESYSTEMS
 * ============================================================
 */

void BtrfsMonitor::reloadFilesystems(bool emitSignals)
{
    if (!isOpen() ||
        !hasFilesystemsTable())
    {
        return;
    }


    QHash<QString, FilesystemState>
        newFilesystems;


    QSqlQuery query(m_db);


    if (!query.exec(
            QStringLiteral(
                "SELECT "
                "fs_uuid, "
                "mount_point, "
                "device_count, "
                "data_profile, "
                "metadata_profile, "
                "total_bytes, "
                "used_bytes, "
                "unallocated_bytes, "
                "scrub_status, "
                "balance_status, "
                "health_state, "
                "last_seen "
                "FROM filesystems "
                "ORDER BY fs_uuid")))
    {
        qWarning()
        << "Cannot read Btrfs filesystems:"
        << query.lastError().text();

        return;
    }


    while (query.next())
    {
        FilesystemState state =
            filesystemFromQuery(query);


        if (state.fsUuid.isEmpty())
            continue;


        newFilesystems.insert(
            state.fsUuid,
            state);
    }


    bool changed =
        newFilesystems.size() !=
        m_filesystems.size();


    /*
     * Confronto filesystem già conosciuti.
     */
    for (auto it =
         newFilesystems.constBegin();
         it != newFilesystems.constEnd();
         ++it)
    {
        const QString &fsUuid =
            it.key();

        const FilesystemState &newState =
            it.value();


        const auto oldIt =
            m_filesystems.constFind(
                fsUuid);


        /*
         * Nuovo filesystem.
         */
        if (oldIt ==
            m_filesystems.constEnd())
        {
            changed = true;

            /*
             * Come nel vecchio comportamento:
             * la prima rilevazione diventa baseline.
             *
             * Non emettiamo healthChanged().
             */

            continue;
        }


        const FilesystemState &oldState =
            oldIt.value();


        if (oldState != newState)
        {
            changed = true;
        }


        /*
         * healthChanged rimane compatibile.
         *
         * NOTA:
         * health_reason non è attualmente memorizzato
         * nella tabella filesystems.
         */

        if (emitSignals &&
            oldState.health !=
                newState.health)
        {
            emit healthChanged(
                fsUuid,
                newState.health,
                QString());
        }
    }


    /*
     * Controlla anche filesystem rimossi.
     */
    if (!changed)
    {
        for (auto it =
             m_filesystems.constBegin();
             it != m_filesystems.constEnd();
             ++it)
        {
            if (!newFilesystems.contains(
                    it.key()))
            {
                changed = true;
                break;
            }
        }
    }


    m_filesystems =
        newFilesystems;


    if (emitSignals &&
        changed)
    {
        emit filesystemsChanged();
    }
}

void BtrfsMonitor::reloadDevices(bool emitSignals)
{
    if (!isOpen())
        return;

    QSqlQuery query(m_db);

    if (!query.exec(
            QStringLiteral(
                "SELECT "
                "fs_uuid, "
                "device_id, "
                "device_path, "
                "present, "
                "total_bytes, "
                "allocated_bytes, "
                "unallocated_bytes, "
                "read_errors, "
                "write_errors, "
                "flush_errors, "
                "corruption_errors, "
                "generation_errors, "
                "last_seen "
                "FROM btrfs_devices "
                "ORDER BY fs_uuid, device_id")))
    {
        qWarning() << "Cannot read Btrfs devices:" << query.lastError().text();
        return;
    }

    QHash<QString, QHash<QString, DeviceState>>
        newDevices;

    while (query.next())
    {
        DeviceState state = deviceFromQuery(query);

        if (state.fsUuid.isEmpty() || state.deviceId.isEmpty())
        {
            continue;
        }

        newDevices[state.fsUuid].insert(state.deviceId, state);
    }

    const bool changed = newDevices != m_devices;

    m_devices = newDevices;

    if (emitSignals && changed)
    {
        emit devicesChanged();
    }
}

void BtrfsMonitor::reloadSmartDevices(bool emitSignals)
{
    if (!isOpen())
        return;

    QSqlQuery query(m_db);

    if (!query.exec(
            QStringLiteral(
                "SELECT "
                "device_path, "
                "device_type, "
                "protocol, "
                "model_name, "
                "serial_number, "
                "firmware_version, "
                "capacity_bytes, "
                "present, "
                "read_ok, "
                "smart_supported, "
                "smart_enabled, "
                "smart_passed, "
                "temperature_celsius, "
                "power_on_hours, "
                "power_cycle_count, "
                "reallocated_sectors, "
                "pending_sectors, "
                "offline_uncorrectable, "
                "crc_errors, "
                "nvme_critical_warning, "
                "nvme_available_spare, "
                "nvme_available_spare_threshold, "
                "nvme_percentage_used, "
                "nvme_media_errors, "
                "nvme_unsafe_shutdowns, "
                "smartctl_exit_status, "
                "health_state, "
                "health_reason, "
                "last_error, "
                "last_seen, "
                "last_smart_read "
                "FROM smart_devices "
                "ORDER BY device_path")))
    {
        qWarning() << "Cannot read SMART devices:" << query.lastError().text();
        return;
    }

    QHash<QString, SmartDeviceState>
        newSmartDevices;

    while (query.next())
    {
        SmartDeviceState state = smartDeviceFromQuery(query);

        if (state.devicePath.isEmpty())
            continue;

        newSmartDevices.insert(state.devicePath, state);
    }

    const bool changed = newSmartDevices != m_smartDevices;

    m_smartDevices = newSmartDevices;

    if (emitSignals && changed)
    {
        emit smartDevicesChanged();
    }
}

/*
 * ============================================================
 * SQLITE DATA VERSION
 * ============================================================
 */

bool BtrfsMonitor::readDataVersion(qint64 &version) const
{
    version = -1;


    if (!isOpen())
        return false;


    QSqlQuery query(m_db);


    if (!query.exec(
            QStringLiteral(
                "PRAGMA data_version")))
    {
        qWarning()
        << "Cannot read SQLite data_version:"
        << query.lastError().text();

        return false;
    }


    if (!query.next())
        return false;


    version =
        query.value(0).toLongLong();


    return true;
}

/*
 * ============================================================
 * CHECK DATABASE CHANGES
 * ============================================================
 */

void BtrfsMonitor::checkForDatabaseChanges()
{
    if (!isOpen())
        return;


    qint64 currentVersion = -1;


    if (!readDataVersion(currentVersion))
    {
        return;
    }


    if (m_lastDataVersion < 0)
    {
        m_lastDataVersion = currentVersion;

        reloadFilesystems(false);
        reloadDevices(false);
        reloadSmartDevices(false);

        return;
    }


    /*
     * Nessun commit proveniente da altre connessioni.
     */
    if (currentVersion == m_lastDataVersion)
    {
        return;
    }


    m_lastDataVersion = currentVersion;


    /*
     * storage-monitor-agent ha modificato SQLite.
     */
    reloadFilesystems(true);
    reloadDevices(true);
    reloadSmartDevices(true);
}

/*
 * ============================================================
 * MANUAL REFRESH
 * ============================================================
 */

void BtrfsMonitor::refresh()
{
    if (!isOpen())
        return;


    qint64 currentVersion = -1;

   if (readDataVersion(currentVersion))
    {
        m_lastDataVersion = currentVersion;
    }

    reloadFilesystems(true);
    reloadDevices(true);
    reloadSmartDevices(true);

    /*
     * Il BtrfsEventManager ha una propria connessione
     * e un proprio monitoraggio data_version, ma un refresh
     * manuale può essere propagato.
     */
    if (m_eventManager)
    {
        m_eventManager->refresh();
    }
}