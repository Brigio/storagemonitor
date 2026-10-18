// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfseventmanager.h"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>


namespace
{

/*
 * ============================================================
 * NORMALIZZAZIONE DEVICE ID
 * ============================================================
 *
 * device_id nel database è NOT NULL.
 *
 * Un evento relativo all'intero filesystem utilizza quindi
 * una stringa vuota, NON NULL.
 */
QString normalizedDeviceId(const QString &deviceId)
{
    if (deviceId.isEmpty())
        return QLatin1String("");

    return deviceId;
}

/*
 * ============================================================
 * TESTO NON NULL
 * ============================================================
 *
 * Utile per colonne TEXT NOT NULL.
 */
QString nonNullText(const QString &text)
{
    if (text.isNull())
        return QLatin1String("");

    return text;
}

/*
 * ============================================================
 * DELTA
 * ============================================================
 */
qint64 calculateDelta(quint64 previousValue, quint64 currentValue)
{
    const qint64 previous = static_cast<qint64>(previousValue);

    const qint64 current = static_cast<qint64>(currentValue);

    return current - previous;
}

} // namespace


// ============================================================
// COSTRUTTORE
// ============================================================

BtrfsEventManager::BtrfsEventManager(const QSqlDatabase &database, QObject *parent) : QObject(parent)
{
    /*
     * Non memorizziamo QSqlDatabase.
     *
     * Memorizziamo solamente il connectionName della connessione
     * già aperta da BtrfsMonitor.
     */

    if (!database.isValid())
    {
        qCritical() << "❌ BtrfsEventManager:" << "invalid database";
        return;
    }


    if (!database.isOpen())
    {
        qCritical() << "❌ BtrfsEventManager:" << "database is not open";
        return;
    }

    m_connectionName = database.connectionName();

    if (m_connectionName.isEmpty())
    {
        qCritical() << "❌ BtrfsEventManager:" << "database connection name is empty";
        return;
    }

    /*
     * Assicura che la tabella degli eventi esista.
     */
    m_ready = ensureSchema();


    if (m_ready)
    {
        qDebug() << "✅ BtrfsEventManager initialized on connection:" << m_connectionName;
    }
    else
    {
        qCritical() << "❌ BtrfsEventManager initialization failed";
    }
}


// ============================================================
// IS READY
// ============================================================

bool BtrfsEventManager::isReady() const
{
    return m_ready;
}


// ============================================================
// ENSURE SCHEMA
// ============================================================

bool BtrfsEventManager::ensureSchema()
{
    if (m_connectionName.isEmpty())
        return false;


    QSqlDatabase db = QSqlDatabase::database(
            m_connectionName,
            false);


    if (!db.isValid() || !db.isOpen())
    {
        qCritical() << "❌ BtrfsEventManager:" << "database connection unavailable";
        return false;
    }

    // ========================================================
    // TABLE
    // ========================================================

    QSqlQuery query(db);


    const QString createTable =
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS btrfs_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,

                fs_uuid TEXT NOT NULL,
                mount_point TEXT,

                device_id TEXT NOT NULL DEFAULT '',
                device_path TEXT,

                event_type TEXT NOT NULL,
                severity TEXT NOT NULL,

                title TEXT NOT NULL,
                message TEXT NOT NULL,

                previous_value INTEGER DEFAULT 0,
                current_value INTEGER DEFAULT 0,
                delta INTEGER DEFAULT 0,

                occurrence_count INTEGER DEFAULT 1,

                active INTEGER NOT NULL DEFAULT 1,
                acknowledged INTEGER NOT NULL DEFAULT 0,

                first_seen INTEGER NOT NULL,
                last_seen INTEGER NOT NULL,

                resolved_at INTEGER,

                created_at INTEGER DEFAULT (strftime('%s','now')),
                updated_at INTEGER DEFAULT (strftime('%s','now')),

                CHECK(active IN (0,1)),
                CHECK(acknowledged IN (0,1)),
                CHECK(occurrence_count >= 1)
            )
        )");


    if (!query.exec(createTable))
    {
        qCritical() << "❌ Cannot create btrfs_events table:" << query.lastError().text();
        return false;
    }

    // ========================================================
    // INDEXES
    // ========================================================

    const QStringList indexes =
        {
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_btrfs_events_fs_uuid "
                "ON btrfs_events(fs_uuid)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_btrfs_events_type "
                "ON btrfs_events(event_type)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_btrfs_events_active "
                "ON btrfs_events(active)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_btrfs_events_last_seen "
                "ON btrfs_events(last_seen DESC)"
                ),

            /*
         * Indice principale per:
         *
         * findActiveEventId()
         * resolveActiveEvent()
         */
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_btrfs_events_active_lookup "
                "ON btrfs_events("
                "fs_uuid, device_id, event_type, active)"
                )
        };


    for (const QString &sql : indexes)
    {
        if (!query.exec(sql))
        {
            qWarning() << "⚠️ Cannot create btrfs_events index:" << query.lastError().text();
            /*
             * Un indice fallito non rende inutilizzabile
             * la tabella.
             */
        }
    }

    return true;
}


// ============================================================
// INSERT EVENT
// ============================================================

bool BtrfsEventManager::insertEvent(const BtrfsEventRecord &event, qint64 *insertedId)
{
    if (insertedId)
        *insertedId = -1;


    if (!m_ready)
    {
        qWarning() << "BtrfsEventManager:" << "manager not ready";
        return false;
    }


    const QString fsUuid = event.fsUuid.trimmed();

    const QString eventType = event.eventType.trimmed().toLower();

    const QString severity = event.severity.trimmed().toLower();

    /*
     * Campi obbligatori.
     */
    if (fsUuid.isEmpty())
    {
        qWarning() << "Cannot insert Btrfs event:" << "fs_uuid is empty";
        return false;
    }


    if (eventType.isEmpty())
    {
        qWarning() << "Cannot insert Btrfs event:" << "event_type is empty";
        return false;
    }

    if (severity.isEmpty())
    {
        qWarning() << "Cannot insert Btrfs event:" << "severity is empty";
        return false;
    }


    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen())
    {
        qWarning() << "Cannot insert Btrfs event:" << "database unavailable";
        return false;
    }

    const QString deviceId = normalizedDeviceId(event.deviceId);

    const qint64 previousValue = static_cast<qint64>(event.previousValue);

    const qint64 currentValue = static_cast<qint64>(event.currentValue);

    const qint64 delta =
        calculateDelta(
            event.previousValue,
            event.currentValue);


    QSqlQuery query(db);


    query.prepare(
        QStringLiteral(R"(
            INSERT INTO btrfs_events (
                fs_uuid,
                mount_point,

                device_id,
                device_path,

                event_type,
                severity,

                title,
                message,

                previous_value,
                current_value,
                delta,

                occurrence_count,

                active,
                acknowledged,

                first_seen,
                last_seen,

                resolved_at,

                created_at,
                updated_at
            )
            VALUES (
                :fs_uuid,
                :mount_point,

                :device_id,
                :device_path,

                :event_type,
                :severity,

                :title,
                :message,

                :previous_value,
                :current_value,
                :delta,

                1,

                :active,
                :acknowledged,

                strftime('%s','now'),
                strftime('%s','now'),

                CASE
                    WHEN :resolved = 1
                    THEN strftime('%s','now')
                    ELSE NULL
                END,

                strftime('%s','now'),
                strftime('%s','now')
            )
        )"));


    query.bindValue(
        QStringLiteral(":fs_uuid"),
        fsUuid);

    query.bindValue(
        QStringLiteral(":mount_point"),
        nonNullText(
            event.mountPoint));

    query.bindValue(
        QStringLiteral(":device_id"),
        deviceId);

    query.bindValue(
        QStringLiteral(":device_path"),
        nonNullText(
            event.devicePath));

    query.bindValue(
        QStringLiteral(":event_type"),
        eventType);

    query.bindValue(
        QStringLiteral(":severity"),
        severity);

    query.bindValue(
        QStringLiteral(":title"),
        nonNullText(
            event.title));

    query.bindValue(
        QStringLiteral(":message"),
        nonNullText(
            event.message));

    query.bindValue(
        QStringLiteral(":previous_value"),
        previousValue);

    query.bindValue(
        QStringLiteral(":current_value"),
        currentValue);

    query.bindValue(
        QStringLiteral(":delta"),
        delta);

    query.bindValue(
        QStringLiteral(":active"),
        event.active ? 1 : 0);

    query.bindValue(
        QStringLiteral(":acknowledged"),
        event.acknowledged ? 1 : 0);

    query.bindValue(
        QStringLiteral(":resolved"),
        event.active ? 0 : 1);


    if (!query.exec())
    {
        qWarning().noquote()
        << "❌ Cannot insert Btrfs event:"
        << eventType
        << "-"
        << query.lastError().text();

        return false;
    }


    if (insertedId)
    {
        *insertedId =
            query.lastInsertId()
                .toLongLong();
    }


    qDebug().noquote()
        << "📝 Btrfs event inserted:"
        << eventType
        << "fs:"
        << fsUuid.left(8);


    return true;
}


// ============================================================
// FIND ACTIVE EVENT
// ============================================================

bool BtrfsEventManager::findActiveEventId(
    const QString &fsUuid,
    const QString &deviceId,
    const QString &eventType,
    qint64 &eventId) const
{
    eventId = -1;


    if (!m_ready)
        return false;


    QSqlDatabase db =
        QSqlDatabase::database(
            m_connectionName,
            false);


    if (!db.isValid() ||
        !db.isOpen())
    {
        qWarning()
        << "Cannot find active Btrfs event:"
        << "database unavailable";

        return false;
    }


    QSqlQuery query(db);


    query.prepare(
        QStringLiteral(R"(
            SELECT id
            FROM btrfs_events

            WHERE fs_uuid = :fs_uuid
              AND device_id = :device_id
              AND event_type = :event_type
              AND active = 1

            ORDER BY id DESC

            LIMIT 1
        )"));


    query.bindValue(
        QStringLiteral(":fs_uuid"),
        fsUuid.trimmed());

    query.bindValue(
        QStringLiteral(":device_id"),
        normalizedDeviceId(
            deviceId));

    query.bindValue(
        QStringLiteral(":event_type"),
        eventType
            .trimmed()
            .toLower());


    if (!query.exec())
    {
        qWarning().noquote()
        << "❌ Cannot search active Btrfs event:"
        << query.lastError().text();

        return false;
    }


    /*
     * Query corretta ma nessun evento.
     *
     * Non è un errore.
     */
    if (!query.next())
        return true;


    eventId =
        query.value(0)
            .toLongLong();


    return true;
}


// ============================================================
// UPDATE ACTIVE EVENT
// ============================================================

bool BtrfsEventManager::updateActiveEvent(
    qint64 eventId,
    const BtrfsEventRecord &event)
{
    if (!m_ready ||
        eventId < 0)
    {
        return false;
    }


    QSqlDatabase db =
        QSqlDatabase::database(
            m_connectionName,
            false);


    if (!db.isValid() ||
        !db.isOpen())
    {
        qWarning()
        << "Cannot update active Btrfs event:"
        << "database unavailable";

        return false;
    }


    const qint64 previousValue =
        static_cast<qint64>(
            event.previousValue);

    const qint64 currentValue =
        static_cast<qint64>(
            event.currentValue);


    const qint64 delta =
        calculateDelta(
            event.previousValue,
            event.currentValue);


    QSqlQuery query(db);


    /*
     * NON modifichiamo acknowledged.
     *
     * Se in futuro l'utente riconosce l'evento tramite D-Bus,
     * un nuovo ciclo del monitor non deve riportarlo
     * automaticamente ad acknowledged = 0.
     */
    query.prepare(
        QStringLiteral(R"(
            UPDATE btrfs_events

            SET
                mount_point = :mount_point,
                device_path = :device_path,

                severity = :severity,

                title = :title,
                message = :message,

                previous_value = :previous_value,
                current_value = :current_value,
                delta = :delta,

                occurrence_count =
                    occurrence_count + 1,

                active = 1,

                last_seen =
                    strftime('%s','now'),

                resolved_at = NULL,

                updated_at =
                    strftime('%s','now')

            WHERE id = :id
              AND active = 1
        )"));


    query.bindValue(
        QStringLiteral(":id"),
        eventId);

    query.bindValue(
        QStringLiteral(":mount_point"),
        nonNullText(
            event.mountPoint));

    query.bindValue(
        QStringLiteral(":device_path"),
        nonNullText(
            event.devicePath));

    query.bindValue(
        QStringLiteral(":severity"),
        event.severity
            .trimmed()
            .toLower());

    query.bindValue(
        QStringLiteral(":title"),
        nonNullText(
            event.title));

    query.bindValue(QStringLiteral(":message"), nonNullText(event.message));

    query.bindValue(QStringLiteral(":previous_value"), previousValue);

    query.bindValue(QStringLiteral(":current_value"), currentValue);

    query.bindValue(QStringLiteral(":delta"), delta);

    if (!query.exec())
    {
        qWarning().noquote() << "❌ Cannot update active Btrfs event:" << event.eventType << "-" << query.lastError().text();
        return false;
    }


    if (query.numRowsAffected() <= 0)
    {
        qWarning() << "Active Btrfs event disappeared before update:" << eventId;
        return false;
    }

    return true;
}


// ============================================================
// RECORD OR UPDATE ACTIVE EVENT
// ============================================================

bool BtrfsEventManager::recordOrUpdateActiveEvent(const BtrfsEventRecord &event)
{
    if (!m_ready)
    {
        qWarning() << "Cannot persist active Btrfs event:" << "manager not ready";
        return false;
    }


    const QString fsUuid = event.fsUuid.trimmed();

    const QString eventType = event.eventType.trimmed().toLower();

    if (fsUuid.isEmpty() ||
        eventType.isEmpty())
    {
        qWarning() << "Cannot persist active Btrfs event:" << "invalid key";
        return false;
    }

    qint64 eventId = -1;

    /*
     * Cerca:
     *
     * fs_uuid + device_id + event_type + active=1
     */
    if (!findActiveEventId(fsUuid, event.deviceId, eventType, eventId))
    {
        return false;
    }

    /*
     * Già presente:
     * aggiorniamo la stessa riga.
     */
    if (eventId >= 0)
    {
        return updateActiveEvent(eventId, event);
    }

    /*
     * Nuova condizione.
     */
    BtrfsEventRecord activeEvent = event;

    activeEvent.fsUuid = fsUuid;

    activeEvent.eventType = eventType;

    activeEvent.active = true;

    return insertEvent(activeEvent);
}


// ============================================================
// RESOLVE ACTIVE EVENT
// ============================================================

bool BtrfsEventManager::resolveActiveEvent(const QString &fsUuid, const QString &deviceId, const QString &eventType)
{
    if (!m_ready) return false;

    const QString normalizedFsUuid = fsUuid.trimmed();
    const QString normalizedEventType = eventType.trimmed().toLower();
    if (normalizedFsUuid.isEmpty() || normalizedEventType.isEmpty()) { qWarning() << "Cannot resolve Btrfs event: invalid key"; return false; }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isValid() || !db.isOpen()) { qWarning() << "Cannot resolve Btrfs event: database unavailable"; return false; }

    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE btrfs_events SET active = 0, resolved_at = strftime('%s','now'), updated_at = strftime('%s','now') WHERE fs_uuid = :fs_uuid AND device_id = :device_id AND event_type = :event_type AND active = 1"));
    query.bindValue(QStringLiteral(":fs_uuid"), normalizedFsUuid);
    query.bindValue(QStringLiteral(":device_id"), normalizedDeviceId(deviceId));
    query.bindValue(QStringLiteral(":event_type"), normalizedEventType);

    if (!query.exec()) { qWarning().noquote() << "❌ Cannot resolve Btrfs event:" << normalizedEventType << "-" << query.lastError().text(); return false; }
    if (query.numRowsAffected() > 0) { qDebug().noquote() << "✅ Btrfs event resolved:" << normalizedEventType << "fs:" << normalizedFsUuid.left(8); }

    return true;
}

// ============================================================
// RESOLVE EVENT BY ID
// ============================================================

bool BtrfsEventManager::resolveEvent(qint64 eventId)
{
    if (!m_ready || eventId <= 0) { qWarning() << "BtrfsEventManager::resolveEvent: invalid event id" << eventId; return false; }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isValid() || !db.isOpen()) { qWarning() << "Cannot resolve Btrfs event: database unavailable"; return false; }

    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE btrfs_events SET active = 0, resolved_at = strftime('%s','now'), updated_at = strftime('%s','now') WHERE id = :id AND active = 1"));
    query.bindValue(QStringLiteral(":id"), eventId);

    if (!query.exec()) { qWarning().noquote() << "Cannot resolve Btrfs event" << eventId << "-" << query.lastError().text(); return false; }
    if (query.numRowsAffected() > 0) { qInfo() << "Btrfs event resolved:" << eventId; return true; }

    /* Nessuna riga modificata. Distinguiamo: evento già risolto -> true, id inesistente -> false */
    QSqlQuery checkQuery(db);
    checkQuery.prepare(QStringLiteral("SELECT active FROM btrfs_events WHERE id = :id LIMIT 1"));
    checkQuery.bindValue(QStringLiteral(":id"), eventId);

    if (!checkQuery.exec()) { qWarning().noquote() << "Cannot verify resolved Btrfs event" << eventId << "-" << checkQuery.lastError().text(); return false; }
    if (!checkQuery.next()) { qWarning() << "Btrfs event not found:" << eventId; return false; }

    return checkQuery.value(0).toInt() == 0;
}


// ============================================================
// ACKNOWLEDGE EVENT
// ============================================================

bool BtrfsEventManager::acknowledgeEvent(qint64 eventId)
{
    if (!m_ready || eventId <= 0) { qWarning() << "BtrfsEventManager::acknowledgeEvent: invalid event id" << eventId; return false; }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isValid() || !db.isOpen()) { qWarning() << "Cannot acknowledge Btrfs event: database unavailable"; return false; }

    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE btrfs_events SET acknowledged = 1, updated_at = strftime('%s','now') WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), eventId);

    if (!query.exec()) { qWarning().noquote() << "Cannot acknowledge Btrfs event" << eventId << "-" << query.lastError().text(); return false; }
    if (query.numRowsAffected() > 0) { qInfo() << "Btrfs event acknowledged:" << eventId; return true; }

    /* Nessuna riga modificata. Distinguiamo: evento già acknowledged -> true, id inesistente -> false */
    QSqlQuery checkQuery(db);
    checkQuery.prepare(QStringLiteral("SELECT acknowledged FROM btrfs_events WHERE id = :id LIMIT 1"));
    checkQuery.bindValue(QStringLiteral(":id"), eventId);

    if (!checkQuery.exec()) { qWarning().noquote() << "Cannot verify acknowledged Btrfs event" << eventId << "-" << checkQuery.lastError().text(); return false; }
    if (!checkQuery.next()) { qWarning() << "Btrfs event not found:" << eventId; return false; }

    return checkQuery.value(0).toInt() != 0;
}

// ============================================================
// CLEAR RESOLVED EVENTS
// ============================================================

bool BtrfsEventManager::clearResolvedEvents()
{
    if (!m_ready)
    {
        qWarning() << "BtrfsEventManager::clearResolvedEvents:" << "manager not ready";
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen())
    {
        qWarning() << "Cannot clear Btrfs event history:" << "database unavailable";
        return false;
    }

    QSqlQuery query(db);

    if (!query.exec(QStringLiteral("DELETE FROM btrfs_events WHERE active = 0")))
    {
        qWarning().noquote() << "Cannot clear Btrfs event history:" << query.lastError().text();
        return false;
    }

    qInfo() << "Btrfs resolved event history cleared:" << query.numRowsAffected() << "row(s)";
    return true;
}
