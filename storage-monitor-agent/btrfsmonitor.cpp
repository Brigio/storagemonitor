// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfsmonitor.h"
#include "systemnotifier.h"
#include "btrfseventmanager.h"
#include "smartmonitor.h"
#include "emailnotifier.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QDateTime>
#include <algorithm>
#include <QProcess>
#include <utility>
#include <QSqlQuery>
#include <QSqlError>
#include <QProcessEnvironment>
#include <QStandardPaths>

// ============================================================
// COSTRUTTORE / DISTRUTTORE
// ============================================================

BtrfsMonitor::BtrfsMonitor(QObject *parent) : QObject(parent)
{
    // ============================================================
    // EMAIL
    // ============================================================

    m_emailNotifier = new EmailNotifier(this);

    // ============================================================
    // NOTIFIER
    // ============================================================

    m_notifier = new SystemNotifier(this);

    // ============================================================
    // DATABASE PATH
    // ============================================================

    const QString dbDir = QStringLiteral("/var/lib/storage-monitor-agent");

    QDir dbDirPath(dbDir);

    if (!dbDirPath.exists())
    {   //Se non esiste creare l'intero percorso memorizzato in dbDirPath
        if (!dbDirPath.mkpath(QStringLiteral(".")))
        {
            qWarning() << "⚠️ Cannot create database directory:" << dbDir;
        }
    }

    m_dbPath = dbDir + QStringLiteral("/btrfs.db");

    // ============================================================
    // INITIALIZE DATABASE
    // ============================================================

    if (!initializeDatabase())
    {
        qWarning() << "⚠️ Database initialization failed," << "scrub/balance/events history will NOT be saved";
        m_eventManager = nullptr;
    }
    else
    {
        // ========================================================
        // BTRFS EVENT MANAGER
        // ========================================================

        m_eventManager = new BtrfsEventManager(m_db, this);

        if (!m_eventManager->isReady())
        {
            qCritical()  << "❌ BtrfsEventManager initialization failed";

            delete m_eventManager;
            m_eventManager = nullptr;
        }
        else
        {
            qDebug() << "✅ BtrfsEventManager initialized";
        }

        // ========================================================
        // SMART MONITOR
        // ========================================================

        //chiama immediatamente il costruttore SmartMonitor::SmartMonitor all'interno di smartmonitor.cpp.
        m_smartMonitor = new SmartMonitor(m_db, this);

        connect(m_smartMonitor, &SmartMonitor::deviceUpdated, this, &BtrfsMonitor::onSmartDeviceUpdated);

        if (m_smartMonitor->isReady() && m_smartMonitor->isAvailable())
        {
            /*
             * SMART ogni 10 minuti. 10 (minuti) × 60 (secondi) × 1000 (millisecondi) = 600.000 millisecondi (ovvero 10 minuti).
             */
            m_smartMonitor->start(10 * 60 * 1000);
        }
        else
        {
            qWarning() << "⚠️ SMART monitoring unavailable";
        }
    }

    // ============================================================
    // BTRFS MONITOR TIMER
    // ============================================================

    auto *timer = new QTimer(this);

    connect(timer, &QTimer::timeout, this, &BtrfsMonitor::checkBtrfsSysfs);

    timer->start(30000);
    //Prima scansione Btrfs immediata.
    QTimer::singleShot(0, this, &BtrfsMonitor::checkBtrfsSysfs);
}

BtrfsMonitor::~BtrfsMonitor()
{
    /*
     * Distruggiamo esplicitamente il manager prima di rimuovere
     * la connessione SQLite del monitor.
     */
    if (m_eventManager)
    {
        delete m_eventManager;
        m_eventManager = nullptr;
    }

    const QString connectionName = m_db.connectionName();


    if (m_db.isOpen())
    {
        m_db.close();
    }


    /*
     * Elimina l'handle locale prima di removeDatabase().
     */
    m_db = QSqlDatabase();

    if (!connectionName.isEmpty())
    {
        QSqlDatabase::removeDatabase(connectionName);
    }
}

// ============================================================
// METODI PUBBLICI
// ============================================================

BtrfsMonitor::HealthState
BtrfsMonitor::healthState(const QString &fsUuid) const
{
    const auto it = m_filesystems.constFind(fsUuid);

    if (it == m_filesystems.constEnd())
        return HealthState::Unknown;

    return it->health;
}

bool BtrfsMonitor::btrfsDevicePresence(const QString &mountPoint, qulonglong deviceId, bool &present) const
{
    present = false;
    const QString path = mountPoint.trimmed();

    if (path.isEmpty() || deviceId == 0) return false;

    const QString id = QString::number(deviceId);

    for (auto fsIt = m_filesystems.constBegin(); fsIt != m_filesystems.constEnd(); ++fsIt) {
        const FilesystemState &filesystem = fsIt.value();
        if (filesystem.mountPoint != path) continue;

        const auto deviceIt = filesystem.devices.constFind(id);
        if (deviceIt == filesystem.devices.constEnd()) return false;

        present = deviceIt->present;
        return true;
    }

    return false;
}

bool BtrfsMonitor::btrfsDeviceAllocatedBytes(const QString &mountPoint, qulonglong deviceId, quint64 &allocatedBytes) const
{
    allocatedBytes = 0;
    const QString path = mountPoint.trimmed();

    if (path.isEmpty() || deviceId == 0) return false;

    const QString id = QString::number(deviceId);

    for (auto fsIt = m_filesystems.constBegin(); fsIt != m_filesystems.constEnd(); ++fsIt) {
        const FilesystemState &filesystem = fsIt.value();
        if (filesystem.mountPoint != path) continue;

        const auto deviceIt = filesystem.devices.constFind(id);
        if (deviceIt == filesystem.devices.constEnd()) return false;

        allocatedBytes = deviceIt->stats.deviceAllocated;
        return true;
    }

    return false;
}

bool BtrfsMonitor::btrfsDeviceTotalBytes(const QString &mountPoint, qulonglong deviceId, quint64 &totalBytes) const
{
    totalBytes = 0;
    const QString path = mountPoint.trimmed();

    if (path.isEmpty() || deviceId == 0) return false;

    const QString id = QString::number(deviceId);

    for (auto fsIt = m_filesystems.constBegin(); fsIt != m_filesystems.constEnd(); ++fsIt)
    {
        const FilesystemState &filesystem = fsIt.value();
        if (filesystem.mountPoint != path) continue;

        const auto deviceIt = filesystem.devices.constFind(id);
        if (deviceIt == filesystem.devices.constEnd()) return false;

        totalBytes = deviceIt->stats.deviceTotal;
        return true;
    }

    return false;
}

bool BtrfsMonitor::filesystemUuidForMountPoint(const QString &mountPoint, QString &fsUuid) const
{
    fsUuid.clear();

    const QString path = mountPoint.trimmed();

    if (path.isEmpty())
        return false;

    for (auto it = m_filesystems.constBegin();
         it != m_filesystems.constEnd();
         ++it)
    {
        if (it.value().mountPoint.trimmed() != path)
            continue;

        fsUuid = it.key();
        return !fsUuid.isEmpty();
    }

    return false;
}

bool BtrfsMonitor::beginDeviceReplacement(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath, const QString &details)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;
    if (sourceDeviceId == 0) return false;

    const QString target = targetDevicePath.trimmed();
    if (target.isEmpty()) return false;

    QString fsUuid;
    if (!filesystemUuidForMountPoint(mountPoint, fsUuid)) {
        qWarning().noquote() << "Cannot begin device replacement: filesystem UUID not found for" << mountPoint;
        return false;
    }

    /* Non devono esistere due replacement aperti contemporaneamente sullo stesso filesystem. */
    QSqlQuery openQuery(m_db);
    openQuery.prepare(QStringLiteral("SELECT id FROM btrfs_device_replacements WHERE fs_uuid = :fs_uuid AND end_time IS NULL ORDER BY id DESC LIMIT 1"));
    openQuery.bindValue(":fs_uuid", fsUuid);

    if (!openQuery.exec()) {
        qWarning().noquote() << "Cannot inspect open device replacement:" << openQuery.lastError().text();
        return false;
    }

    if (openQuery.next()) {
        qWarning().noquote() << "Cannot begin device replacement: an open replacement already exists for" << fsUuid.left(8);
        return false;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("INSERT INTO btrfs_device_replacements (fs_uuid, source_device_id, target_device_path, status, progress_percent, details, start_time, end_time, duration_seconds, created_at, updated_at) VALUES (:fs_uuid, :source_device_id, :target_device_path, 'starting', -1, :details, :start_time, NULL, 0, :created_at, :updated_at)"));

    query.bindValue(":fs_uuid", fsUuid);
    query.bindValue(":source_device_id", QString::number(sourceDeviceId));
    query.bindValue(":target_device_path", target);
    query.bindValue(":details", details);
    query.bindValue(":start_time", now);
    query.bindValue(":created_at", now);
    query.bindValue(":updated_at", now);

    if (!query.exec()) {
        qWarning().noquote() << "Cannot save device replacement:" << query.lastError().text();
        return false;
    }

    return true;
}

bool BtrfsMonitor::updateDeviceReplacement(const QString &mountPoint, const QString &status, int progressPercent,  const QString &details)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;
    if (progressPercent < -1 || progressPercent > 100) return false;

    const QString normalized = status.trimmed().toLower();
    if (normalized != QStringLiteral("starting") && normalized != QStringLiteral("running") && normalized != QStringLiteral("stopping")) {
        qWarning() << "Invalid active replacement status:" << status;
        return false;
    }

    QString fsUuid;
    if (!filesystemUuidForMountPoint(mountPoint, fsUuid)) return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE btrfs_device_replacements SET status = :status, progress_percent = :progress_percent, details = :details, updated_at = strftime('%s', 'now') WHERE id = (SELECT id FROM btrfs_device_replacements WHERE fs_uuid = :fs_uuid AND end_time IS NULL ORDER BY id DESC LIMIT 1)"));

    query.bindValue(":status", normalized);
    query.bindValue(":progress_percent", progressPercent);
    query.bindValue(":details", details);
    query.bindValue(":fs_uuid", fsUuid);

    if (!query.exec()) {
        qWarning().noquote() << "Cannot update device replacement:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

bool BtrfsMonitor::finishDeviceReplacement(const QString &mountPoint, const QString &status, int progressPercent, const QString &details)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;
    if (progressPercent < -1 || progressPercent > 100) return false;

    const QString normalized = status.trimmed().toLower();
    const bool terminal = normalized == QStringLiteral("finished") || normalized == QStringLiteral("cancelled") || normalized == QStringLiteral("canceled") || normalized == QStringLiteral("error") || normalized == QStringLiteral("failed") || normalized == QStringLiteral("interrupted");

    if (!terminal) {
        qWarning() << "Invalid terminal replacement status:" << status;
        return false;
    }

    QString fsUuid;
    if (!filesystemUuidForMountPoint(mountPoint, fsUuid)) return false;

    QSqlQuery findQuery(m_db);
    findQuery.prepare(QStringLiteral("SELECT id, start_time FROM btrfs_device_replacements WHERE fs_uuid = :fs_uuid AND end_time IS NULL ORDER BY id DESC LIMIT 1"));
    findQuery.bindValue(":fs_uuid", fsUuid);

    if (!findQuery.exec() || !findQuery.next()) return false;

    const qint64 id = findQuery.value(0).toLongLong();
    const qint64 startTime = findQuery.value(1).toLongLong();
    const qint64 endTime = QDateTime::currentSecsSinceEpoch();
    const qint64 duration = startTime > 0 && endTime >= startTime ? endTime - startTime : 0;

    QSqlQuery updateQuery(m_db);
    updateQuery.prepare(QStringLiteral("UPDATE btrfs_device_replacements SET status = :status, progress_percent = :progress_percent, details = :details, end_time = :end_time, duration_seconds = :duration_seconds, updated_at = :updated_at WHERE id = :id"));

    updateQuery.bindValue(":status", normalized);
    updateQuery.bindValue(":progress_percent", progressPercent);
    updateQuery.bindValue(":details", details);
    updateQuery.bindValue(":end_time", endTime);
    updateQuery.bindValue(":duration_seconds", duration);
    updateQuery.bindValue(":updated_at", endTime);
    updateQuery.bindValue(":id", id);

    if (!updateQuery.exec()) {
        qWarning().noquote() << "Cannot finish device replacement:" << updateQuery.lastError().text();
        return false;
    }

    return true;
}

bool BtrfsMonitor::acknowledgeEvent(const QString &source, qint64 eventId)
{
    const QString normalizedSource = source.trimmed().toLower();

    if (eventId <= 0)
    {
        qWarning() << "Cannot acknowledge storage event:" << "invalid id" << eventId;
        return false;
    }

    if (normalizedSource == QStringLiteral("btrfs"))
    {
        if (!m_eventManager || !m_eventManager->isReady())
        {
            qWarning() << "Cannot acknowledge Btrfs event:" << "event manager unavailable";
            return false;
        }

        return m_eventManager->acknowledgeEvent(eventId);
    }

    if (normalizedSource == QStringLiteral("smart"))
    {
        if (!m_smartMonitor)
        {
            qWarning() << "Cannot acknowledge SMART event:" << "SMART monitor unavailable";

            return false;
        }

        return m_smartMonitor->acknowledgeEvent(eventId);
    }

    qWarning() << "Cannot acknowledge storage event:" << "unknown source""" << source;

    return false;
}

bool BtrfsMonitor::resolveEvent(const QString &source, qint64 eventId)
{
    const QString normalizedSource = source.trimmed().toLower();

    if (eventId <= 0)
    {
        qWarning() << "Cannot resolve storage event:" << "invalid id" << eventId;
       return false;
    }

    if (normalizedSource == QStringLiteral("btrfs"))
    {
        if (!m_eventManager || !m_eventManager->isReady())
        {
            qWarning() << "Cannot resolve Btrfs event:" << "event manager unavailable";
            return false;
        }

        return m_eventManager->resolveEvent(eventId);
    }

    if (normalizedSource == QStringLiteral("smart"))
    {
        if (!m_smartMonitor)
        {
            qWarning() << "Cannot resolve SMART event:" << "SMART monitor unavailable";
            return false;
        }

        return m_smartMonitor->resolveEvent(eventId);
    }

    qWarning() << "Cannot resolve storage event:" << "unknown source" << source;
    return false;
}

bool BtrfsMonitor::clearEventHistory()
{
    if (!m_db.isValid() || !m_db.isOpen()) { qWarning() << "Cannot clear event history:" << "database unavailable";
        return false; }

    if (!m_eventManager || !m_eventManager->isReady()) { qWarning() << "Cannot clear event history:" << "Btrfs event manager unavailable";
        return false; }

    if (!m_smartMonitor) { qWarning() << "Cannot clear event history:" << "SMART monitor unavailable";
        return false; }

    if (!m_db.transaction()) { qWarning().noquote() << "Cannot start event history transaction:" << m_db.lastError().text();
        return false; }

    if (!m_eventManager->clearResolvedEvents()) { qWarning() << "Cannot clear Btrfs event history"; m_db.rollback();
        return false; }

    if (!m_smartMonitor->clearResolvedEvents()) { qWarning() << "Cannot clear SMART event history"; m_db.rollback();
        return false; }

    if (!m_db.commit()) { qWarning().noquote() << "Cannot commit event history transaction:" << m_db.lastError().text(); m_db.rollback();
        return false; }

    qInfo() << "Resolved storage event history cleared";
    return true;
}

// ============================================================
// METODI STATICI
// ============================================================

QString BtrfsMonitor::healthStateToString(HealthState state)
{
    switch (state)
    {
    case HealthState::Unknown:
        return QStringLiteral("UNKNOWN");
    case HealthState::Ok:
        return QStringLiteral("OK");
    case HealthState::Warning:
        return QStringLiteral("WARNING");
    case HealthState::Error:
        return QStringLiteral("ERROR");
    }

    return QStringLiteral("UNKNOWN");
}

// ============================================================
// DATABASE METHODS
// ============================================================

bool BtrfsMonitor::initializeDatabase()
{
    m_db = QSqlDatabase::addDatabase("QSQLITE", "btrfs_monitor");
    m_db.setDatabaseName(m_dbPath);

    if (!m_db.open()) {
        qCritical() << "❌ Cannot open database:" << m_db.lastError().text();
        return false;
    }

    if (!createTables()) {
        qCritical() << "❌ Cannot create SQLite tables:" << m_db.lastError().text();
        m_db.close();
        return false;
    }

    m_dbInitialized = true;
    qDebug() << "✅ Database initialized at:" << m_dbPath;

    return true;
}

bool BtrfsMonitor::createTables()
{
    QSqlQuery query(m_db);

    // Tabella filesystems
    QString createFilesystems = R"(
        CREATE TABLE IF NOT EXISTS filesystems (
            fs_uuid TEXT PRIMARY KEY,
            mount_point TEXT,
            device_count INTEGER DEFAULT 0,
            data_profile TEXT,
            metadata_profile TEXT,
            total_bytes INTEGER DEFAULT 0,
            used_bytes INTEGER DEFAULT 0,
            unallocated_bytes INTEGER DEFAULT 0,
            scrub_status TEXT,
            balance_status TEXT,
            health_state INTEGER DEFAULT 0,
            last_seen INTEGER,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        ))";

    if (!query.exec(createFilesystems)) {
        qCritical() << "Cannot create filesystems table:" << query.lastError().text();
        return false;
    }

    // ============================================================
    // BTRFS MAINTENANCE POLICY
    //
    // Configurazione persistente delle operazioni automatiche.
    // Per ora contiene esclusivamente lo scrub periodico.
    // ============================================================

    QString createMaintenancePolicy = R"(CREATE TABLE IF NOT EXISTS btrfs_maintenance_policy (fs_uuid TEXT PRIMARY KEY, auto_scrub_enabled INTEGER NOT NULL DEFAULT 0 CHECK(auto_scrub_enabled IN (0,1)), auto_scrub_interval_weeks INTEGER NOT NULL DEFAULT 4 CHECK(auto_scrub_interval_weeks BETWEEN 1 AND 6), auto_scrub_retry_after INTEGER NOT NULL DEFAULT 0 CHECK(auto_scrub_retry_after >= 0), created_at INTEGER DEFAULT (strftime('%s','now')), updated_at INTEGER DEFAULT (strftime('%s','now')), FOREIGN KEY (fs_uuid) REFERENCES filesystems(fs_uuid) ON DELETE CASCADE))";

    if (!query.exec(createMaintenancePolicy))
    {
        qCritical()
        << "Cannot create Btrfs maintenance policy table:"
        << query.lastError().text();

        return false;
    }

    // ============================================================
    // MIGRATION: AUTO SCRUB RETRY / BACKOFF
    // ============================================================

    bool hasAutoScrubRetryAfter = false;
    QSqlQuery policyColumns(m_db);

    if (!policyColumns.exec(QStringLiteral("PRAGMA table_info(btrfs_maintenance_policy)"))) {
        qCritical().noquote() << "Cannot inspect Btrfs maintenance policy table:" << policyColumns.lastError().text();
        return false;
    }

    while (policyColumns.next()) {
        if (policyColumns.value(1).toString().trimmed() == QStringLiteral("auto_scrub_retry_after")) {
            hasAutoScrubRetryAfter = true;
            break;
        }
    }

    if (!hasAutoScrubRetryAfter) {
        QSqlQuery migration(m_db);
        if (!migration.exec(QStringLiteral("ALTER TABLE btrfs_maintenance_policy ADD COLUMN auto_scrub_retry_after INTEGER NOT NULL DEFAULT 0"))) {
            qCritical().noquote() << "Cannot add automatic scrub retry column:" << migration.lastError().text();
            return false;
        }
        qInfo() << "Added automatic scrub retry/backoff column";
    }

    // Tabella device Btrfs
    QString createBtrfsDevices = R"(
    CREATE TABLE IF NOT EXISTS btrfs_devices (
        fs_uuid TEXT NOT NULL,
        device_id TEXT NOT NULL,
        device_path TEXT NOT NULL DEFAULT '',
        present INTEGER NOT NULL DEFAULT 1,

        total_bytes INTEGER NOT NULL DEFAULT 0,
        allocated_bytes INTEGER NOT NULL DEFAULT 0,
        unallocated_bytes INTEGER NOT NULL DEFAULT 0,

        read_errors INTEGER NOT NULL DEFAULT 0,
        write_errors INTEGER NOT NULL DEFAULT 0,
        flush_errors INTEGER NOT NULL DEFAULT 0,
        corruption_errors INTEGER NOT NULL DEFAULT 0,
        generation_errors INTEGER NOT NULL DEFAULT 0,

        last_seen INTEGER,
        created_at INTEGER DEFAULT (strftime('%s', 'now')),
        updated_at INTEGER DEFAULT (strftime('%s', 'now')),

        PRIMARY KEY (fs_uuid, device_id),

        CHECK(present IN (0,1)),

        FOREIGN KEY (fs_uuid)
            REFERENCES filesystems(fs_uuid)
            ON DELETE CASCADE
    ))";

    if (!query.exec(createBtrfsDevices))
    {
        qCritical() << "Cannot create btrfs_devices table:" << query.lastError().text();
        return false;
    }


    // ============================================================
    // BTRFS DEVICE REPLACEMENTS
    //
    // Storico persistente delle operazioni "btrfs replace".
    // È separato da btrfs_operations perché un replacement ha
    // informazioni proprie: source devid, target e percentuale.
    // ============================================================

    QString createDeviceReplacements = R"(
    CREATE TABLE IF NOT EXISTS btrfs_device_replacements (
        id INTEGER PRIMARY KEY AUTOINCREMENT,

        fs_uuid TEXT NOT NULL,

        source_device_id TEXT NOT NULL,
        target_device_path TEXT NOT NULL,

        status TEXT NOT NULL,
        progress_percent INTEGER NOT NULL DEFAULT -1,

        details TEXT NOT NULL DEFAULT '',

        start_time INTEGER,
        end_time INTEGER,
        duration_seconds INTEGER NOT NULL DEFAULT 0,

        created_at INTEGER DEFAULT (strftime('%s', 'now')),
        updated_at INTEGER DEFAULT (strftime('%s', 'now')),

        CHECK(progress_percent BETWEEN -1 AND 100),

        FOREIGN KEY (fs_uuid)
            REFERENCES filesystems(fs_uuid)
            ON DELETE CASCADE
    ))";

    if (!query.exec(createDeviceReplacements))
    {
        qCritical() << "Cannot create btrfs_device_replacements table:" << query.lastError().text();
       return false;
    }

    // Tabella btrfs_operations (per SCRUB e BALANCE)
    QString createOperations = R"(
        CREATE TABLE IF NOT EXISTS btrfs_operations (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            fs_uuid TEXT NOT NULL,
            operation_type TEXT NOT NULL,
            status TEXT NOT NULL,
            start_time INTEGER,
            end_time INTEGER,
            duration_seconds INTEGER DEFAULT 0,
            processed_bytes INTEGER DEFAULT 0,
            speed_bytes_per_sec INTEGER DEFAULT 0,
            error_count INTEGER DEFAULT 0,
            created_at INTEGER DEFAULT (strftime('%s', 'now')),
            updated_at INTEGER DEFAULT (strftime('%s', 'now'))
        )
    )";

    if (!query.exec(createOperations)) {
        qCritical() << "Cannot create operations table:" << query.lastError().text();
        return false;
    }

    // Indici
    QStringList createIndexes = {
        "CREATE INDEX IF NOT EXISTS idx_ops_fs_uuid ON btrfs_operations(fs_uuid)",
        "CREATE INDEX IF NOT EXISTS idx_ops_type ON btrfs_operations(operation_type)",
        "CREATE INDEX IF NOT EXISTS idx_ops_status ON btrfs_operations(status)",
        "CREATE INDEX IF NOT EXISTS idx_ops_created ON btrfs_operations(created_at DESC)",

        "CREATE INDEX IF NOT EXISTS idx_replace_fs_uuid ON btrfs_device_replacements(fs_uuid)",
        "CREATE INDEX IF NOT EXISTS idx_replace_status ON btrfs_device_replacements(status)",
        "CREATE INDEX IF NOT EXISTS idx_replace_created ON btrfs_device_replacements(created_at DESC)",
    };

    for (const QString &indexQuery : createIndexes) {
        if (!query.exec(indexQuery)) {
            qWarning() << "Cannot create index:" << query.lastError().text();
        }
    }

    // ============================================================
    // REPAIR LEGACY INTERRUPTED SCRUB OPERATIONS
    //
    // Versioni precedenti potevano salvare lo stato "interrupted"
    // senza chiudere la riga con end_time.
    // La riparazione è idempotente: agisce solo sulle righe ancora
    // aperte.
    // ============================================================

    QSqlQuery repairInterruptedScrubs(m_db);

    if (!repairInterruptedScrubs.exec(R"(UPDATE btrfs_operations SET end_time = CASE WHEN updated_at IS NOT NULL AND updated_at > 0 THEN updated_at WHEN created_at IS NOT NULL AND created_at > 0 THEN created_at WHEN start_time IS NOT NULL AND start_time > 0 THEN start_time ELSE strftime('%s', 'now') END, duration_seconds = CASE WHEN start_time IS NOT NULL AND start_time > 0 THEN MAX(0, (CASE WHEN updated_at IS NOT NULL AND updated_at > 0 THEN updated_at WHEN created_at IS NOT NULL AND created_at > 0 THEN created_at ELSE start_time END) - start_time) ELSE 0 END WHERE operation_type = 'scrub' AND LOWER(TRIM(status)) = 'interrupted' AND end_time IS NULL)"))
    {
        qWarning().noquote() << "Cannot repair legacy interrupted scrub operations:" << repairInterruptedScrubs.lastError().text();
        return false;
    }

    if (repairInterruptedScrubs.numRowsAffected() > 0)
    {
        qInfo() << "Repaired" << repairInterruptedScrubs.numRowsAffected() << "legacy interrupted scrub operation(s)";
    }

    return true;
}

bool BtrfsMonitor::autoScrubConfig(const QString &fsUuid, AutoScrubConfig &config) const
{
    // Valori di default (disabilitato, intervallo 4 settimane)
    config = AutoScrubConfig{};

    const QString uuid = fsUuid.trimmed();
    if (uuid.isEmpty()) return false;

    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    QSqlQuery query(m_db);
    query.prepare(R"(
        SELECT auto_scrub_enabled, auto_scrub_interval_weeks
        FROM btrfs_maintenance_policy
        WHERE fs_uuid = :fs_uuid
        LIMIT 1
    )");

    query.bindValue(QStringLiteral(":fs_uuid"), uuid);

    if (!query.exec()) {
        qWarning() << "Cannot read automatic scrub configuration:" << query.lastError().text();
        return false;
    }

    // Se non ci sono righe, non è un errore e restano i valori di default
    if (!query.next()) return true;

    config.enabled = query.value(0).toInt() != 0;
    config.intervalWeeks = qBound(1, query.value(1).toInt(), 6);

    return true;
}

bool BtrfsMonitor::setAutoScrubConfig(const QString &fsUuid, bool enabled, int intervalWeeks)
{
    const QString uuid = fsUuid.trimmed();
    if (uuid.isEmpty()) return false;

    if (intervalWeeks < 1 || intervalWeeks > 6) {
        qWarning() << "Invalid automatic scrub interval:" << intervalWeeks;
        return false;
    }

    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT INTO btrfs_maintenance_policy (fs_uuid, auto_scrub_enabled, auto_scrub_interval_weeks, updated_at)
        VALUES (:fs_uuid, :enabled, :interval_weeks, strftime('%s', 'now'))
        ON CONFLICT(fs_uuid) DO UPDATE SET
            auto_scrub_enabled = excluded.auto_scrub_enabled,
            auto_scrub_interval_weeks = excluded.auto_scrub_interval_weeks,
            updated_at = strftime('%s', 'now')
    )");

    query.bindValue(QStringLiteral(":fs_uuid"), uuid);
    query.bindValue(QStringLiteral(":enabled"), enabled ? 1 : 0);
    query.bindValue(QStringLiteral(":interval_weeks"), intervalWeeks);

    if (!query.exec()) {
        qWarning() << "Cannot save automatic scrub configuration:" << query.lastError().text();
        return false;
    }

    qInfo().noquote() << "Automatic scrub configuration saved:" << uuid << "enabled:" << enabled << "interval:" << intervalWeeks << "week(s)";
    return true;
}

bool BtrfsMonitor::autoScrubConfigForMountPoint(const QString &mountPoint, AutoScrubConfig &config) const
{
    config = AutoScrubConfig{};

    const QString path = mountPoint.trimmed();
    if (path.isEmpty()) return false;

    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    // Il client D-Bus conosce il mount point, la configurazione è indicizzata tramite fs_uuid
    QSqlQuery query(m_db);
    query.prepare(R"(
        SELECT fs_uuid FROM filesystems
        WHERE mount_point = :mount_point
        ORDER BY last_seen DESC LIMIT 1
    )");

    query.bindValue(QStringLiteral(":mount_point"), path);

    if (!query.exec()) {
        qWarning() << "Cannot resolve filesystem UUID for automatic scrub:" << query.lastError().text();
        return false;
    }

    if (!query.next()) {
        qWarning().noquote() << "Automatic scrub configuration: filesystem not found for mount point" << path;
        return false;
    }

    const QString fsUuid = query.value(0).toString().trimmed();
    if (fsUuid.isEmpty()) return false;

    return autoScrubConfig(fsUuid, config);
}

bool BtrfsMonitor::setAutoScrubRetryAfterForMountPoint(const QString &mountPoint, qint64 retryAfterEpoch)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty()) return false;
    if (retryAfterEpoch < 0) return false;
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    // ============================================================
    // MOUNT POINT -> FS UUID
    // ============================================================
    QSqlQuery fsQuery(m_db);
    fsQuery.prepare(R"(SELECT fs_uuid FROM filesystems WHERE mount_point = :mount_point ORDER BY last_seen DESC LIMIT 1)");
    fsQuery.bindValue(QStringLiteral(":mount_point"), path);

    if (!fsQuery.exec()) {
        qWarning().noquote() << "Cannot resolve filesystem UUID for automatic scrub retry:" << fsQuery.lastError().text();
        return false;
    }

    if (!fsQuery.next()) {
        qWarning().noquote() << "Automatic scrub retry: filesystem not found for mount point" << path;
        return false;
    }

    const QString fsUuid = fsQuery.value(0).toString().trimmed();
    if (fsUuid.isEmpty()) return false;

    // ============================================================
    // SAVE RETRY TIME
    // ============================================================
    QSqlQuery updateQuery(m_db);
    updateQuery.prepare(R"(UPDATE btrfs_maintenance_policy SET auto_scrub_retry_after = :retry_after WHERE fs_uuid = :fs_uuid)");
    updateQuery.bindValue(QStringLiteral(":retry_after"), retryAfterEpoch);
    updateQuery.bindValue(QStringLiteral(":fs_uuid"), fsUuid);

    if (!updateQuery.exec()) {
        qWarning().noquote() << "Cannot save automatic scrub retry time:" << updateQuery.lastError().text();
        return false;
    }

    qInfo().noquote() << "Automatic scrub retry/backoff saved:" << path << "retry after:" << retryAfterEpoch;
    return true;
}

bool BtrfsMonitor::setAutoScrubConfigForMountPoint(const QString &mountPoint, bool enabled, int intervalWeeks)
{
    const QString path = mountPoint.trimmed();
    if (path.isEmpty()) return false;

    if (intervalWeeks < 1 || intervalWeeks > 6) {
        qWarning() << "Invalid automatic scrub interval:" << intervalWeeks;
        return false;
    }

    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    // L'interfaccia esterna usa il mount point, la policy nel database è indicizzata tramite fs_uuid
    QSqlQuery query(m_db);
    query.prepare(R"(
        SELECT fs_uuid FROM filesystems
        WHERE mount_point = :mount_point
        ORDER BY last_seen DESC LIMIT 1
    )");

    query.bindValue(QStringLiteral(":mount_point"), path);

    if (!query.exec()) {
        qWarning() << "Cannot resolve filesystem UUID for automatic scrub:" << query.lastError().text();
        return false;
    }

    if (!query.next()) {
        qWarning().noquote() << "Set automatic scrub configuration: filesystem not found for mount point" << path;
        return false;
    }

    const QString fsUuid = query.value(0).toString().trimmed();
    if (fsUuid.isEmpty()) return false;

    return setAutoScrubConfig(fsUuid, enabled, intervalWeeks);
}

bool BtrfsMonitor::autoScrubScheduleForMountPoint(const QString &mountPoint, AutoScrubSchedule &schedule) const
{
    schedule = AutoScrubSchedule{};
    const QString path = mountPoint.trimmed();

    if (path.isEmpty()) return false;
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    // ============================================================
    // RISOLVI MOUNT POINT -> FS UUID
    // ============================================================
    QSqlQuery fsQuery(m_db);
    fsQuery.prepare(R"(SELECT fs_uuid FROM filesystems WHERE mount_point = :mount_point ORDER BY last_seen DESC LIMIT 1)");
    fsQuery.bindValue(QStringLiteral(":mount_point"), path);

    if (!fsQuery.exec()) {
        qWarning().noquote() << "Cannot resolve filesystem UUID for automatic scrub schedule:" << fsQuery.lastError().text();
        return false;
    }

    if (!fsQuery.next()) {
        qWarning().noquote() << "Automatic scrub schedule: filesystem not found for mount point" << path;
        return false;
    }

    const QString fsUuid = fsQuery.value(0).toString().trimmed();
    if (fsUuid.isEmpty()) return false;

    // ============================================================
    // CONFIGURAZIONE
    // ============================================================
    AutoScrubConfig config;
    if (!autoScrubConfig(fsUuid, config)) {
        qWarning().noquote() << "Cannot read automatic scrub configuration for schedule:" << fsUuid;
        return false;
    }

    // ============================================================
    // ULTIMO SCRUB RIUSCITO
    // ============================================================
    QSqlQuery scrubQuery(m_db);
    scrubQuery.prepare(R"(SELECT end_time FROM btrfs_operations WHERE fs_uuid = :fs_uuid AND operation_type = 'scrub' AND end_time IS NOT NULL AND end_time > 0 AND LOWER(TRIM(status)) IN ('finished', 'completed', 'complete', 'done') ORDER BY end_time DESC, id DESC LIMIT 1)");
    scrubQuery.bindValue(QStringLiteral(":fs_uuid"), fsUuid);

    if (!scrubQuery.exec()) {
        qWarning().noquote() << "Cannot read last successful scrub:" << scrubQuery.lastError().text();
        return false;
    }

    if (scrubQuery.next()) {
        const qint64 lastSuccessfulEpoch = scrubQuery.value(0).toLongLong();
        if (lastSuccessfulEpoch > 0) schedule.lastSuccessfulEpoch = lastSuccessfulEpoch;
    }

    if (!config.enabled) {
        schedule.nextDueEpoch = 0;
        return true;
    }

    // ============================================================
    // ANCHOR DELLA SCHEDULAZIONE
    // ============================================================
    qint64 scheduleAnchor = schedule.lastSuccessfulEpoch;

    if (scheduleAnchor <= 0) {
        QSqlQuery policyQuery(m_db);
        policyQuery.prepare(R"(SELECT updated_at, created_at FROM btrfs_maintenance_policy WHERE fs_uuid = :fs_uuid AND auto_scrub_enabled = 1 LIMIT 1)");
        policyQuery.bindValue(QStringLiteral(":fs_uuid"), fsUuid);

        if (!policyQuery.exec()) {
            qWarning().noquote() << "Cannot read automatic scrub policy timestamp:" << policyQuery.lastError().text();
            return false;
        }

        if (!policyQuery.next()) return true;

        scheduleAnchor = policyQuery.value(0).toLongLong();
        if (scheduleAnchor <= 0) scheduleAnchor = policyQuery.value(1).toLongLong();
    }

    if (scheduleAnchor <= 0) return true;

    constexpr qint64 secondsPerWeek = 7LL * 24LL * 60LL * 60LL;
    schedule.nextDueEpoch = scheduleAnchor + static_cast<qint64>(config.intervalWeeks) * secondsPerWeek;

    // ============================================================
    // RETRY / BACKOFF DOPO TENTATIVO AUTOMATICO
    // ============================================================
    QSqlQuery retryQuery(m_db);
    retryQuery.prepare(R"(SELECT auto_scrub_retry_after FROM btrfs_maintenance_policy WHERE fs_uuid = :fs_uuid LIMIT 1)");
    retryQuery.bindValue(QStringLiteral(":fs_uuid"), fsUuid);

    if (!retryQuery.exec()) {
        qWarning().noquote() << "Cannot read automatic scrub retry time:" << retryQuery.lastError().text();
        return false;
    }

    if (retryQuery.next()) {
        const qint64 retryAfterEpoch = retryQuery.value(0).toLongLong();
        /* La prossima esecuzione automatica non può precedere né la scadenza normale né il backoff. */
        if (retryAfterEpoch > schedule.nextDueEpoch) schedule.nextDueEpoch = retryAfterEpoch;
    }

    return true;
}

QStringList BtrfsMonitor::currentBtrfsMountPoints() const
{
    QStringList mountPoints;

    for (auto it = m_filesystems.constBegin(); it != m_filesystems.constEnd(); ++it) {
        const QString path = it.value().mountPoint.trimmed();
        if (path.isEmpty()) continue;
        if (!mountPoints.contains(path)) mountPoints.append(path);
    }

    mountPoints.sort(Qt::CaseInsensitive);
    return mountPoints;
}

bool BtrfsMonitor::dueAutoScrubMountPoints(qint64 nowEpoch, QStringList &mountPoints) const
{
    mountPoints.clear();
    if (nowEpoch <= 0) return false;
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    // ============================================================
    // FILESYSTEM CON AUTO SCRUB ABILITATO
    // ============================================================
    QSqlQuery query(m_db);
    query.prepare(R"(SELECT f.mount_point FROM btrfs_maintenance_policy AS p JOIN filesystems AS f ON f.fs_uuid = p.fs_uuid WHERE p.auto_scrub_enabled = 1 AND f.mount_point IS NOT NULL AND TRIM(f.mount_point) <> '' ORDER BY f.mount_point COLLATE NOCASE)");

    if (!query.exec()) {
        qWarning().noquote() << "Cannot read automatic scrub candidates:" << query.lastError().text();
        return false;
    }

    QStringList candidates;
    while (query.next()) {
        const QString path = query.value(0).toString().trimmed();
        if (!path.isEmpty()) candidates.append(path);
    }

    /*
     * Non utilizziamo filesystem rimasti soltanto nello storico SQLite.
     * Devono essere realmente presenti nella scansione corrente di /sys/fs/btrfs.
     */
    const QStringList currentMountPoints = currentBtrfsMountPoints();

    /*
     * Ordiniamo gli scrub scaduti dalla scadenza più vecchia alla più recente.
     */
    QMap<qint64, QStringList> dueByEpoch;

    for (const QString &path : std::as_const(candidates)) {
        if (!currentMountPoints.contains(path)) continue;

        AutoScrubSchedule schedule;
        if (!autoScrubScheduleForMountPoint(path, schedule)) {
            qWarning().noquote() << "Cannot calculate automatic scrub schedule for" << path;
            continue;
        }

        if (schedule.nextDueEpoch <= 0) continue;
        if (schedule.nextDueEpoch > nowEpoch) continue;

        dueByEpoch[schedule.nextDueEpoch].append(path);
    }

    for (auto it = dueByEpoch.constBegin(); it != dueByEpoch.constEnd(); ++it) {
        QStringList paths = it.value();
        paths.sort(Qt::CaseInsensitive);
        for (const QString &path : std::as_const(paths)) mountPoints.append(path);
    }

    return true;
}

bool BtrfsMonitor::saveFilesystemState(const QString &fsUuid, const FilesystemState &state)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen())
        return false;

    const QString scrubStatus = m_scrubStates.value(fsUuid).status;
    const QString balanceStatus = m_balanceStates.value(fsUuid).status;

    QSqlQuery query(m_db);
    query.prepare(R"(
        UPDATE filesystems SET
            mount_point = :mount_point, device_count = :device_count,
            data_profile = :data_profile, metadata_profile = :metadata_profile,
            total_bytes = :total_bytes, used_bytes = :used_bytes,
            unallocated_bytes = :unallocated_bytes,
            scrub_status = :scrub_status, balance_status = :balance_status,
            health_state = :health_state, last_seen = :last_seen,
            updated_at = strftime('%s', 'now')
        WHERE fs_uuid = :fs_uuid
    )");

    query.bindValue(":fs_uuid", fsUuid);
    query.bindValue(":mount_point", state.mountPoint);
    query.bindValue(":device_count", state.deviceCount);
    query.bindValue(":data_profile", profileToString(state.space.dataProfile));
    query.bindValue(":metadata_profile", profileToString(state.space.metadataProfile));
    query.bindValue(":total_bytes", static_cast<qint64>(state.space.total + state.space.unallocated));
    query.bindValue(":used_bytes", static_cast<qint64>(state.space.used));
    query.bindValue(":unallocated_bytes", static_cast<qint64>(state.space.unallocated));
    query.bindValue(":scrub_status", scrubStatus);
    query.bindValue(":balance_status", balanceStatus);
    query.bindValue(":health_state", static_cast<int>(state.health));
    query.bindValue(":last_seen", QDateTime::currentSecsSinceEpoch());

    if (!query.exec()) {
        qWarning() << "❌ Cannot update filesystem state:" << query.lastError().text();
        return false;
    }

    if (query.numRowsAffected() == 0) {
        QSqlQuery insertQuery(m_db);
        insertQuery.prepare(R"(
            INSERT INTO filesystems (
                fs_uuid, mount_point, device_count, data_profile, metadata_profile,
                total_bytes, used_bytes, unallocated_bytes, scrub_status, balance_status,
                health_state, last_seen
            ) VALUES (
                :fs_uuid, :mount_point, :device_count, :data_profile, :metadata_profile,
                :total_bytes, :used_bytes, :unallocated_bytes, :scrub_status, :balance_status,
                :health_state, :last_seen
            )
        )");
        insertQuery.bindValue(":fs_uuid", fsUuid);
        insertQuery.bindValue(":mount_point", state.mountPoint);
        insertQuery.bindValue(":device_count", state.deviceCount);
        insertQuery.bindValue(":data_profile", profileToString(state.space.dataProfile));
        insertQuery.bindValue(":metadata_profile", profileToString(state.space.metadataProfile));
        insertQuery.bindValue(":total_bytes", static_cast<qint64>(state.space.total + state.space.unallocated));
        insertQuery.bindValue(":used_bytes", static_cast<qint64>(state.space.used));
        insertQuery.bindValue(":unallocated_bytes", static_cast<qint64>(state.space.unallocated));
        insertQuery.bindValue(":scrub_status", scrubStatus);
        insertQuery.bindValue(":balance_status", balanceStatus);
        insertQuery.bindValue(":health_state", static_cast<int>(state.health));
        insertQuery.bindValue(":last_seen", QDateTime::currentSecsSinceEpoch());
        if (!insertQuery.exec()) {
            qWarning() << "❌ Cannot insert filesystem state:" << insertQuery.lastError().text();
            return false;
        }
    }
    return true;
}

bool BtrfsMonitor::saveBtrfsDevices(const QString &fsUuid, const FilesystemState &state)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen())
    {
        return false;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();

    QSqlQuery query(m_db);

    query.prepare(R"(
        INSERT INTO btrfs_devices (
            fs_uuid,
            device_id,
            device_path,
            present,
            total_bytes,
            allocated_bytes,
            unallocated_bytes,
            read_errors,
            write_errors,
            flush_errors,
            corruption_errors,
            generation_errors,
            last_seen,
            updated_at
        )
        VALUES (
            :fs_uuid,
            :device_id,
            :device_path,
            :present,
            :total_bytes,
            :allocated_bytes,
            :unallocated_bytes,
            :read_errors,
            :write_errors,
            :flush_errors,
            :corruption_errors,
            :generation_errors,
            :last_seen,
            strftime('%s', 'now')
        )
        ON CONFLICT(fs_uuid, device_id)
        DO UPDATE SET
            device_path =
             CASE
                 WHEN excluded.device_path <> ''
                     THEN excluded.device_path
                 ELSE btrfs_devices.device_path
             END,
            present = excluded.present,
            total_bytes = excluded.total_bytes,
            allocated_bytes = excluded.allocated_bytes,
            unallocated_bytes = excluded.unallocated_bytes,
            read_errors = excluded.read_errors,
            write_errors = excluded.write_errors,
            flush_errors = excluded.flush_errors,
            corruption_errors = excluded.corruption_errors,
            generation_errors = excluded.generation_errors,
            last_seen = excluded.last_seen,
            updated_at = strftime('%s', 'now')
    )");

    for (auto it = state.devices.constBegin();
         it != state.devices.constEnd();
         ++it)
    {
        const QString &deviceId = it.key();

        const DeviceState &device = it.value();

        const DeviceStats &stats = it.value().stats;

        query.bindValue(QStringLiteral(":fs_uuid"), fsUuid);
        query.bindValue(QStringLiteral(":device_id"), deviceId);
        query.bindValue(QStringLiteral(":device_path"), stats.devicePath.isNull() ? QString() : stats.devicePath);
        query.bindValue(QStringLiteral(":present"), device.present ? 1 : 0);
        query.bindValue(QStringLiteral(":total_bytes"), static_cast<qint64>(stats.deviceTotal));
        query.bindValue(QStringLiteral(":allocated_bytes"), static_cast<qint64>(stats.deviceAllocated));
        query.bindValue(QStringLiteral(":unallocated_bytes"), static_cast<qint64>(stats.deviceUnallocated));
        query.bindValue(QStringLiteral(":read_errors"), static_cast<qint64>(stats.readErrors));
        query.bindValue(QStringLiteral(":write_errors"), static_cast<qint64>(stats.writeErrors));
        query.bindValue(QStringLiteral(":flush_errors"), static_cast<qint64>(stats.flushErrors));
        query.bindValue(QStringLiteral(":corruption_errors"), static_cast<qint64>(stats.corruptionErrors));
        query.bindValue(QStringLiteral(":generation_errors"), static_cast<qint64>(stats.generationErrors));
        query.bindValue(QStringLiteral(":last_seen"), now);


        if (!query.exec())
        {
            qWarning().noquote() << "Cannot save Btrfs device" << deviceId << "for filesystem"  << fsUuid.left(8) << ":" << query.lastError().text();
            return false;
        }

        qDebug().noquote() << "💾 Saved Btrfs device:" << deviceId << stats.devicePath << "for" << fsUuid.left(8);
    }

    return true;
}

bool BtrfsMonitor::removeStaleBtrfsDevices(const QString &fsUuid, const QSet<QString> &memberDeviceIds)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) return false;

    /*
     * Non eseguiamo mai una pulizia con una lista vuota.
     * Una scansione devinfo vuota viene considerata
     * non valida dal chiamante.
     */
    if (memberDeviceIds.isEmpty()) return false;

    if (!m_db.transaction()) {
        qWarning() << "Cannot start Btrfs device cleanup transaction:" << m_db.lastError().text();
        return false;
    }

    QSqlQuery selectQuery(m_db);
    selectQuery.prepare(R"(SELECT device_id FROM btrfs_devices WHERE fs_uuid = :fs_uuid)");
    selectQuery.bindValue(QStringLiteral(":fs_uuid"), fsUuid);

    if (!selectQuery.exec()) {
        qWarning() << "Cannot read stored Btrfs devices:" << selectQuery.lastError().text();
        m_db.rollback();
        return false;
    }

    QStringList staleDeviceIds;
    while (selectQuery.next()) {
        const QString deviceId = selectQuery.value(0).toString();
        if (!memberDeviceIds.contains(deviceId)) staleDeviceIds.append(deviceId);
    }

    QSqlQuery deleteQuery(m_db);
    deleteQuery.prepare(R"(DELETE FROM btrfs_devices WHERE fs_uuid = :fs_uuid AND device_id = :device_id)");

    for (const QString &deviceId : std::as_const(staleDeviceIds)) {
        deleteQuery.bindValue(QStringLiteral(":fs_uuid"), fsUuid);
        deleteQuery.bindValue(QStringLiteral(":device_id"), deviceId);

        if (!deleteQuery.exec()) {
            qWarning().noquote() << "Cannot remove stale Btrfs device" << deviceId << "for" << fsUuid.left(8) << ":" << deleteQuery.lastError().text();
            m_db.rollback();
            return false;
        }
        qDebug().noquote() << "🗑 Removed Btrfs device no longer member:" << deviceId << "from" << fsUuid.left(8);
    }

    if (!m_db.commit()) {
        qWarning() << "Cannot commit Btrfs device cleanup:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }

    return true;
}

bool BtrfsMonitor::saveOperationState(const QString &fsUuid, const QString &operationType, const QString &status, quint64 processedBytes, quint64 speed, int errorCount)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen())
    {
        return false;
    }

    const QString normalized = status.trimmed().toLower();

    /*
     * running / started / paused appartengono tutti
     * ad una operazione ancora aperta.
     */
    const bool isActive = normalized == QStringLiteral("running") || normalized == QStringLiteral("started") || normalized == QStringLiteral("paused");

    // ============================================================
    // CERCA UNA OPERAZIONE GIÀ APERTA
    // ============================================================

    QSqlQuery findQuery(m_db);

    /*
     * L'indicatore affidabile di operazione aperta è end_time IS NULL.
     *
     * In particolare un balance "paused" è ancora la stessa operazione.
     */
    findQuery.prepare(R"(
        SELECT id
        FROM btrfs_operations
        WHERE fs_uuid = :fs_uuid
          AND operation_type = :operation_type
          AND end_time IS NULL
        ORDER BY id DESC
        LIMIT 1
    )");

    findQuery.bindValue(":fs_uuid", fsUuid);

    findQuery.bindValue(":operation_type", operationType);

    if (!findQuery.exec())
    {
        qWarning() << "❌ Cannot search open operation:" << findQuery.lastError().text();
        return false;
    }

    if (findQuery.next())
    {
        /*
         * Esiste già una riga aperta.
         * Aggiorniamo quella invece di crearne una seconda.
         */
        return updateOperationState(
            fsUuid,
            operationType,
            normalized,
            processedBytes,
            speed,
            errorCount);
    }


    // ============================================================
    // NESSUNA OPERAZIONE APERTA
    // ============================================================

    /*
     * Uno stato terminale osservato al primo polling non significa
     * che l'agent abbia appena visto terminare una operazione.
     *
     * Esempio:
     *
     * daemon parte -> "not running"
     *
     * Non deve creare una riga fittizia.
     */
    if (!isActive)
    {
        qDebug() << "No open" << operationType << "operation found; ignoring terminal state:" << normalized << "for" << fsUuid.left(8);
        return true;
    }


    // ============================================================
    // NUOVA OPERAZIONE
    // ============================================================

    QSqlQuery query(m_db);

    query.prepare(R"(
        INSERT INTO btrfs_operations (
            fs_uuid,
            operation_type,
            status,
            start_time,
            end_time,
            duration_seconds,
            processed_bytes,
            speed_bytes_per_sec,
            error_count,
            created_at,
            updated_at
        )
        VALUES (
            :fs_uuid,
            :operation_type,
            :status,
            :start_time,
            NULL,
            0,
            :processed_bytes,
            :speed_bytes_per_sec,
            :error_count,
            strftime('%s', 'now'),
            strftime('%s', 'now')
        )
    )");

    const qint64 now = QDateTime::currentSecsSinceEpoch();

    query.bindValue(":fs_uuid", fsUuid);

    query.bindValue(":operation_type", operationType);

    query.bindValue(":status", normalized);

    query.bindValue(":start_time", now);

    query.bindValue(":processed_bytes", static_cast<qint64>(processedBytes));

    query.bindValue(":speed_bytes_per_sec", static_cast<qint64>(speed));

    query.bindValue(":error_count", errorCount);

    if (!query.exec())
    {
        qWarning() << "❌ Cannot save operation state:" << query.lastError().text();
        return false;
    }

    qDebug() << "✅ Saved new" << operationType << "operation:" << normalized << "for" << fsUuid.left(8);

    return true;
}


bool BtrfsMonitor::updateOperationState(const QString &fsUuid, const QString &operationType, const QString &status, quint64 processedBytes, quint64 speed, int errorCount)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen()) {
        return false;
    }

    const QString normalized = status.trimmed().toLower();

    /*
     * Cerchiamo l'operazione APERTA.
     *
     * Non usiamo lo status per determinarlo:
     * una operazione paused, ad esempio, è ancora aperta.
     *
     * L'indicatore corretto è end_time IS NULL.
     */
    QSqlQuery findQuery(m_db);

    findQuery.prepare(R"(
        SELECT id, start_time
        FROM btrfs_operations
        WHERE fs_uuid = :fs_uuid
          AND operation_type = :operation_type
          AND end_time IS NULL
        ORDER BY id DESC
        LIMIT 1
    )");

    findQuery.bindValue(
        ":fs_uuid",
        fsUuid);

    findQuery.bindValue(
        ":operation_type",
        operationType);

    if (!findQuery.exec()) {
        qWarning()
        << "❌ Cannot find open operation record:"
        << findQuery.lastError().text();

        return false;
    }

    /*
     * Nessuna operazione aperta.
     */
    if (!findQuery.next()) {

        /*
         * Se lo stato ricevuto indica una nuova
         * operazione attiva, lasciamo a
         * saveOperationState() la creazione della riga.
         */
        const bool isActive =
            normalized == QStringLiteral("running") ||
            normalized == QStringLiteral("started") ||
            normalized == QStringLiteral("paused");

        if (isActive) {
            return saveOperationState(
                fsUuid,
                operationType,
                status,
                processedBytes,
                speed,
                errorCount);
        }

        /*
         * Uno stato terminale senza una operazione
         * aperta non deve creare una riga fittizia.
         *
         * Esempio:
         *
         * daemon parte
         * status = "not running"
         *
         * Non significa che sia appena terminato
         * uno scrub/balance.
         */
        qDebug()
            << "No open"
            << operationType
            << "operation for terminal state:"
            << status
            << "on"
            << fsUuid.left(8);

        return true;
    }

    const qint64 recordId = findQuery.value(0).toLongLong();

    const qint64 startTime = findQuery.value(1).toLongLong();

    /*
     * Stati terminali conosciuti.
     *
     * cancelled/canceled sono importanti soprattutto
     * per scrub/balance interrotti volontariamente.
     */
    const bool isFinished =
        normalized == QStringLiteral("finished") ||
        normalized == QStringLiteral("completed") ||
        normalized == QStringLiteral("complete") ||
        normalized == QStringLiteral("done") ||
        normalized == QStringLiteral("not running") ||
        normalized == QStringLiteral("cancelled") ||
        normalized == QStringLiteral("canceled") ||
        normalized == QStringLiteral("interrupted") ||
        normalized.contains(QStringLiteral("error")) ||
        normalized.contains(QStringLiteral("failed"));

    QSqlQuery updateQuery(m_db);

    if (isFinished) {

        const qint64 endTime = QDateTime::currentSecsSinceEpoch();

        const qint64 duration = (startTime > 0 && endTime >= startTime) ? endTime - startTime : 0;

        updateQuery.prepare(R"(
            UPDATE btrfs_operations
            SET status = :status,
                end_time = :end_time,
                duration_seconds = :duration_seconds,
                processed_bytes = :processed_bytes,
                speed_bytes_per_sec = :speed_bytes_per_sec,
                error_count = :error_count,
                updated_at = strftime('%s', 'now')
            WHERE id = :id
        )");

        updateQuery.bindValue(":end_time", endTime);

        updateQuery.bindValue(":duration_seconds", duration);

        qDebug() << "⏱️ Operation" << operationType << "closed after" << duration << "seconds";

    } else {

        /*
         * Operazione ancora aperta.
         *
         * Vale ad esempio per:
         *
         * running
         * started
         * paused
         *
         * Non tocchiamo end_time.
         */
        updateQuery.prepare(R"(
            UPDATE btrfs_operations
            SET status = :status,
                processed_bytes = :processed_bytes,
                speed_bytes_per_sec = :speed_bytes_per_sec,
                error_count = :error_count,
                updated_at = strftime('%s', 'now')
            WHERE id = :id
        )");
    }

    updateQuery.bindValue(":id", recordId);

    updateQuery.bindValue(":status", normalized);

    updateQuery.bindValue(":processed_bytes", static_cast<qint64>(processedBytes));

    updateQuery.bindValue(":speed_bytes_per_sec", static_cast<qint64>(speed));

    updateQuery.bindValue(":error_count", errorCount);

    if (!updateQuery.exec()) {
        qWarning() << "❌ Cannot update operation state:" << updateQuery.lastError().text();

        return false;
    }

    qDebug() << "✅ Updated" << operationType << "record:"  << normalized << "for" << fsUuid.left(8);

    return true;
}

bool BtrfsMonitor::getLastOperationState(const QString &fsUuid, const QString &operationType, QString &status, QDateTime &startTime) const
{
    status.clear();
    startTime = QDateTime();

    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(R"(
        SELECT status, start_time
        FROM btrfs_operations
        WHERE fs_uuid = :fs_uuid AND operation_type = :operation_type
        ORDER BY id DESC LIMIT 1
    )");
    query.bindValue(":fs_uuid", fsUuid);
    query.bindValue(":operation_type", operationType);

    if (!query.exec()) {
        qWarning() << "❌ Cannot get last operation state:"
                   << query.lastError().text();
        return false;
    }
    if (!query.next())
        return false;

    status = query.value(0).toString();
    const qint64 startTimeSecs = query.value(1).toLongLong();
    if (startTimeSecs > 0)
        startTime = QDateTime::fromSecsSinceEpoch(startTimeSecs);
    return true;
}

// ============================================================
// READ SCRUB STATUS FROM SYSFS (NUOVO)
// ============================================================

void BtrfsMonitor::readScrubStatus(const QString &mountPoint, QString &status, quint64 &processedBytes, quint64 &speed, int &errorCount) const
{
    status.clear();
    processedBytes = 0;
    speed = 0;
    errorCount = 0;


    if (mountPoint.trimmed().isEmpty()) {
        qWarning()
        << "Cannot read scrub status:"
        << "empty mount point";
        return;
    }


    const QString btrfsExecutable =
        QStandardPaths::findExecutable(
            QStringLiteral("btrfs"));

    if (btrfsExecutable.isEmpty()) {
        qWarning()
        << "Cannot read scrub status:"
        << "btrfs executable not found";
        return;
    }


    QProcess process;


    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();

    environment.insert(
        QStringLiteral("LC_ALL"),
        QStringLiteral("C"));

    process.setProcessEnvironment(
        environment);


    process.start(
        btrfsExecutable,
        {
            QStringLiteral("scrub"),
            QStringLiteral("status"),
            mountPoint
        });


    if (!process.waitForStarted(2000)) {
        qWarning().noquote()
        << "Cannot start btrfs scrub status:"
        << process.errorString();

        return;
    }


    if (!process.waitForFinished(5000)) {
        qWarning().noquote()
        << "Timeout reading scrub status for"
        << mountPoint;

        process.kill();
        process.waitForFinished();

        return;
    }


    const QString stdoutText =
        QString::fromUtf8(
            process.readAllStandardOutput());

    const QString stderrText =
        QString::fromUtf8(
            process.readAllStandardError());

    const QString output =
        (stdoutText +
         QStringLiteral("\n") +
         stderrText)
            .trimmed();


    qDebug().noquote() << "🧹 Scrub status:" << mountPoint << "exit:" << process.exitCode() << "\n" << output;

    /*
     * --------------------------------------------------------
     * REGEX
     * --------------------------------------------------------
     */

    static const QRegularExpression statusRegex(QStringLiteral("^Status:\\s*(.+)$"),QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression bytesRegex(QStringLiteral("^Bytes scrubbed:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?i?B|B)?"),QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression rateRegex(QStringLiteral("^Rate:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?i?B|B)?/s"),QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression errorValueRegex(QStringLiteral("\\b(?:csum|super|verify|read)=(\\d+)"),QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression totalRegex(QStringLiteral("^Total to scrub:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?i?B|B)?"), QRegularExpression::CaseInsensitiveOption);

    const QStringList lines =  output.split(QChar('\n'),Qt::SkipEmptyParts);

    quint64 totalToScrubBytes = 0;

    for (const QString &line : lines) {

        const QString trimmed = line.trimmed();

        /*
         * ----------------------------------------------------
         * STATUS
         * ----------------------------------------------------
         */
        {
            const QRegularExpressionMatch match = statusRegex.match(trimmed);

            if (match.hasMatch()) {

                QString rawStatus =
                    match.captured(1)
                        .trimmed()
                        .toLower();


                if (rawStatus.contains(QStringLiteral("running"))) {

                    status = QStringLiteral("running");
                }
                else if (rawStatus.contains(QStringLiteral("finished")) || rawStatus.contains(QStringLiteral("completed"))) {

                    status = QStringLiteral("finished");
                }
                else if (rawStatus.contains(QStringLiteral("aborted")) ||
                    rawStatus.contains(QStringLiteral("cancelled")) ||
                    rawStatus.contains(QStringLiteral("canceled"))) {

                    /*
                     * Normalizziamo tutte queste forme
                     * nella chiave persistente usata dal DB.
                     */
                    status = QStringLiteral("cancelled");
                }
                else {
                    status =
                        rawStatus;
                }

                continue;
            }
        }

        // ----------------------------------------------------
        // TOTAL TO SCRUB
        // ----------------------------------------------------
        {
            const QRegularExpressionMatch match = totalRegex.match(trimmed);

            if (match.hasMatch())
            {
                const double value = match.captured(1).toDouble();

                QString unit = match.captured(2);

                if (unit.isEmpty())
                    unit = QStringLiteral("B");

                totalToScrubBytes =
                    convertToBytes(value, unit);

                continue;
            }
        }

        /*
         * ----------------------------------------------------
         * BYTES SCRUBBED
         * ----------------------------------------------------
         */

        {
            const QRegularExpressionMatch match = bytesRegex.match(trimmed);

            if (match.hasMatch()) {

                const double value =
                    match.captured(1)
                        .toDouble();

                QString unit =
                    match.captured(2);

                if (unit.isEmpty()) {
                    unit =
                        QStringLiteral("B");
                }

                processedBytes =
                    convertToBytes(
                        value,
                        unit);

                continue;
            }
        }


        /*
         * ----------------------------------------------------
         * RATE
         * ----------------------------------------------------
         */

        {
            const QRegularExpressionMatch match = rateRegex.match(trimmed);

            if (match.hasMatch()) {

                const double value =
                    match.captured(1)
                        .toDouble();

                QString unit =
                    match.captured(2);

                if (unit.isEmpty()) {
                    unit = QStringLiteral("B");
                }

                speed = convertToBytes(value, unit);

                continue;
            }
        }


        /*
         * ----------------------------------------------------
         * ERROR SUMMARY
         * ----------------------------------------------------
         */

        if (trimmed.startsWith(QStringLiteral("Error summary:"), Qt::CaseInsensitive)) {

            if (trimmed.contains(QStringLiteral("no errors found"), Qt::CaseInsensitive)) {

                errorCount = 0;
                continue;
            }


            /*
             * Può contenere ad esempio:
             *
             * csum=72
             * read=2
             * verify=1
             * ...
             */

            int totalErrors = 0;

            QRegularExpressionMatchIterator it =
                errorValueRegex
                    .globalMatch(trimmed);

            while (it.hasNext()) {

                const QRegularExpressionMatch match =
                    it.next();

                totalErrors +=
                    match.captured(1)
                        .toInt();
            }

            errorCount =
                totalErrors;
        }
    }


    /*
     * Se abbiamo ottenuto uno Status siamo riusciti
     * a interpretare il comando anche se l'exit code
     * segnala, ad esempio, errori dello scrub.
     */

    if (!status.isEmpty())
    {
        /*
     * A scrub concluso btrfs-progs può non stampare
     * più "Bytes scrubbed", ma soltanto "Total to scrub".
     *
     * In quel caso l'intero totale è stato processato.
     */
        if (status == QStringLiteral("finished") && processedBytes == 0 && totalToScrubBytes > 0)
        {
            processedBytes = totalToScrubBytes;
        }

        return;
    }


    /*
     * Nessuna informazione utile.
     */
    qWarning().noquote()
        << "Unable to determine Btrfs scrub status."
        << "exit:"
        << process.exitCode()
        << "output:"
        << output;
}

void BtrfsMonitor::readBalanceStatus(const QString &mountPoint, QString &status) const
{
    status.clear();

    if (mountPoint.trimmed().isEmpty()) {
        qWarning()
        << "Cannot read balance status:"
        << "empty mount point";
        return;
    }

    const QString btrfsExecutable = QStandardPaths::findExecutable(QStringLiteral("btrfs"));

    if (btrfsExecutable.isEmpty()) {
        qWarning()
        << "Cannot read balance status:"
        << "btrfs executable not found";
        return;
    }

    QProcess process;

    /*
     * Output stabile in inglese per il parser.
     */
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    environment.insert(
        QStringLiteral("LC_ALL"),
        QStringLiteral("C"));

    process.setProcessEnvironment(environment);

    process.start(
        btrfsExecutable,
        {
            QStringLiteral("balance"),
            QStringLiteral("status"),
            mountPoint
        });

    if (!process.waitForStarted(2000)) {
        qWarning().noquote()
        << "Cannot start btrfs balance status:"
        << process.errorString();
        return;
    }

    if (!process.waitForFinished(5000)) {
        qWarning().noquote()
        << "Timeout reading balance status for"
        << mountPoint;

        process.kill();
        process.waitForFinished();

        return;
    }

    const QString stdoutText =
        QString::fromUtf8(
            process.readAllStandardOutput());

    const QString stderrText =
        QString::fromUtf8(
            process.readAllStandardError());

    const QString output =
        (stdoutText +
         QStringLiteral("\n") +
         stderrText)
            .trimmed();

    const QString normalized =
        output.toLower();

    qDebug().noquote() << "🔄 Balance status:" << mountPoint << "exit:" << process.exitCode() << "\n" << output;

    /*
     * ATTENZIONE:
     *
     * btrfs balance status può restituire 1
     * mentre il balance è in esecuzione.
     *
     * Quindi analizziamo l'output PRIMA
     * dell'exit code.
     */

    if (normalized.contains(
            QStringLiteral("is running")) ||
        normalized.contains(
            QStringLiteral("balance running"))) {

        status = QStringLiteral("running");

        return;
    }

    if (normalized.contains(
            QStringLiteral("is paused")) ||
        normalized.contains(
            QStringLiteral("balance paused"))) {

        status = QStringLiteral("paused");

        return;
    }

    /*
     * Nessuna operazione balance attualmente aperta.
     */
    if (normalized.contains(
            QStringLiteral("no balance found"))) {

        status = QStringLiteral("not running");

        return;
    }

    /*
     * Gestione difensiva di eventuali varianti.
     */
    if (normalized.contains(
            QStringLiteral("cancelled")) ||
        normalized.contains(
            QStringLiteral("canceled"))) {

        status = QStringLiteral("cancelled");

        return;
    }

    /*
     * Se non abbiamo riconosciuto lo stato,
     * l'exit code ci aiuta a diagnosticare.
     */
    qWarning().noquote()
        << "Unable to determine Btrfs balance status."
        << "exit:"
        << process.exitCode()
        << "output:"
        << output;
}

// ============================================================
// PROCESS SCRUB STATE
// ============================================================

void BtrfsMonitor::processScrubState(const QString &fsUuid, const QString &fsPath)
{
    Q_UNUSED(fsPath);


    QString mountPoint;


    if (m_filesystems.contains(fsUuid)) {
        mountPoint =
            m_filesystems.value(fsUuid)
                .mountPoint;
    }


    if (mountPoint.isEmpty()) {
        mountPoint =
            findMountPointForUUID(fsUuid);
    }


    if (mountPoint.isEmpty()) {
        qWarning()
        << "Cannot determine mount point for scrub:"
        << fsUuid.left(8);

        return;
    }


    QString status;

    quint64 processedBytes = 0;
    quint64 speed = 0;
    int errorCount = 0;


    readScrubStatus(
        mountPoint,
        status,
        processedBytes,
        speed,
        errorCount);


    OperationState &state =
        m_scrubStates[fsUuid];


    QString oldStatus =
        state.status
            .trimmed()
            .toLower();


    /*
     * Recovery dello stato dopo restart dell'agent.
     */
    if (oldStatus.isEmpty()) {

        QString dbStatus;
        QDateTime dbStartTime;


        if (getLastOperationState(
                fsUuid,
                QStringLiteral("scrub"),
                dbStatus,
                dbStartTime)) {

            oldStatus =
                dbStatus
                    .trimmed()
                    .toLower();

            state.status =
                oldStatus;

            state.startTime =
                dbStartTime;
        }
    }


    /*
     * Errore di lettura:
     *
     * non inventiamo uno stato.
     */
    if (status.isEmpty()) {
        return;
    }


    status =
        status.trimmed().toLower();


    if (m_filesystems.contains(fsUuid)) {
        m_filesystems[fsUuid]
            .scrubStatus = status;
    }


    const bool active =
        status == QStringLiteral("running") ||
        status == QStringLiteral("started");


    /*
     * --------------------------------------------------------
     * CAMBIO STATO
     * --------------------------------------------------------
     */

    if (oldStatus != status) {

        qDebug()
        << "📝 Scrub status changed:"
        << oldStatus
        << "->"
        << status;


        if (!updateOperationState(
                fsUuid,
                QStringLiteral("scrub"),
                status,
                processedBytes,
                speed,
                errorCount)) {

            qWarning()
            << "Cannot persist scrub state:"
            << status;
        }
    }


    /*
     * --------------------------------------------------------
     * STESSO STATO RUNNING, MA PROGRESSO CAMBIATO
     * --------------------------------------------------------
     */

    else if (active &&
             (state.processedBytes != processedBytes ||
              state.speedBytesPerSec != speed ||
              state.errorCount != errorCount)) {

        if (!updateOperationState(
                fsUuid,
                QStringLiteral("scrub"),
                status,
                processedBytes,
                speed,
                errorCount)) {

            qWarning()
            << "Cannot update scrub progress";
        }
    }


    /*
     * --------------------------------------------------------
     * AGGIORNA RAM
     * --------------------------------------------------------
     */

    const bool wasActive =
        state.isRunning;


    state.status =
        status;

    state.processedBytes =
        processedBytes;

    state.speedBytesPerSec =
        speed;

    state.errorCount =
        errorCount;

    state.isRunning =
        active;


    if (active &&
        !wasActive &&
        !state.startTime.isValid()) {

        state.startTime =
            QDateTime::currentDateTime();
    }
}

// ============================================================
// PROCESS BALANCE STATE
// ============================================================

void BtrfsMonitor::processBalanceState(
    const QString &fsUuid,
    const QString &fsPath)
{
    Q_UNUSED(fsPath);

    /*
     * Prima prova lo stato già noto del filesystem.
     */
    QString mountPoint;

    if (m_filesystems.contains(fsUuid)) {
        mountPoint =
            m_filesystems.value(fsUuid)
                .mountPoint;
    }

    /*
     * Fallback.
     */
    if (mountPoint.isEmpty()) {
        mountPoint =
            findMountPointForUUID(fsUuid);
    }

    if (mountPoint.isEmpty()) {
        qWarning()
        << "Cannot determine mount point for balance:"
        << fsUuid.left(8);
        return;
    }


    QString status;

    readBalanceStatus(
        mountPoint,
        status);


    /*
     * Stato in memoria.
     */
    OperationState &state =
        m_balanceStates[fsUuid];

    QString oldStatus =
        state.status.trimmed().toLower();


    /*
     * Se l'agent è appena ripartito, recuperiamo
     * almeno lo stato precedente dal DB.
     */
    if (oldStatus.isEmpty()) {
        QString dbStatus;
        QDateTime dbStartTime;

        if (getLastOperationState(
                fsUuid,
                QStringLiteral("balance"),
                dbStatus,
                dbStartTime)) {

            oldStatus =
                dbStatus.trimmed().toLower();

            state.status =
                oldStatus;

            state.startTime =
                dbStartTime;
        }
    }


    /*
     * Se non siamo riusciti a leggere lo stato,
     * non chiudiamo automaticamente qualcosa
     * solo per un errore del comando.
     */
    if (status.isEmpty()) {
        return;
    }


    status =
        status.trimmed().toLower();


    if (m_filesystems.contains(fsUuid)) {
        m_filesystems[fsUuid]
            .balanceStatus = status;
    }


    const bool active =
        status == QStringLiteral("running") ||
        status == QStringLiteral("started") ||
        status == QStringLiteral("paused");


    if (oldStatus != status) {

        qDebug()
        << "📝 Balance status changed:"
        << oldStatus
        << "->"
        << status;

        if (!updateOperationState(
                fsUuid,
                QStringLiteral("balance"),
                status,
                0,
                0,
                0)) {

            qWarning()
            << "Cannot persist balance state:"
            << status;
        }
    }


    /*
     * Manteniamo lo stato RAM coerente.
     */
    const bool wasActive =
        state.isRunning;

    state.status =
        status;


    /*
     * Per Balance "paused" appartiene ancora
     * alla stessa operazione.
     */
    state.isRunning =
        active;


    if (active &&
        !wasActive &&
        !state.startTime.isValid()) {

        state.startTime =
            QDateTime::currentDateTime();
    }


    /*
     * Operazione terminata.
     */
    if (!active) {
        state.isRunning = false;
    }
}

// ============================================================
// METODI PRIVATI ESISTENTI (INVARIATI)
// ============================================================

bool BtrfsMonitor::readDeviceStats(const QString &filePath, DeviceStats &stats) const
{
    QFile file(filePath);

    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning() << "Unable to open file:" << filePath
                   << "- Error:" << file.errorString();
        return false;
    }

    const QByteArray data = file.readAll();

    if (data.isEmpty()) {
        qWarning() << "File is empty or unreadable:" << filePath;
        return false;
    }

    if (data.size() > 1024 * 1024) {
        qWarning() << "Abnormally large file (" << data.size()
        << "bytes):" << filePath;
        return false;
    }

    const QString content = QString::fromUtf8(data);

    if (content.isEmpty() && !data.isEmpty()) {
        qWarning() << "Invalid UTF-8 content in file:" << filePath;
        return false;
    }

    const QStringList lines = content.split('\n', Qt::SkipEmptyParts);

    if (lines.size() > 100) {
        qWarning() << "Too many lines (" << lines.size()
        << "):" << filePath;
        return false;
    }

    // Static regex to avoid Clazy warning
    static const QRegularExpression whitespaceRegex(QStringLiteral("\\s+"));

    bool hasValidData = false;
    int parsedLines = 0;

    for (const QString &line : lines) {
        const QString trimmedLine = line.trimmed();

        if (trimmedLine.isEmpty())
            continue;

        const QStringList parts = trimmedLine.split(whitespaceRegex, Qt::SkipEmptyParts);

        if (parts.size() != 2) {
            qDebug() << "Unexpected line format:" << trimmedLine;
            continue;
        }

        bool ok = false;
        const quint64 value = parts.at(1).toULongLong(&ok);

        if (!ok) {
            qDebug() << "Non-numeric value:" << parts.at(1);
            continue;
        }

        const QString type = parts.at(0);
        hasValidData = true;
        parsedLines++;

        if (type == QStringLiteral("read_errs"))
            stats.readErrors = value;
        else if (type == QStringLiteral("write_errs"))
            stats.writeErrors = value;
        else if (type == QStringLiteral("flush_errs"))
            stats.flushErrors = value;
        else if (type == QStringLiteral("corruption_errs"))
            stats.corruptionErrors = value;
        else if (type == QStringLiteral("generation_errs"))
            stats.generationErrors = value;
        else {
            qDebug() << "Unknown statistics type:" << type;
        }
    }

    if (!hasValidData) {
        qWarning() << "No valid statistics found in file:" << filePath;
        return false;
    }

    qDebug() << "Read" << parsedLines << "statistics from file" << filePath;
    return true;
}

BtrfsMonitor::HealthState
BtrfsMonitor::evaluateFilesystem(const FilesystemState &filesystem, const QSet<QString> &currentDevices, QString &reason) const
{
    /*
     * ------------------------------------------
     * MISSING DEVICES
     * ------------------------------------------
     */
    for (auto it = filesystem.devices.constBegin();
         it != filesystem.devices.constEnd();
         ++it)
    {
        if (!currentDevices.contains(it.key()))
        {
            reason =
                QStringLiteral(
                    "Btrfs device %1 is no longer present."
                    ).arg(it.key());

            return HealthState::Warning;
        }
    }

    /*
     * ------------------------------------------
     * DEVICE ERRORS
     * ------------------------------------------
     */
    for (auto it = filesystem.devices.constBegin();
         it != filesystem.devices.constEnd();
         ++it)
    {
        const DeviceStats &stats = it.value().stats;

        /*
         * Corruption and generation errors
         * are considered critical.
         */
        if (stats.corruptionErrors > 0)
        {
            reason =
                QStringLiteral(
                    "Detected %1 corruption error(s) "
                    "on device %2."
                    )
                    .arg(stats.corruptionErrors)
                    .arg(it.key());

            return HealthState::Error;
        }

        if (stats.generationErrors > 0)
        {
            reason =
                QStringLiteral(
                    "Detected %1 generation error(s) "
                    "on device %2."
                    )
                    .arg(stats.generationErrors)
                    .arg(it.key());

            return HealthState::Error;
        }

        /*
         * Normal I/O errors:
         * WARNING.
         */
        if (stats.readErrors > 0)
        {
            reason =
                QStringLiteral(
                    "Device %1 has "
                    "%2 read error(s)."
                    )
                    .arg(it.key())
                    .arg(stats.readErrors);

            return HealthState::Warning;
        }

        if (stats.writeErrors > 0)
        {
            reason =
                QStringLiteral(
                    "Device %1 has "
                    "%2 write error(s)."
                    )
                    .arg(it.key())
                    .arg(stats.writeErrors);

            return HealthState::Warning;
        }

        if (stats.flushErrors > 0)
        {
            reason =
                QStringLiteral(
                    "Device %1 has "
                    "%2 flush error(s)."
                    )
                    .arg(it.key())
                    .arg(stats.flushErrors);

            return HealthState::Warning;
        }
    }

    /*
     * ------------------------------------------
     * NO PROBLEMS
     * ------------------------------------------
     */
    reason =
        QStringLiteral(
            "Btrfs filesystem is operational."
            );

    return HealthState::Ok;
}

void BtrfsMonitor::sendSystemNotification(const QString &fsUuid, HealthState state, const QString &reason)
{
    const QString shortUuid = fsUuid.left(8);
    QString title;
    QString message;
    QString emailSeverity;

    SystemNotifier::NotificationPriority priority = SystemNotifier::Normal;
    int timeout = 5000;

    switch (state) {
    case HealthState::Error:
        title = "❌ BTRFS CRITICAL";
        message = QString("Filesystem %1: %2")
                      .arg(shortUuid, getShortReason(reason));
        emailSeverity = QStringLiteral("critical");
        priority = SystemNotifier::Critical;
        timeout = 0;  // Permanente

        break;

    case HealthState::Warning:
        title = "⚠️ BTRFS Warning";
        message = QString("Filesystem %1: %2")
                      .arg(shortUuid, getShortReason(reason));
        emailSeverity = QStringLiteral("warning");
        priority = SystemNotifier::Normal;
        timeout = 8000;

        break;

    case HealthState::Ok:
        title = "✅ BTRFS OK";
        message = QString("Filesystem %1 is healthy").arg(shortUuid);
        emailSeverity = QStringLiteral("info");
        priority = SystemNotifier::Low;
        timeout = 3000;

        break;

    default:
        return;
    }

    // ========================================================
    // DESKTOP
    // ========================================================

    if (m_notifier)
    {
    m_notifier->sendNotification(title, message, priority, timeout);
    }

    // ========================================================
    // EMAIL
    // ========================================================
    if (m_emailNotifier)
    {
        m_emailNotifier->sendNotification(title, message, emailSeverity);
    }
}

void BtrfsMonitor::updateFilesystemState(const QString &fsUuid, HealthState newState, const QString &reason)
{
    FilesystemState &filesystem = m_filesystems[fsUuid];

    static QHash<QString, int> errorCount;

    // ========================================================
    // FIRST DETECTION
    // ========================================================

    if (!filesystem.initialized)
    {
        filesystem.health = newState;
        filesystem.initialized = true;

        if (newState == HealthState::Error)
        {
            errorCount[fsUuid] = 0;
        }
        else
        {
            errorCount.remove(fsUuid);
        }

        qInfo().noquote()
            << "Btrfs:"
            << fsUuid
            << "initial status:"
            << healthStateToString(newState)
            << "-"
            << reason;

        // Notify only if WARNING or ERROR at startup
        if (newState == HealthState::Warning ||
            newState == HealthState::Error)
        {
            sendSystemNotification(
                fsUuid,
                newState,
                QStringLiteral("(At startup) ") + reason);
        }

        return;
    }

    // ========================================================
    // STATUS CHANGED
    // ========================================================

    if (filesystem.health != newState)
    {
        const HealthState oldState =
            filesystem.health;

        filesystem.health = newState;

        /*
         * Nuovo episodio Error:
         * riparte da zero.
         *
         * Uscita da Error:
         * eliminiamo il vecchio contatore.
         */
        if (newState == HealthState::Error)
        {
            errorCount[fsUuid] = 0;
        }
        else
        {
            errorCount.remove(fsUuid);
        }

        // Emit signal
        emit healthChanged(
            fsUuid,
            newState,
            reason);

        // Desktop + email
        sendSystemNotification(
            fsUuid,
            newState,
            reason);

        // Log
        qInfo().noquote()
            << "Btrfs:"
            << fsUuid
            << "status changed:"
            << healthStateToString(oldState)
            << "->"
            << healthStateToString(newState)
            << "-"
            << reason;

        return;
    }

    // ========================================================
    // STATUS PERSISTS
    // ========================================================

    if (newState == HealthState::Error)
    {
        const int count =
            ++errorCount[fsUuid];

        /*
         * Con scansione ogni 30 secondi:
         * 5 cicli = circa 2.5 minuti.
         */
        if (count % 5 == 0)
        {
            sendSystemNotification(
                fsUuid,
                newState,
                QStringLiteral("⚠️ PERSISTENT ERROR: ") +
                    reason);
        }
    }
}

// ============================================================
// METODI DI LETTURA SPAZIO (INVARIATI)
// ============================================================

quint64 BtrfsMonitor::convertToBytes(double value, const QString &unit) const
{
    if (unit == QStringLiteral("B")) return static_cast<quint64>(value);
    if (unit == QStringLiteral("KiB")) return static_cast<quint64>(value * 1024);
    if (unit == QStringLiteral("MiB")) return static_cast<quint64>(value * 1024 * 1024);
    if (unit == QStringLiteral("GiB")) return static_cast<quint64>(value * 1024LL * 1024 * 1024);
    if (unit == QStringLiteral("TiB")) return static_cast<quint64>(value * 1024LL * 1024 * 1024 * 1024);
    return static_cast<quint64>(value);
}

QString BtrfsMonitor::formatBytes(quint64 bytes) const
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KiB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    if (bytes < 1024 * 1024 * 1024) return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    if (bytes < 1024ULL * 1024 * 1024 * 1024) return QStringLiteral("%1 GiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 TiB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
}

bool BtrfsMonitor::readSpaceInfo(const QString &fsUuid, SpaceInfo &space) const
{
    // ============================================================
    // METODO 1: Prova a leggere da sysfs
    // ============================================================
    const QString basePath = QStringLiteral("/sys/fs/btrfs/") + fsUuid;
    QFile infoFile(basePath + QStringLiteral("/info"));

    if (infoFile.exists() && infoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&infoFile);
        QString line;

        bool metadataFound = false;
        bool dataFound = false;
        bool systemFound = false;

        static const QRegularExpression metadataRegex(
            QStringLiteral("Metadata\\s+total\\s+([\\d.]+)\\s*(\\w+)\\s+used\\s+([\\d.]+)\\s*(\\w+)"));
        static const QRegularExpression dataRegex(
            QStringLiteral("Data\\s+total\\s+([\\d.]+)\\s*(\\w+)\\s+used\\s+([\\d.]+)\\s*(\\w+)"));
        static const QRegularExpression systemRegex(
            QStringLiteral("System\\s+total\\s+([\\d.]+)\\s*(\\w+)\\s+used\\s+([\\d.]+)\\s*(\\w+)"));

        while (in.readLineInto(&line)) {
            const QString trimmedLine = line.trimmed();
            QRegularExpressionMatch match;

            if ((match = metadataRegex.match(trimmedLine)).hasMatch()) {
                space.metadataTotal = convertToBytes(match.captured(1).toDouble(), match.captured(2));
                space.metadataUsed = convertToBytes(match.captured(3).toDouble(), match.captured(4));
                metadataFound = true;
            }
            else if ((match = dataRegex.match(trimmedLine)).hasMatch()) {
                space.dataTotal = convertToBytes(match.captured(1).toDouble(), match.captured(2));
                space.dataUsed = convertToBytes(match.captured(3).toDouble(), match.captured(4));
                dataFound = true;
            }
            else if ((match = systemRegex.match(trimmedLine)).hasMatch()) {
                space.systemTotal = convertToBytes(match.captured(1).toDouble(), match.captured(2));
                space.systemUsed = convertToBytes(match.captured(3).toDouble(), match.captured(4));
                systemFound = true;
            }
        }
        infoFile.close();

        if (metadataFound || dataFound || systemFound) {
            space.total = space.metadataTotal + space.dataTotal + space.systemTotal;
            space.used = space.metadataUsed + space.dataUsed + space.systemUsed;

            // Leggi unallocated
            QFile allocFile(basePath + QStringLiteral("/allocation"));
            if (allocFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream allocIn(&allocFile);
                while (allocIn.readLineInto(&line)) {
                    const QString trimmedLine = line.trimmed();
                    if (trimmedLine.contains(QStringLiteral("unallocated:"))) {
                        const QStringList parts = trimmedLine.split(QStringLiteral(":"), Qt::SkipEmptyParts);
                        if (parts.size() == 2) {
                            const QStringList valueParts = parts.at(1).trimmed().split(QStringLiteral(" "), Qt::SkipEmptyParts);
                            if (valueParts.size() >= 2) {
                                space.unallocated = convertToBytes(valueParts.at(0).toDouble(), valueParts.at(1));
                            }
                        }
                        break;
                    }
                }
                allocFile.close();
            }

            // Leggi profili RAID
            readProfileInfo(fsUuid, space.dataProfile, space.metadataProfile);

            qDebug().noquote() << "  ✅ Space info read from sysfs";
            return true;
        }
        infoFile.close();
    }

    // ============================================================
    // METODO 2: Usa il comando btrfs
    // ============================================================
    qDebug().noquote() << "  📌 /info not found or unreadable, using btrfs command";

    QString mountPoint = findMountPointForUUID(fsUuid);
    if (mountPoint.isEmpty()) {
        qWarning() << "  ❌ Cannot find mount point for UUID:" << fsUuid;
        return false;
    }

    return readSpaceInfoFromBtrfsCommand(mountPoint, space);
}

// ============================================================
// METODI PER LEGGERE SPAZIO DAL COMANDO BTRFS
// ============================================================

QString BtrfsMonitor::findMountPointForUUID(const QString &fsUuid) const
{
    QString searchUuid = fsUuid;
    if (searchUuid.startsWith("UUID=")) {
        searchUuid = searchUuid.mid(5);
    }

    // ============================================================
    // METODO 1: Usa findmnt con filtro UUID (il più affidabile)
    // ============================================================
    QProcess findmntProcess;
    findmntProcess.start("findmnt", QStringList() << "-n" << "-o" << "TARGET,SOURCE" << "-t" << "btrfs");
    if (findmntProcess.waitForFinished(3000)) {
        QString output = findmntProcess.readAllStandardOutput();
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);

        for (const QString &line : std::as_const(lines)) {
            const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                const QString mountPoint = parts[0].trimmed();
                const QString device = parts[1].trimmed();

                // Verifica se il device contiene l'UUID
                // I device btrfs possono essere /dev/sdX o /dev/mapper/...
                if (device.contains(searchUuid) || device.contains(fsUuid)) {
                    qDebug().noquote() << "  • Found mount point via findmnt:" << mountPoint;
                    return mountPoint;
                }
            }
        }
    }

    // ============================================================
    // METODO 2: Usa findmnt per avere i mount point e verifica con lsblk
    // ============================================================
    QProcess findmntSimple;
    findmntSimple.start("findmnt", QStringList() << "-n" << "-o" << "TARGET" << "-t" << "btrfs");
    if (findmntSimple.waitForFinished(3000)) {
        QString output = findmntSimple.readAllStandardOutput();
        QStringList mountPoints = output.split('\n', Qt::SkipEmptyParts);

        for (const QString &mountPoint : std::as_const(mountPoints)) {
            QString cleanMount = mountPoint.trimmed();

            // Usa lsblk per ottenere l'UUID del dispositivo
            QProcess lsblkProcess;
            lsblkProcess.start("lsblk", QStringList() << "-n" << "-o" << "UUID,MOUNTPOINT" << "-t" << "btrfs");
            if (lsblkProcess.waitForFinished(3000)) {
                QString lsblkOutput = lsblkProcess.readAllStandardOutput();
                QStringList lsblkLines = lsblkOutput.split('\n', Qt::SkipEmptyParts);

                for (const QString &lsblkLine : std::as_const(lsblkLines)) {
                    const QStringList lsblkParts = lsblkLine.split(' ', Qt::SkipEmptyParts);
                    if (lsblkParts.size() >= 2) {
                        const QString uuid = lsblkParts[0].trimmed();
                        const QString mnt = lsblkParts[1].trimmed();

                        if (mnt == cleanMount && (uuid == searchUuid || uuid == fsUuid)) {
                            qDebug().noquote() << "  • Found mount point via lsblk:" << cleanMount;
                            return cleanMount;
                        }
                    }
                }
            }
        }
    }

    // ============================================================
    // METODO 3: Fallback: cerca in /proc/mounts e verifica con stat
    // ============================================================
    QFile mountsFile("/proc/mounts");
    if (!mountsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Cannot open /proc/mounts";
        return QString();
    }

    QTextStream in(&mountsFile);
    QString line;

    while (in.readLineInto(&line)) {
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 3) continue;

        if (parts[2] == "btrfs") {
            const QString mountPoint = parts[1];
            const QString device = parts[0];

            // Usa lsblk per ottenere l'UUID
            QProcess lsblkProcess;
            lsblkProcess.start("lsblk", QStringList() << "-n" << "-o" << "UUID" << device);
            if (lsblkProcess.waitForFinished(2000)) {
                QString uuid = lsblkProcess.readAllStandardOutput().trimmed();
                if (uuid == searchUuid || uuid == fsUuid) {
                    mountsFile.close();
                    qDebug().noquote() << "  • Found mount point via /proc/mounts + lsblk:" << mountPoint;
                    return mountPoint;
                }
            }
        }
    }
    mountsFile.close();

    qWarning().noquote() << "  ❌ Could not find mount point for UUID:" << fsUuid;
    return QString();
}

bool BtrfsMonitor::parseBtrfsUsageOutput(const QString &output, SpaceInfo &space) const
{
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    bool foundUnallocated = false;

    // Regex per i valori globali
    static const QRegularExpression unallocatedRegex(
        QStringLiteral("Device unallocated:\\s+([\\d.]+)\\s*(\\w*)"));
    static const QRegularExpression usedRegex(
        QStringLiteral("Used:\\s+([\\d.]+)\\s*(\\w*)"));
    static const QRegularExpression allocatedRegex(
        QStringLiteral("Device allocated:\\s+([\\d.]+)\\s*(\\w*)"));

    // Regex per Data, Metadata, System chunks
    // Ordine: profilo, size, unità_size, used, unità_used
    static const QRegularExpression dataChunkRegex(
        QStringLiteral("Data,(\\w+):\\s+Size:\\s*([\\d.]+)\\s*(\\w*),\\s+Used:\\s*([\\d.]+)\\s*(\\w*)"));
    static const QRegularExpression metadataChunkRegex(
        QStringLiteral("Metadata,(\\w+):\\s+Size:\\s*([\\d.]+)\\s*(\\w*),\\s+Used:\\s*([\\d.]+)\\s*(\\w*)"));
    static const QRegularExpression systemChunkRegex(
        QStringLiteral("System,(\\w+):\\s+Size:\\s*([\\d.]+)\\s*(\\w*),\\s+Used:\\s*([\\d.]+)\\s*(\\w*)"));

    for (const QString &line : std::as_const(lines)) {
        const QString trimmed = line.trimmed();
        QRegularExpressionMatch match;

        if ((match = unallocatedRegex.match(trimmed)).hasMatch()) {
            space.unallocated = convertToBytes(match.captured(1).toDouble(), match.captured(2));
            foundUnallocated = true;
            qDebug().noquote() << "  • Unallocated:" << formatBytes(space.unallocated);
        }
        else if ((match = usedRegex.match(trimmed)).hasMatch()) {
            space.used = convertToBytes(match.captured(1).toDouble(), match.captured(2));
            qDebug().noquote() << "  • Used:" << formatBytes(space.used);
        }
        else if ((match = allocatedRegex.match(trimmed)).hasMatch()) {
            quint64 allocated = convertToBytes(match.captured(1).toDouble(), match.captured(2));
            qDebug().noquote() << "  • Device allocated:" << formatBytes(allocated);
            if (space.total == 0) {
                space.total = allocated;
            }
        }
        else if ((match = dataChunkRegex.match(trimmed)).hasMatch()) {
            // captured(1) = profilo (es. "RAID1")
            // captured(2) = size value
            // captured(3) = size unit
            // captured(4) = used value
            // captured(5) = used unit
            space.dataTotal = convertToBytes(match.captured(2).toDouble(), match.captured(3));
            space.dataUsed = convertToBytes(match.captured(4).toDouble(), match.captured(5));
            space.dataProfile = stringToProfile(match.captured(1).toLower());
            qDebug().noquote() << "  • Data:" << match.captured(1) << formatBytes(space.dataTotal) << "used:" << formatBytes(space.dataUsed);
        }
        else if ((match = metadataChunkRegex.match(trimmed)).hasMatch()) {
            space.metadataTotal = convertToBytes(match.captured(2).toDouble(), match.captured(3));
            space.metadataUsed = convertToBytes(match.captured(4).toDouble(), match.captured(5));
            space.metadataProfile = stringToProfile(match.captured(1).toLower());
            qDebug().noquote() << "  • Metadata:" << match.captured(1) << formatBytes(space.metadataTotal) << "used:" << formatBytes(space.metadataUsed);

        }
        else if ((match = systemChunkRegex.match(trimmed)).hasMatch()) {
            space.systemTotal = convertToBytes(match.captured(2).toDouble(), match.captured(3));
            space.systemUsed = convertToBytes(match.captured(4).toDouble(), match.captured(5));
            qDebug().noquote() << "  • System:" << formatBytes(space.systemTotal) << "used:" << formatBytes(space.systemUsed);
        }
    }

    // `space.total` is the PHYSICAL device allocation reported by
    // "Device allocated".  Do not replace it with the sum of logical
    // Data/Metadata/System chunk sizes: with RAID1 those are logical
    // chunk sizes and are intentionally different from physical allocation.
    const quint64 chunkTotal = space.metadataTotal + space.dataTotal + space.systemTotal;
    if (chunkTotal > 0) {
        qDebug().noquote() << "  • Logical chunk total:" << formatBytes(chunkTotal);
    }

    qDebug().noquote() << "  • Final Physical Allocated:" << formatBytes(space.total);
    qDebug().noquote() << "  • Final Used:" << formatBytes(space.used);
    qDebug().noquote() << "  • Final Unallocated:" << formatBytes(space.unallocated);

    return (space.total > 0 && foundUnallocated);
}

bool BtrfsMonitor::readSpaceInfoFromBtrfsCommand(const QString &mountPoint, SpaceInfo &space) const
{
    if (mountPoint.isEmpty()) {
        return false;
    }

    qDebug().noquote() << "  • Running: btrfs filesystem usage -b" << mountPoint;

    QProcess process;
    process.start("btrfs", QStringList() << "filesystem" << "usage" << "-b" << mountPoint);

    if (!process.waitForFinished(5000)) {
        qWarning() << "  ❌ Timeout executing btrfs command for" << mountPoint;
        return false;
    }

    if (process.exitCode() != 0) {
        QString error = process.readAllStandardError();
        qWarning() << "  ❌ btrfs command failed:" << error.trimmed();
        return false;
    }

    QString output = process.readAllStandardOutput();
    qDebug().noquote() << "  • btrfs command completed successfully";

    // Parsing dell'output
    return parseBtrfsUsageOutput(output, space);
}

bool BtrfsMonitor::readDeviceSpaceInfoFromBtrfsCommand(const QString &mountPoint, QHash<QString, DeviceStats> &deviceStats) const
{
    deviceStats.clear();

    if (mountPoint.isEmpty())
        return false;

    qDebug().noquote() << "  • Running: btrfs device usage -b" << mountPoint;

    QProcess process;

    process.start(QStringLiteral("btrfs"),
        QStringList() << QStringLiteral("device") << QStringLiteral("usage") << QStringLiteral("-b") << mountPoint
        );

    if (!process.waitForStarted(2000)) {
        qWarning().noquote() << "  ❌ Cannot start btrfs device usage:" << process.errorString();
        return false;
    }

    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);

        qWarning().noquote() << "  ❌ Timeout executing btrfs device usage for" << mountPoint;
        return false;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());

    const QString errorOutput = QString::fromLocal8Bit(process.readAllStandardError());

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {

        qWarning().noquote() << "  ❌ btrfs device usage failed:" << errorOutput.trimmed();
        return false;
    }

    if (output.trimmed().isEmpty()) {
        qWarning().noquote() << "  ❌ btrfs device usage returned no output";
        return false;
    }

    /*
     * Example root output:
     *
     * /dev/sda, ID: 1
     *    Device size:       53687091200
     *    Device slack:                0
     *    Data,RAID1:        ...
     *    Metadata,RAID1:    ...
     *    System,RAID1:      ...
     *    Unallocated:       ...
     */

    static const QRegularExpression idRegex(
        QStringLiteral( R"(^(?:(/dev/[^\s,]+|missing),\s*)?ID:\s*(\d+)\s*$)"));

    static const QRegularExpression sizeRegex(
        QStringLiteral(R"(^\s*Device size:\s*(\d+)\s*$)"));

    static const QRegularExpression slackRegex(
        QStringLiteral(R"(^\s*Device slack:\s*(\d+)\s*$)"));

    static const QRegularExpression unallocatedRegex(
        QStringLiteral(R"(^\s*Unallocated:\s*(\d+)\s*$)"));

    /*
     * Cattura qualsiasi profilo:
     *
     * Data,RAID1:
     * Data,single:
     * Data,RAID10/4:
     * Metadata,RAID1:
     * System,RAID1:
     *
     * Possono esserci più righe dello stesso tipo con
     * profili diversi: vanno tutte sommate.
     */
    static const QRegularExpression allocationRegex(QStringLiteral(R"(^\s*(Data|Metadata|System),[^:]+:\s*(\d+)\s*$)"));

    QHash<QString, quint64> totalById;
    QHash<QString, quint64> slackById;
    QHash<QString, quint64> unallocatedById;
    QHash<QString, quint64> allocatedById;
    QHash<QString, QString> pathById;

    QSet<QString> hasUnallocated;
    QSet<QString> hasDetailedAllocation;

    QString currentId;

    const QStringList lines = output.split('\n');

    for (const QString &rawLine : lines) {

        const QString line = rawLine.trimmed();

        if (line.isEmpty())
            continue;

        QRegularExpressionMatch match;

        /*
         * Nuovo device.
         */
        match = idRegex.match(line);

        if (match.hasMatch())
        {
            QString devicePath = match.captured(1).trimmed();

            if (devicePath == QStringLiteral("missing"))
            {
                devicePath.clear();
            }

            currentId = match.captured(2).trimmed();

            totalById[currentId] = 0;
            slackById[currentId] = 0;
            unallocatedById[currentId] = 0;
            allocatedById[currentId] = 0;

            pathById[currentId] = devicePath;

            qDebug().noquote() << "  • Found device:" << currentId << devicePath;

            continue;
        }

        if (currentId.isEmpty())
            continue;

        /*
         * Device size.
         */
        match = sizeRegex.match(line);

        if (match.hasMatch()) {

            totalById[currentId] = match.captured(1).toULongLong();

            continue;
        }

        /*
         * Device slack.
         *
         * Informativo: NON viene sottratto da Device size
         * nel calcolo dell'allocazione Btrfs.
         */
        match = slackRegex.match(line);

        if (match.hasMatch()) {

            slackById[currentId] = match.captured(1).toULongLong();

            continue;
        }

        /*
         * Valore esatto di unallocated per questo device.
         */
        match = unallocatedRegex.match(line);

        if (match.hasMatch()) {

            unallocatedById[currentId] = match.captured(1).toULongLong();

            hasUnallocated.insert(currentId);

            continue;
        }

        /*
         * Allocazioni fisiche Btrfs sul device.
         *
         * Sommiamo Data + Metadata + System,
         * indipendentemente dal profilo.
         */
        match = allocationRegex.match(line);

        if (match.hasMatch()) {

            const quint64 bytes = match.captured(2).toULongLong();

            allocatedById[currentId] += bytes;

            hasDetailedAllocation.insert(currentId);

            continue;
        }
    }

    /*
     * Costruzione risultato.
     */
    for (auto it = totalById.constBegin();
         it != totalById.constEnd();
         ++it) {

        const QString &id = it.key();

        const quint64 total = it.value();

        if (total == 0)
        {
            DeviceStats stats;

            stats.devicePath = pathById.value(id);

            stats.deviceTotal = 0;

            stats.deviceAllocated = allocatedById.value(id);

            stats.deviceUnallocated = unallocatedById.value(id);

            deviceStats.insert(id, stats);

            qWarning().noquote()
                << "  ⚠️ Device"
                << id
                << stats.devicePath
                << "has zero size;"
                << "Allocated:"
                << formatBytes(stats.deviceAllocated)
                << "Unallocated:"
                << formatBytes(stats.deviceUnallocated)
                << "preserving record for missing-device detection";

            continue;
        }

        if (!hasUnallocated.contains(id)) {
            qWarning().noquote() << "  ⚠️ Device" << id<< "has no Unallocated value";
            continue;
        }

        const quint64 unallocated = unallocatedById.value(id);

        quint64 allocated = 0;

        /*
         * Metodo preferito:
         * somma delle righe Data/Metadata/System.
         */
        if (hasDetailedAllocation.contains(id)) {

            allocated = allocatedById.value(id);

        } else {

            /*
             * Fallback.
             *
             * Device size è la capacità disponibile
             * a Btrfs, quindi:
             *
             * allocated = size - unallocated
             */
            if (total >= unallocated)
                allocated = total - unallocated;
        }

        /*
         * Sanity check.
         *
         * La somma delle allocazioni dettagliate dovrebbe
         * corrispondere a Device size - Unallocated.
         */
        const quint64 derivedAllocated =
            total >= unallocated
                ? total - unallocated
                : 0;

        if (hasDetailedAllocation.contains(id) &&
            allocated != derivedAllocated) {

            const quint64 difference =
                allocated > derivedAllocated
                    ? allocated - derivedAllocated
                    : derivedAllocated - allocated;

            qDebug().noquote()
                << "  ⚠️ Device"
                << id
                << "allocation difference:"
                << formatBytes(difference)
                << "(rows:"
                << formatBytes(allocated)
                << "derived:"
                << formatBytes(derivedAllocated)
                << ")";
        }

        DeviceStats stats;

        stats.devicePath = pathById.value(id);

        stats.deviceTotal = total;

        /*
         * Cambiamo il nome deviceUsed con deviceAllocated
         * per non modificare la struct.
         *
         * ATTENZIONE:
         * semanticamente questo valore è
         * PHYSICAL ALLOCATED, non filesystem Used.
         */
        stats.deviceAllocated = allocated;

        stats.deviceUnallocated = unallocated;

        deviceStats.insert(id, stats);

        const double allocationPercent =
            total > 0
                ? (static_cast<double>(allocated) /
                   static_cast<double>(total)) * 100.0
                : 0.0;

        qDebug().noquote()
            << "  • DEVICE"
            << id
            << "Path:"
            << stats.devicePath
            << "Total:"
            << formatBytes(total)
            << "Allocated:"
            << formatBytes(allocated)
            << "Unallocated:"
            << formatBytes(unallocated)
            << "Slack:"
            << formatBytes(slackById.value(id))
            << "Allocation:"
            << QString::number(
                   allocationPercent,
                   'f',
                   1)
                   + "%";
    }

    if (deviceStats.isEmpty()) {
        qWarning().noquote() << "  ❌ No valid per-device allocation information";
        return false;
    }

    qDebug().noquote() << "  • Read" << deviceStats.size() << "exact per-device allocation records";

    return true;
}

bool BtrfsMonitor::readDeviceSpaceInfo(const QString &fsUuid, const QString &devId, DeviceState &devState, int deviceCount) const
{
    Q_UNUSED(deviceCount);

    const QString devPath =
        QStringLiteral("/sys/fs/btrfs/") + fsUuid +
        QStringLiteral("/devinfo/") + devId;

    const QStringList sizePaths = {
        devPath + QStringLiteral("/size"),
        devPath + QStringLiteral("/device_size"),
        devPath + QStringLiteral("/total_bytes")
    };

    for (const QString &path : sizePaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        QTextStream in(&file);
        const QString line = in.readLine().trimmed();
        bool ok = false;
        const quint64 value = line.toULongLong(&ok);
        if (ok && value > 0) {
            devState.stats.deviceTotal = value;
            break;
        }
    }

    auto readUint64File = [](const QString &path, quint64 &value) -> bool {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        QTextStream in(&file);
        bool ok = false;
        const quint64 parsed = in.readLine().trimmed().toULongLong(&ok);
        if (ok) {
            value = parsed;
            return true;
        }
        return false;
    };

    readUint64File(devPath + QStringLiteral("/used"),
                   devState.stats.deviceAllocated);
    readUint64File(devPath + QStringLiteral("/unallocated"),
                   devState.stats.deviceUnallocated);

    return devState.stats.deviceTotal > 0;
}

// ============================================================
// VALUTAZIONE REGOLE (INVARIATE)
// ============================================================

void BtrfsMonitor::evaluateSpaceRules(const QString &fsUuid, const FilesystemState &filesystem, QStringList &warnings) const
{
    Q_UNUSED(fsUuid);

    const SpaceInfo &space = filesystem.space;

    // ============================================================
    // REGOLA 1: SPAZIO FISICO NON ALLOCATO
    // ============================================================

    // Il controllo usa lo spazio fisico non allocato.
    const quint64 unallocatedBytes = space.unallocated;

    // CALCOLA LO SPAZIO TOTALE REALE DEL DISPOSITIVO
    // Spazio totale = spazio allocato + spazio non allocato
    quint64 totalDeviceSpace = space.total + space.unallocated;

    if (totalDeviceSpace > 0) {

        const double unallocatedPercent =
            (static_cast<double>(unallocatedBytes) /
             static_cast<double>(totalDeviceSpace)) * 100.0;

        // Soglia fissa: 2 GB (come da specifica originale)
        const quint64 minUnallocatedBytes = MIN_UNALLOCATED_BYTES; // 2 GB

        if (unallocatedPercent < UNALLOCATED_PERCENT_THRESHOLD ||
            unallocatedBytes < minUnallocatedBytes) {

            const QString warning = QStringLiteral(
                                        "⚠️ CRITICAL: Unallocated space is too low!\n"
                                        "  • Physical Unallocated: %1\n"
                                        "  • Logical Unallocated: %2 (%3% of total device space)\n"
                                        "  • RAID Factor: %4x\n"
                                        "  • Allocated Space: %5\n"
                                        "  • Total Device Space: %6\n"
                                        "  • Threshold: %7% or %8"
                                        )
                                        .arg(
                                            formatBytes(space.unallocated),
                                            formatBytes(unallocatedBytes),
                                            QString::number(unallocatedPercent, 'f', 1),
                                            QString::number(space.logicalFactor, 'f', 2),
                                            formatBytes(space.total),
                                            formatBytes(totalDeviceSpace),
                                            QString::number(UNALLOCATED_PERCENT_THRESHOLD),
                                            formatBytes(minUnallocatedBytes)
                                            );

            warnings.append(warning);
        }
    }
}

void BtrfsMonitor::evaluateMetadataRules(const QString &fsUuid, const FilesystemState &filesystem, QStringList &warnings) const
{
    Q_UNUSED(fsUuid);

    const SpaceInfo &space = filesystem.space;

    // Rimuovi questo debug o spostalo dentro il if
    // qDebug() << "REGOLA 2 " << space.metadataTotal << METADATA_SIZE_THRESHOLD << space.metadataTotal

    // ============================================================
    // REGOLA 2: DISPARITÀ DELL'USO DEI METADATI (EURISTICA)
    // ============================================================
    //
    // NOTA: Questa è una euristica, non una regola definitiva.
    // Genera un WARNING quando:
    //   - I chunk metadata allocati superano la soglia configurata
    //   - L'utilizzo dei metadata scende sotto il 30%
    //
    // Questo potrebbe indicare:
    //   - Metadata allocati eccessivamente per il carico di lavoro
    //   - Possibile spreco di spazio
    //   - Ma potrebbe anche essere normale per certi workload
    //
    // È una segnalazione informativa: non esegue alcuna azione automatica.

    if (space.metadataTotal > METADATA_SIZE_THRESHOLD && space.metadataTotal > 0) {

        const double metadataUsagePercent =
            (static_cast<double>(space.metadataUsed) /
             static_cast<double>(space.metadataTotal)) * 100.0;

        if (metadataUsagePercent < METADATA_USAGE_THRESHOLD) {

            const QString warning = QStringLiteral(
                                        "ℹ️ INFO: Metadata allocation efficiency check\n"
                                        "  • Metadata allocated: %1\n"
                                        "  • Metadata used: %2 (%3%)\n"
                                        "  • Unused metadata: %4 (%5%)\n"
                                        "  • Threshold: >%6 allocated and <%7% used\n"
                                        "\n"
                                        "  This is an indication, not a critical error.\n"
                                        "  No automatic action is performed."
                                        )
                                        .arg(
                                            formatBytes(space.metadataTotal),
                                            formatBytes(space.metadataUsed),
                                            QString::number(metadataUsagePercent, 'f', 1),
                                            formatBytes(space.metadataTotal - space.metadataUsed),
                                            QString::number(100.0 - metadataUsagePercent, 'f', 1),
                                            formatBytes(METADATA_SIZE_THRESHOLD),
                                            QString::number(METADATA_USAGE_THRESHOLD)
                                            );
            warnings.append(warning);
        }
    }
}


RaidImbalanceInfo BtrfsMonitor::evaluateRaidBalanceRules(const QString &fsUuid, const FilesystemState &filesystem, QStringList &warnings) const
{
    Q_UNUSED(fsUuid);

    RaidImbalanceInfo result;


    if (filesystem.deviceCount < 2) {

        qDebug().noquote()
        << "  • Skipping device allocation imbalance check:"
        << "deviceCount =" << filesystem.deviceCount;

        return result;
    }


    QList<QPair<QString, double>> deviceAllocation;


    for (auto it = filesystem.devices.constBegin();
         it != filesystem.devices.constEnd();
         ++it)
    {
        const DeviceState &devState =
            it.value();

        const QString &devId =
            it.key();


        /*
         * IMPORTANTE:
         *
         * Nel codice nuovo dovrebbe essere deviceAllocated,
         * non deviceUsed.
         */
        const quint64 total =
            devState.stats.deviceTotal;

        const quint64 allocated =
            devState.stats.deviceAllocated;


        qDebug().noquote()
            << "  • Device" << devId
            << "Total:" << formatBytes(total)
            << "Allocated:" << formatBytes(allocated);


        if (total == 0)
            continue;


        const quint64 safeAllocated =
            qMin(allocated, total);


        const double percent =
            (static_cast<double>(safeAllocated) /
             static_cast<double>(total)) * 100.0;


        deviceAllocation.append(
            qMakePair(
                devId,
                percent));
    }


    if (deviceAllocation.size() < 2) {

        qDebug().noquote()
        << "  • Not enough devices for"
        << "allocation imbalance check";

        return result;
    }


    const auto maxIt =
        std::max_element(
            deviceAllocation.constBegin(),
            deviceAllocation.constEnd(),
            [](const auto &a,
               const auto &b)
            {
                return a.second < b.second;
            });


    const auto minIt =
        std::min_element(
            deviceAllocation.constBegin(),
            deviceAllocation.constEnd(),
            [](const auto &a,
               const auto &b)
            {
                return a.second < b.second;
            });


    result.fullestDevice =
        maxIt->first;

    result.emptiestDevice =
        minIt->first;

    result.fullestPercent =
        maxIt->second;

    result.emptiestPercent =
        minIt->second;

    result.differencePercent =
        result.fullestPercent -
        result.emptiestPercent;


    qDebug().noquote()
        << "  • Allocation difference:"
        << QString::number(
               result.differencePercent,
               'f',
               1)
        << "%";


    if (result.differencePercent <=
        RAID_IMBALANCE_THRESHOLD)
    {
        qDebug().noquote()
        << "  • Device allocation imbalance"
        << "is within acceptable limits";

        return result;
    }


    result.detected = true;


    const QString warning =
        QStringLiteral(
            "⚠️ WARNING: Device allocation imbalance detected!\n"
            "  • Device %1: %2% allocated (highest)\n"
            "  • Device %3: %4% allocated (lowest)\n"
            "  • Difference: %5%\n"
            "  • Threshold: %6%"
            )
            .arg(
                result.fullestDevice,
                QString::number(
                    result.fullestPercent,
                    'f',
                    1),
                result.emptiestDevice,
                QString::number(
                    result.emptiestPercent,
                    'f',
                    1),
                QString::number(
                    result.differencePercent,
                    'f',
                    1),
                QString::number(
                    RAID_IMBALANCE_THRESHOLD,
                    'f',
                    1));


    warnings.append(warning);


    return result;
}

// ============================================================
// PROCESS FILESYSTEM
// ============================================================

void BtrfsMonitor::processFilesystem(const QString &fsUuid)
{
    const QString fsPath = QStringLiteral("/sys/fs/btrfs/") + fsUuid;

    FilesystemState &filesystem = m_filesystems[fsUuid];

    qDebug().noquote() << "\n" << QString(60, '-');
    qDebug().noquote() << "📁 Processing filesystem:" << fsUuid;
    qDebug().noquote() << QString(60, '-');

    // ============================================================
    // MOUNT POINT
    // ============================================================

    const QString mountPoint = findMountPointForUUID(fsUuid);

    if (!mountPoint.isEmpty())
    {
        filesystem.mountPoint = mountPoint;
        qDebug().noquote() << "  • Mount point:" << mountPoint;
    }
    else
    {
        qWarning().noquote() << "  ⚠ Cannot determine mount point for" << fsUuid.left(8);
    }

    // ============================================================
    // SPACE INFO
    // ============================================================

    SpaceInfo space;

    if (readSpaceInfo(fsUuid, space))
    {
        filesystem.space = space;
        qDebug().noquote() << "  ✅ Space info read successfully";
    }
    else
    {
        qWarning().noquote() << "  ❌ Failed to read space info";
    }

    // ============================================================
    // NUM DEVICES
    // ============================================================

    int deviceCount = 0;

    QFile numDevicesFile(fsPath + QStringLiteral("/num_devices"));

    if (numDevicesFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QTextStream in(&numDevicesFile);

        bool ok = false;
        deviceCount = in.readAll().trimmed().toInt(&ok);

        if (ok)
        {
            filesystem.deviceCount = deviceCount;
            qDebug().noquote() << "  • Device count from sysfs:" << deviceCount;
        }

        numDevicesFile.close();
    }
    else
    {
        qDebug().noquote() << "  • num_devices not available;" << "using devinfo count";
    }

    // ============================================================
    // DEVICE ERROR STATS
    // ============================================================

    QDir devinfoDir(fsPath + QStringLiteral("/devinfo"));

    QSet<QString> currentDevices;
    QStringList deviceIds;

    QSet<QString> memberDeviceIds;

    /*
    * Durante btrfs replace il target viene esposto
    * temporaneamente come device speciale, normalmente
    * con devid 0.
    *
    * Non deve essere considerato un normale membro
    * del filesystem dal monitor.
    */
    QSet<QString> replaceTargetDeviceIds;

    bool replaceActive = false;
    bool devinfoScanValid = false;

    if (devinfoDir.exists())
    {
        deviceIds = devinfoDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

        if (!deviceIds.isEmpty())
        {
            devinfoScanValid = true;
        }
        else
        {
            qWarning().noquote() << "  ⚠ devinfo scan returned no devices;" << "database cleanup will be skipped";
        }

        qDebug().noquote() << "  • Devices in devinfo:" << deviceIds.size() << deviceIds;

        for (const QString &devId : std::as_const(deviceIds))
        {

            bool isReplaceTarget = (devId == QStringLiteral("0"));

            /* I kernel recenti espongono anche /sys/fs/btrfs/<uuid>/devinfo/<devid>/replace_target con valore 1 per il target temporaneo. */
            QFile replaceTargetFile(devinfoDir.absoluteFilePath(devId + QStringLiteral("/replace_target")));

            if (replaceTargetFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                if (QString::fromUtf8(replaceTargetFile.readAll()).trimmed() == QStringLiteral("1")) isReplaceTarget = true;
                replaceTargetFile.close();
            }

            if (isReplaceTarget) {
                replaceTargetDeviceIds.insert(devId);
                replaceActive = true;
                qDebug().noquote() << "  • Device" << devId << "is temporary Btrfs replace target; excluded from normal device accounting";
                continue;
            }

            currentDevices.insert(devId);
            memberDeviceIds.insert(devId);

            DeviceState &device = filesystem.devices[devId];

            // ------------------------------------------------------------
            // DEVICE PRESENCE
            // ------------------------------------------------------------

            device.present = true;

            QFile missingFile(devinfoDir.absoluteFilePath(devId + QStringLiteral("/missing")));

            if (missingFile.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                const QString missingValue = QString::fromUtf8(missingFile.readAll()).trimmed();

                device.present = (missingValue != QStringLiteral("1"));

                missingFile.close();
            }

            qDebug().noquote() << "  • Device" << devId << "presence:" << (device.present ? QStringLiteral("present") : QStringLiteral("missing"));

            // ------------------------------------------------------------
            // ERROR STATS
            // ------------------------------------------------------------

            const QString statsPath = devinfoDir.absoluteFilePath(devId + QStringLiteral("/error_stats"));

            DeviceStats stats;

            if (readDeviceStats(statsPath, stats))
            {
                device.stats.readErrors = stats.readErrors;
                device.stats.writeErrors = stats.writeErrors;
                device.stats.flushErrors = stats.flushErrors;
                device.stats.corruptionErrors = stats.corruptionErrors;
                device.stats.generationErrors = stats.generationErrors;
            }
        }

        const int effectiveDeviceCount = deviceIds.size() - replaceTargetDeviceIds.size();

        if (deviceCount == 0 || deviceCount != effectiveDeviceCount)
        {
           filesystem.deviceCount = effectiveDeviceCount;
        }

        qDebug().noquote() << "  • Effective Btrfs member devices:" << effectiveDeviceCount;

    }
    else
    {
        qDebug().noquote() << "  • devinfo directory is not available";
    }

    // ============================================================
    // DEVICE ALLOCATION
    // ============================================================

    /*
     * Una sola esecuzione:
     *
     *     btrfs device usage <mountpoint>
     *
     * per filesystem.
     */

    if (!mountPoint.isEmpty()) {
        QHash<QString, DeviceStats> commandDeviceStats;

        if (readDeviceSpaceInfoFromBtrfsCommand(mountPoint, commandDeviceStats)) {
            for (auto it = commandDeviceStats.constBegin(); it != commandDeviceStats.constEnd(); ++it) {

                if (replaceTargetDeviceIds.contains(it.key()))
                {
                    qDebug().noquote() << "  • Ignoring allocation for" << "temporary replace target" << it.key();
                   continue;
                }

                DeviceState &state = filesystem.devices[it.key()];
                const QString commandDevicePath = it.value().devicePath.trimmed();

                if (!commandDevicePath.isEmpty() && !QFile::exists(commandDevicePath))
                {
                    state.present = false;

                    state.stats.deviceTotal = it.value().deviceTotal;

                    state.stats.deviceAllocated = it.value().deviceAllocated;

                    state.stats.deviceUnallocated = it.value().deviceUnallocated;

                    state.stats.devicePath = commandDevicePath;

                    currentDevices.insert(it.key());

                    continue;
                }

                if (it.value().deviceTotal == 0)
                {
                    state.present = false;

                    state.stats.deviceTotal = it.value().deviceTotal;

                    state.stats.deviceAllocated = it.value().deviceAllocated;

                    state.stats.deviceUnallocated = it.value().deviceUnallocated;

                    if (!commandDevicePath.isEmpty())
                    {
                        state.stats.devicePath = commandDevicePath;
                    }

                    currentDevices.insert(it.key());

                    continue;
                }

                state.stats.deviceTotal = it.value().deviceTotal;
                state.stats.deviceAllocated = it.value().deviceAllocated;
                state.stats.deviceUnallocated = it.value().deviceUnallocated;
                state.stats.devicePath = it.value().devicePath;
                currentDevices.insert(it.key());
            }

            const int effectiveCommandDeviceCount = commandDeviceStats.size() - replaceTargetDeviceIds.size();

            if (filesystem.deviceCount < effectiveCommandDeviceCount)
            {
                filesystem.deviceCount = effectiveCommandDeviceCount;
            }
        }
    }

    // ============================================================
    // REMOVE DEVICES NO LONGER PRESENT
    // ============================================================

    for (auto it = filesystem.devices.begin(); it != filesystem.devices.end();)
    {
        if (!currentDevices.contains(it.key()))
        {
            it = filesystem.devices.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // ============================================================
    // SCRUB / BALANCE
    // ============================================================

    // ============================================================
    // SCRUB / BALANCE
    // ============================================================

    /*
    * Device replace usa internamente il motore scrub
    * del kernel.
    *
    * Durante il replace `btrfs scrub status` può quindi
    * apparire "running", ma NON si tratta di uno scrub
    * di manutenzione avviato dall'utente o dallo scheduler.
    *
    * Non dobbiamo salvarlo nella history scrub.
    */
    if (!replaceActive)
    {
        processScrubState(fsUuid, fsPath);
    }
    else
    {
        qDebug().noquote() << "  • Device replace active;" << "scrub state tracking suppressed";
    }

    processBalanceState(fsUuid, fsPath);

    // ============================================================
    // LOGICAL SPACE
    // ============================================================

    calculateLogicalUnallocated(filesystem.space, filesystem.deviceCount);

    // ============================================================
    // DEBUG
    // ============================================================

    qDebug().noquote() << "\n📊 Final space info for" << fsUuid.left(8) << ":";
    qDebug().noquote() << QString(50, '-');

    qDebug().noquote() << "  • Physical Allocated:" << formatBytes(filesystem.space.total);
    qDebug().noquote() << "  • Used:" << formatBytes(filesystem.space.used);
    qDebug().noquote() << "  • Physical Unallocated:" << formatBytes(filesystem.space.unallocated);
    qDebug().noquote() << "  • Physical Device Total:" << formatBytes(filesystem.space.total + filesystem.space.unallocated);
    qDebug().noquote() << "  • Logical Unallocated:" << formatBytes(filesystem.space.logicalUnallocated);
    qDebug().noquote() << "  • Logical Factor:" << QString::number(filesystem.space.logicalFactor, 'f', 3);
    qDebug().noquote() << "  • Devices:" << filesystem.deviceCount;

    if (filesystem.space.metadataTotal > 0)
    {
        const double metaUsage = (static_cast<double>(filesystem.space.metadataUsed) / static_cast<double>(filesystem.space.metadataTotal)) * 100.0;

        qDebug().noquote() << "  • Metadata usage:" << QString::number(metaUsage, 'f', 1) << "%";
    }

    qDebug().noquote() << "  • Scrub status:" << m_scrubStates.value(fsUuid).status;
    qDebug().noquote() << "  • Balance status:" << m_balanceStates.value(fsUuid).status;

    qDebug().noquote() << QString(50, '-');

    // ============================================================
    // BASE HEALTH
    // ============================================================

    QString reason;

    HealthState newHealth = evaluateFilesystem(filesystem, currentDevices, reason);

    // ============================================================
    // ADDITIONAL RULES
    // ============================================================

    QStringList warnings;

    evaluateSpaceRules(fsUuid, filesystem, warnings);
    evaluateMetadataRules(fsUuid, filesystem, warnings);

    const RaidImbalanceInfo raidImbalance = evaluateRaidBalanceRules(fsUuid, filesystem, warnings);

    // ============================================================
    // RAID IMBALANCE EVENT
    // ============================================================

    if (m_eventManager && m_eventManager->isReady())
    {
        if (raidImbalance.detected)
        {
            BtrfsEventRecord event;

            event.fsUuid = fsUuid;
            event.mountPoint = mountPoint;

            /*
             * Evento globale del filesystem:
             * deviceId vuoto.
             */
            event.deviceId.clear();
            event.devicePath.clear();

            event.eventType = QStringLiteral("raid_imbalance");
            event.severity = QStringLiteral("warning");
            event.title = QStringLiteral("Btrfs device allocation imbalance");

            event.message = QStringLiteral("Device allocation imbalance detected. "
                                           "Highest: device %1 at %2%. "
                                           "Lowest: device %3 at %4%. "
                                           "Difference: %5%. "
                                           "Threshold: %6%.")
                                .arg(raidImbalance.fullestDevice,
                                     QString::number(raidImbalance.fullestPercent, 'f', 1),
                                     raidImbalance.emptiestDevice,
                                     QString::number(raidImbalance.emptiestPercent, 'f', 1),
                                     QString::number(raidImbalance.differencePercent, 'f', 1),
                                     QString::number(RAID_IMBALANCE_THRESHOLD, 'f', 1));

            /*
             * Memorizziamo le percentuali x100
             * perché previous/current sono INTEGER.
             *
             * 32.45% -> 3245
             */
            event.previousValue = static_cast<quint64>(raidImbalance.emptiestPercent * 100.0);
            event.currentValue = static_cast<quint64>(raidImbalance.fullestPercent * 100.0);

            event.active = true;
            event.acknowledged = false;

            if (!m_eventManager->recordOrUpdateActiveEvent(event))
            {
                qWarning() << "❌ Cannot persist RAID imbalance event for" << fsUuid.left(8);
            }
        }
        else
        {
            /*
             * La condizione non esiste più.
             *
             * resolveActiveEvent() deve essere idempotente:
             * se non esiste un evento attivo deve comunque
             * restituire true.
             */
            if (!m_eventManager->resolveActiveEvent(fsUuid, QString(), QStringLiteral("raid_imbalance")))
            {
                qWarning() << "❌ Cannot resolve RAID imbalance event for" << fsUuid.left(8);
            }
        }
    }
    else
    {
        qWarning() << "BtrfsEventManager is not initialized";
    }

    // ============================================================
    // MERGE WARNINGS INTO HEALTH REASON
    // ============================================================

    if (!warnings.isEmpty())
    {
        const QString combinedWarnings = warnings.join(QStringLiteral("\n\n"));

        if (reason.isEmpty() || reason == QStringLiteral("Btrfs filesystem is operational."))
        {
            reason = combinedWarnings;
        }
        else
        {
            reason += QStringLiteral("\n\n") + combinedWarnings;
        }

        if (newHealth == HealthState::Ok)
        {
            newHealth = HealthState::Warning;
        }
    }

    // ============================================================
    // UPDATE MEMORY + DATABASE
    // ============================================================

    updateFilesystemState(fsUuid, newHealth, reason);

    saveFilesystemState(fsUuid, filesystem);

    const bool devicesSaved = saveBtrfsDevices(fsUuid, filesystem);

    if (!devicesSaved)
    {
        qWarning() << "Cannot persist Btrfs devices for" << fsUuid.left(8);
    }
    else if (devinfoScanValid)
    {
        if (!removeStaleBtrfsDevices(fsUuid, memberDeviceIds))
        {
            qWarning() << "Cannot clean stale Btrfs devices for" << fsUuid.left(8);
        }
    }
    else
    {
        qWarning() << "Skipping Btrfs device cleanup because" << "devinfo scan was not valid for"<< fsUuid.left(8);
    }
}

// ============================================================
// CHECK BTRFS SYSFS (INVARIATO)
// ============================================================

void BtrfsMonitor::checkBtrfsSysfs()
{
    // Log separator with date/time
    qDebug().noquote() << "\n"
                       << QString(60, '=') << "\n"
                       << "🔍 Btrfs Monitor -"
                       << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
                       << "\n"
                       << QString(60, '=');

    QDir btrfsDir(QStringLiteral("/sys/fs/btrfs/"));

    if (!btrfsDir.exists()) {
        qWarning() << "❌ Btrfs sysfs not available.";
        return;
    }

    const QStringList filesystemList =
        btrfsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    QSet<QString> currentFilesystems;

    // VERIFICA FORMATO: Solo la prima volta e per il primo filesystem
    for (const QString &fsUuid : filesystemList) {
        // Skip special directories
        if (fsUuid == QStringLiteral("features") ||
            fsUuid == QStringLiteral("profiles")) {
            continue;
        }

        // Verifica il formato del file /info per il primo filesystem trovato
        if (!m_formatVerified) {
            verifySysfsFormat(fsUuid);
            m_formatVerified = true;
        }

        currentFilesystems.insert(fsUuid);
        processFilesystem(fsUuid);
    }

    // Missing filesystems
    const QStringList previousFilesystems = m_filesystems.keys();
    for (const QString &fsUuid : previousFilesystems) {
        if (currentFilesystems.contains(fsUuid))
            continue;

        const HealthState oldState = m_filesystems.value(fsUuid).health;

        if (oldState != HealthState::Unknown &&
            oldState != HealthState::Error) {

            const QString reason =
                QStringLiteral("Btrfs filesystem is no longer present in sysfs");

            emit healthChanged(fsUuid, HealthState::Error, reason);
            sendSystemNotification(fsUuid, HealthState::Error,
                                   QStringLiteral("Btrfs filesystem disappeared from the system!"));
        }

        m_filesystems.remove(fsUuid);
    }
}

// ============================================================
// GESTIONE PROFILI RAID (INVARIATI)
// ============================================================

BtrfsProfile BtrfsMonitor::stringToProfile(const QString &profileStr) const
{
    if (profileStr == QStringLiteral("single")) return BtrfsProfile::Single;
    if (profileStr == QStringLiteral("raid0")) return BtrfsProfile::Raid0;
    if (profileStr == QStringLiteral("raid1")) return BtrfsProfile::Raid1;
    if (profileStr == QStringLiteral("raid1c3")) return BtrfsProfile::Raid1c3;
    if (profileStr == QStringLiteral("raid1c4")) return BtrfsProfile::Raid1c4;
    if (profileStr == QStringLiteral("raid10")) return BtrfsProfile::Raid10;
    if (profileStr == QStringLiteral("raid5")) return BtrfsProfile::Raid5;
    if (profileStr == QStringLiteral("raid6")) return BtrfsProfile::Raid6;
    return BtrfsProfile::Unknown;
}

QString BtrfsMonitor::profileToString(BtrfsProfile profile) const
{
    switch (profile)
    {
    case BtrfsProfile::Single:  return QStringLiteral("single");
    case BtrfsProfile::Raid0:   return QStringLiteral("raid0");
    case BtrfsProfile::Raid1:   return QStringLiteral("raid1");
    case BtrfsProfile::Raid1c3: return QStringLiteral("raid1c3");
    case BtrfsProfile::Raid1c4: return QStringLiteral("raid1c4");
    case BtrfsProfile::Raid10:  return QStringLiteral("raid10");
    case BtrfsProfile::Raid5:   return QStringLiteral("raid5");
    case BtrfsProfile::Raid6:   return QStringLiteral("raid6");
    case BtrfsProfile::Unknown:
    default:                    return QStringLiteral("unknown");
    }
}

double BtrfsMonitor::getLogicalFactor(BtrfsProfile profile, int numDevices) const
{
    switch (profile) {
    case BtrfsProfile::Single:
    case BtrfsProfile::Raid0:
        return 1.0;

    case BtrfsProfile::Raid1:
        return 0.5; // 1/2

    case BtrfsProfile::Raid1c3:
        return 1.0 / 3.0; // 1/3

    case BtrfsProfile::Raid1c4:
        return 0.25; // 1/4

    case BtrfsProfile::Raid10:
        return 0.5; // 1/2

    case BtrfsProfile::Raid5:
        if (numDevices > 0) {
            return static_cast<double>(numDevices - 1) / numDevices;
        }
        return 1.0;

    case BtrfsProfile::Raid6:
        if (numDevices > 0) {
            return static_cast<double>(numDevices - 2) / numDevices;
        }
        return 1.0;

    case BtrfsProfile::Unknown:
    default:
        return 1.0;
    }
}

bool BtrfsMonitor::readProfileInfo(const QString &fsUuid, BtrfsProfile &dataProfile, BtrfsProfile &metadataProfile) const
{
    const QString basePath = QStringLiteral("/sys/fs/btrfs/") + fsUuid;
    QFile infoFile(basePath + QStringLiteral("/info"));

    if (!infoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "Unable to open info file for profile reading:" << infoFile.fileName();
        return false;
    }

    QTextStream in(&infoFile);
    QString line;

    // Cerca le righe con Data e Metadata
    static const QRegularExpression dataProfileRegex(
        QStringLiteral("Data\\s*:\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*(\\w+)"));
    static const QRegularExpression metadataProfileRegex(
        QStringLiteral("Metadata\\s*:\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*\\w+\\s*(\\w+)"));

    while (in.readLineInto(&line)) {
        QRegularExpressionMatch match;

        if ((match = dataProfileRegex.match(line)).hasMatch()) {
            dataProfile = stringToProfile(match.captured(1));
            qDebug() << "Data profile found:" << match.captured(1);
        }
        else if ((match = metadataProfileRegex.match(line)).hasMatch()) {
            metadataProfile = stringToProfile(match.captured(1));
            qDebug() << "Metadata profile found:" << match.captured(1);
        }
    }

    return (dataProfile != BtrfsProfile::Unknown || metadataProfile != BtrfsProfile::Unknown);
}

void BtrfsMonitor::calculateLogicalUnallocated(SpaceInfo &space, int numDevices) const
{
    // Usa il profilo Data come principale per il calcolo dello spazio
    BtrfsProfile primaryProfile = space.dataProfile;

    // Se il profilo Data è Unknown, prova con Metadata
    if (primaryProfile == BtrfsProfile::Unknown) {
        primaryProfile = space.metadataProfile;
    }

    // Calcola il fattore logico
    space.logicalFactor = getLogicalFactor(primaryProfile, numDevices);

    // Calcola lo spazio non allocato logico
    space.logicalUnallocated = static_cast<quint64>(
        static_cast<double>(space.unallocated) * space.logicalFactor
        );

    qDebug() << "Profile:" << static_cast<int>(primaryProfile)
             << "Devices:" << numDevices
             << "Factor:" << space.logicalFactor
             << "Physical Unallocated:" << formatBytes(space.unallocated)
             << "Logical Unallocated:" << formatBytes(space.logicalUnallocated);
}

// ============================================================
// METODI PER IL DEBUG DEL FORMATO (INVARIATI)
// ============================================================

void BtrfsMonitor::verifySysfsFormat(const QString &fsUuid) const
{
    const QString basePath = QStringLiteral("/sys/fs/btrfs/") + fsUuid;

    // Verifica il file /info NON C'E'
    QFile infoFile(basePath + QStringLiteral("/info"));

    qDebug() << basePath + QStringLiteral("/info");

    if (infoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&infoFile);
        QString line;

        qDebug().noquote() << "\n📄 Verifying /info format for" << fsUuid.left(8) << ":";
        qDebug().noquote() << QString(50, '-');

        int lineCount = 0;
        while (in.readLineInto(&line) && lineCount < 20) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()) {
                qDebug().noquote() << QString("  %1").arg(trimmed);
                lineCount++;
            }
        }
        infoFile.close();
        qDebug().noquote() << QString(50, '-');
    } else {
        qWarning() << "❌ Cannot open /info file for" << fsUuid.left(8);
    }

    // Verifica il file /allocation
    QFile allocFile(basePath + QStringLiteral("/allocation"));
    if (allocFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&allocFile);
        QString line;

        qDebug().noquote() << "\n📄 Verifying /allocation format for" << fsUuid.left(8) << ":";
        qDebug().noquote() << QString(50, '-');

        while (in.readLineInto(&line)) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && trimmed.contains("unallocated")) {
                qDebug().noquote() << QString("  %1").arg(trimmed);
            }
        }
        allocFile.close();
        qDebug().noquote() << QString(50, '-');
    } else {
        qWarning() << "❌ Cannot open /allocation file for" << fsUuid.left(8);
    }
}

void BtrfsMonitor::logSpaceInfo(const QString &fsUuid, const SpaceInfo &space) const
{
    qDebug().noquote() << "\n📊 Space info for" << fsUuid.left(8) << ":";
    qDebug().noquote() << QString(50, '-');
    qDebug().noquote() << "  • Allocated (total):" << formatBytes(space.total);
    qDebug().noquote() << "  • Used:" << formatBytes(space.used);
    qDebug().noquote() << "  • Unallocated:" << formatBytes(space.unallocated);
    qDebug().noquote() << "  • Total Device:" << formatBytes(space.total + space.unallocated);
    qDebug().noquote() << "  • Data Profile:" << static_cast<int>(space.dataProfile);
    qDebug().noquote() << "  • Metadata Profile:" << static_cast<int>(space.metadataProfile);
    qDebug().noquote() << "  • Logical Factor:" << QString::number(space.logicalFactor, 'f', 3);
    qDebug().noquote() << "  • Logical Unallocated:" << formatBytes(space.logicalUnallocated);

    if (space.metadataTotal > 0) {
        double metaUsage = (static_cast<double>(space.metadataUsed) /
                            static_cast<double>(space.metadataTotal)) * 100.0;
        qDebug().noquote() << "  • Metadata usage:" << QString::number(metaUsage, 'f', 1) << "%";
        qDebug().noquote() << "  • Metadata total:" << formatBytes(space.metadataTotal);
        qDebug().noquote() << "  • Metadata used:" << formatBytes(space.metadataUsed);
    }

    if (space.dataTotal > 0) {
        double dataUsage = (static_cast<double>(space.dataUsed) /
                            static_cast<double>(space.dataTotal)) * 100.0;
        qDebug().noquote() << "  • Data usage:" << QString::number(dataUsage, 'f', 1) << "%";
        qDebug().noquote() << "  • Data total:" << formatBytes(space.dataTotal);
        qDebug().noquote() << "  • Data used:" << formatBytes(space.dataUsed);
    }

    qDebug().noquote() << QString(50, '-');
}

QString BtrfsMonitor::getShortReason(const QString &reason) const
{
    if (reason.isEmpty()) {
        return QStringLiteral("Unknown issue");
    }

    // Pattern matching per estrarre la causa principale
    if (reason.contains("Unallocated space", Qt::CaseInsensitive)) {
        return QStringLiteral("Low unallocated space");
    }
    if (reason.contains("Metadata allocation", Qt::CaseInsensitive) || reason.contains("Metadata usage", Qt::CaseInsensitive)) {
        return QStringLiteral("Metadata inefficiency");
    }
    if (reason.contains("Device utilization imbalance", Qt::CaseInsensitive) || reason.contains("imbalance", Qt::CaseInsensitive)) {
        return QStringLiteral("RAID imbalance");
    }
    if (reason.contains("error(s) on device", Qt::CaseInsensitive) || reason.contains("read error", Qt::CaseInsensitive) || reason.contains("write error", Qt::CaseInsensitive)) {
        return QStringLiteral("Device errors");
    }
    if (reason.contains("no longer present", Qt::CaseInsensitive) || reason.contains("missing", Qt::CaseInsensitive)) {
        return QStringLiteral("Missing device");
    }
    if (reason.contains("corruption", Qt::CaseInsensitive) || reason.contains("corrupt", Qt::CaseInsensitive)) {
        return QStringLiteral("Data corruption");
    }
    if (reason.contains("Btrfs filesystem is operational", Qt::CaseInsensitive) || reason.contains("operational", Qt::CaseInsensitive)) {
        return QStringLiteral("All healthy");
    }

    // Se non riconosciuto, prendi la prima riga e accorcia
    QStringList lines = reason.split('\n', Qt::SkipEmptyParts);
    if (!lines.isEmpty()) {
        QString first = lines.first().trimmed();

        // Rimuovi emoji e caratteri speciali all'inizio
        static const QRegularExpression leadingEmojiRegex(QStringLiteral("^[⚠️❌✅ℹ️📊🔍📁📄]*\\s*"));
        first.remove(leadingEmojiRegex);

        // Rimuovi etichette comuni
        static const QRegularExpression severityLabelRegex(QStringLiteral("^(CRITICAL:|WARNING:|ERROR:|INFO:)\\s*"), QRegularExpression::CaseInsensitiveOption);
        first.remove(severityLabelRegex);;

        if (first.length() > 50) {
            return first.left(47) + "...";
        }
        return first;
    }

    // Se tutto fallisce, tronca il reason
    if (reason.length() > 50) {
        return reason.left(47) + "...";
    }
    return reason;
}

// ============================================================
// SMART DEVICE UPDATED
// ============================================================

void BtrfsMonitor::onSmartDeviceUpdated(const QString &devicePath)
{
    if (!m_dbInitialized || !m_db.isValid() || !m_db.isOpen())
    {
        return;
    }

    // ========================================================
    // LEGGI LO STATO SMART APPENA SALVATO
    // ========================================================

    QSqlQuery query(m_db);

    query.prepare(QStringLiteral("SELECT model_name, health_state, health_reason, temperature_celsius, read_ok FROM smart_devices WHERE device_path = :device_path LIMIT 1"));

    query.bindValue(QStringLiteral(":device_path"), devicePath);

    if (!query.exec())
    {
        qWarning() << "❌ Cannot read SMART state for" << devicePath << ":"  << query.lastError().text();
        return;
    }

    if (!query.next())
    {
        qWarning() << "SMART device not found in database:" << devicePath;

        return;
    }

    // ========================================================
    // DATI CORRENTI
    // ========================================================

    const QString modelName = query.value(QStringLiteral("model_name")).toString();

    const int newHealth = query.value(QStringLiteral("health_state")).toInt();

    const QString reason = query.value(QStringLiteral("health_reason")).toString();

    const int temperature = query.value(QStringLiteral("temperature_celsius")).toInt();

    const bool readOk = query.value(QStringLiteral("read_ok")).toInt() != 0;


    QString temperatureText;

    if (temperature >= 0)
    {
        temperatureText =
            QStringLiteral("%1 °C")
                .arg(temperature);
    }
    else
    {
        temperatureText =
            QStringLiteral("N/A");
    }

    // ========================================================
    // LOG
    // ========================================================

    qInfo().noquote()
        << "SMART:"
        << devicePath
        << modelName
        << "health:"
        << newHealth
        << "temperature:"
        << temperatureText
        << "-"
        << reason;

    // ========================================================
    // LETTURA SMART FALLITA
    // ========================================================

    /*
     * Un errore di lettura smartctl non equivale
     * automaticamente a un guasto hardware.
     *
     * Non modifichiamo nemmeno m_lastSmartHealth:
     * una lettura fallita non rappresenta un nuovo stato SMART.
     */
    if (!readOk)
    {
        qWarning().noquote()
        << "SMART data unavailable for"
        << devicePath;

        return;
    }

    // ========================================================
    // STATO PRECEDENTE
    // ========================================================

    const bool firstDetection =
        !m_lastSmartHealth.contains(
            devicePath);

    int oldHealth =
        static_cast<int>(
            SmartHealthState::Unknown);

    if (!firstDetection)
    {
        oldHealth =
            m_lastSmartHealth.value(
                devicePath);
    }

    /*
     * La lettura è valida:
     * memorizziamo lo stato appena osservato.
     */
    m_lastSmartHealth.insert(
        devicePath,
        newHealth);

    // ========================================================
    // PRIMA RILEVAZIONE
    // ========================================================

    /*
     * All'avvio:
     *
     * OK      -> nessuna notifica
     * UNKNOWN -> nessuna notifica
     * WARNING -> notifica
     * ERROR   -> notifica
     */
    if (firstDetection)
    {
        if (newHealth != static_cast<int>(SmartHealthState::Warning) && newHealth != static_cast<int>(SmartHealthState::Error))
        {
            return;
        }
    }
    else
    {
        // ====================================================
        // STATO INVARIATO
        // ====================================================

        /*
         * Warning -> Warning
         * Error   -> Error
         * Ok      -> Ok
         *
         * Nessun nuovo desktop notification
         * e nessuna nuova email.
         */
        if (oldHealth == newHealth)
        {
            return;
        }
    }

    // ========================================================
    // DEVICE LABEL
    // ========================================================

    QString deviceLabel =
        devicePath;

    if (!modelName.isEmpty())
    {
        deviceLabel =
            QStringLiteral("%1 (%2)")
                .arg(
                    modelName,
                    devicePath);
    }

    // ========================================================
    // COSTRUZIONE NOTIFICA
    // ========================================================

    QString title;
    QString message;
    QString emailSeverity;

    SystemNotifier::NotificationPriority priority = SystemNotifier::Normal;

    int timeout = 8000;

    switch (static_cast<SmartHealthState>(newHealth))
    {
        // ----------------------------------------------------
        // ERROR
        // ----------------------------------------------------

    case SmartHealthState::Error:
    {
        title = QStringLiteral("❌ SMART CRITICAL");
        message = QStringLiteral("%1: %2").arg(deviceLabel, reason);
        emailSeverity = QStringLiteral("critical");
        priority = SystemNotifier::Critical;
        timeout = 0;

        break;
    }

        // ----------------------------------------------------
        // WARNING
        // ----------------------------------------------------

    case SmartHealthState::Warning:
    {
        title = QStringLiteral("⚠️ SMART Warning");
        message = QStringLiteral("%1: %2").arg(deviceLabel, reason);
        emailSeverity = QStringLiteral("warning");
        priority = SystemNotifier::Normal;
        timeout = 10000;

        break;
    }

        // ----------------------------------------------------
        // RECOVERY
        // ----------------------------------------------------

    case SmartHealthState::Ok:
    {
        /*
             * Un OK genera "Recovered" solamente
             * se prima avevamo un vero problema.
             */
        const bool wasProblem = oldHealth == static_cast<int>(SmartHealthState::Warning) ||
            oldHealth == static_cast<int>(SmartHealthState::Error);

        if (!wasProblem)
        {
            return;
        }

        title = QStringLiteral("✅ SMART Recovered");
        message = QStringLiteral("%1 is healthy").arg(deviceLabel);
        emailSeverity = QStringLiteral("info");
        priority = SystemNotifier::Low;
        timeout = 5000;

        break;
    }

        // ----------------------------------------------------
        // UNKNOWN
        // ----------------------------------------------------

    case SmartHealthState::Unknown:
    default:
    {
        /*
             * UNKNOWN non genera né notifica desktop
             * né email.
             */
        return;
    }
    }

    // ========================================================
    // DESKTOP NOTIFICATION
    // ========================================================

    if (m_notifier)
    {
        m_notifier->sendNotification(title, message, priority, timeout);
    }

    // ========================================================
    // EMAIL NOTIFICATION
    // ========================================================

    if (m_emailNotifier)
    {
        /*
         * Best effort:
         * un errore email non deve interferire
         * con monitoraggio o persistenza.
         *
         * EmailNotifier esegue autonomamente il logging
         * dell'eventuale errore msmtp/sendmail.
         */
        m_emailNotifier->sendNotification(title, message, emailSeverity);
    }
}

void BtrfsMonitor::refreshNow()
{
    checkBtrfsSysfs();
}
