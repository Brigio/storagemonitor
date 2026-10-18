// SPDX-License-Identifier: GPL-3.0-or-later

#include "smarteventmanager.h"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>


namespace
{

QString nonNullText(const QString &text)
{
    if (text.isNull())
        return QLatin1String("");
    return text;
}


bool isValidEventRecord(const SmartEventRecord &event)
{
    return !event.devicePath.isEmpty() && !event.eventType.isEmpty() && !event.severity.isEmpty() && !event.title.isEmpty();
}

} // namespace


// ============================================================
// COSTRUTTORE
// ============================================================

SmartEventManager::SmartEventManager(
    const QSqlDatabase &database,
    QObject *parent)
    : QObject(parent)
{
    if (!database.isValid())
    {
        qCritical() << "❌ SmartEventManager: invalid database";
        return;
    }

    if (!database.isOpen())
    {
        qCritical() << "❌ SmartEventManager: database is not open";
        return;
    }

    m_connectionName = database.connectionName();

    if (m_connectionName.isEmpty())
    {
        qCritical() << "❌ SmartEventManager: database connection name is empty";
        return;
    }

    m_ready = ensureSchema();

    if (!m_ready)
    {
        qCritical() << "❌ SmartEventManager: schema initialization failed";
    }
}


// ============================================================
// STATO
// ============================================================

bool SmartEventManager::isReady() const
{
    return m_ready;
}


// ============================================================
// SCHEMA SQLITE
// ============================================================

bool SmartEventManager::ensureSchema()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen())
    {
        return false;
    }

    QSqlQuery query(db);

    const QString createTable =
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS smart_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,

                device_path TEXT NOT NULL,
                serial_number TEXT NOT NULL DEFAULT '',
                model_name TEXT NOT NULL DEFAULT '',

                event_type TEXT NOT NULL,
                severity TEXT NOT NULL,

                title TEXT NOT NULL,
                message TEXT NOT NULL,

                previous_value INTEGER NOT NULL DEFAULT 0,
                current_value INTEGER NOT NULL DEFAULT 0,

                occurrence_count INTEGER NOT NULL DEFAULT 1,

                active INTEGER NOT NULL DEFAULT 1,
                acknowledged INTEGER NOT NULL DEFAULT 0,

                first_seen INTEGER NOT NULL,
                last_seen INTEGER NOT NULL,

                resolved_at INTEGER,

                created_at INTEGER DEFAULT (strftime('%s','now')),
                updated_at INTEGER DEFAULT (strftime('%s','now')),

                CHECK(active IN (0,1)),
                CHECK(acknowledged IN (0,1)),
                CHECK(occurrence_count >= 1),
                CHECK(severity IN ('info','warning','critical'))
            )
        )");

    if (!query.exec(createTable))
    {
        qCritical()
        << "❌ Cannot create smart_events table:"
        << query.lastError().text();

        return false;
    }

    const QStringList indexes =
        {
            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_smart_events_device "
                "ON smart_events(device_path)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_smart_events_type "
                "ON smart_events(event_type)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_smart_events_active "
                "ON smart_events(active)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_smart_events_last_seen "
                "ON smart_events(last_seen)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_smart_events_active_lookup "
                "ON smart_events(device_path, event_type, active)"
                ),

            QStringLiteral(
                "CREATE INDEX IF NOT EXISTS "
                "idx_smart_events_ack_state "
                "ON smart_events(active, acknowledged)"
                ),

            /*
         * Difesa a livello DB: per un device e un event_type non
         * possono esistere due righe contemporaneamente attive.
         */
            QStringLiteral(
                "CREATE UNIQUE INDEX IF NOT EXISTS "
                "idx_smart_events_one_active "
                "ON smart_events(device_path, event_type) "
                "WHERE active = 1"
                )
        };

    for (const QString &sql : indexes)
    {
        if (!query.exec(sql))
        {
            qCritical()
            << "❌ Cannot create SMART event index:"
            << query.lastError().text();

            return false;
        }
    }

    return true;
}


// ============================================================
// CERCA EVENTO ATTIVO
// ============================================================

bool SmartEventManager::findActiveEventId(
    const QString &devicePath,
    const QString &eventType,
    qint64 &eventId) const
{
    eventId = -1;

    if (!m_ready ||
        devicePath.isEmpty() ||
        eventType.isEmpty())
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
        return false;
    }

    QSqlQuery query(db);

    query.prepare(
        QStringLiteral(R"(
            SELECT id
            FROM smart_events
            WHERE device_path = :device_path
              AND event_type = :event_type
              AND active = 1
            ORDER BY id DESC
            LIMIT 1
        )"));

    query.bindValue(
        QStringLiteral(":device_path"),
        devicePath);

    query.bindValue(
        QStringLiteral(":event_type"),
        eventType);

    if (!query.exec())
    {
        qWarning()
        << "❌ Cannot search active SMART event for"
        << devicePath
        << eventType
        << ":"
        << query.lastError().text();

        return false;
    }

    if (query.next())
    {
        eventId =
            query.value(0).toLongLong();
    }

    return true;
}


// ============================================================
// INSERT
// ============================================================

bool SmartEventManager::insertEvent(
    const SmartEventRecord &event,
    qint64 *insertedId)
{
    if (insertedId)
        *insertedId = -1;

    if (!m_ready ||
        !isValidEventRecord(event))
    {
        qWarning()
        << "❌ SmartEventManager::insertEvent: invalid event";

        return false;
    }

    QSqlDatabase db =
        QSqlDatabase::database(
            m_connectionName,
            false);

    if (!db.isValid() ||
        !db.isOpen())
    {
        return false;
    }

    QSqlQuery query(db);

    query.prepare(
        QStringLiteral(R"(
            INSERT INTO smart_events (
                device_path,
                serial_number,
                model_name,
                event_type,
                severity,
                title,
                message,
                previous_value,
                current_value,
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
                :device_path,
                :serial_number,
                :model_name,
                :event_type,
                :severity,
                :title,
                :message,
                :previous_value,
                :current_value,
                1,
                :active,
                :acknowledged,
                strftime('%s','now'),
                strftime('%s','now'),
                CASE
                    WHEN :active_for_resolved = 1 THEN NULL
                    ELSE strftime('%s','now')
                END,
                strftime('%s','now'),
                strftime('%s','now')
            )
        )"));

    query.bindValue(
        QStringLiteral(":device_path"),
        event.devicePath);

    query.bindValue(
        QStringLiteral(":serial_number"),
        nonNullText(event.serialNumber));

    query.bindValue(
        QStringLiteral(":model_name"),
        nonNullText(event.modelName));

    query.bindValue(
        QStringLiteral(":event_type"),
        event.eventType);

    query.bindValue(
        QStringLiteral(":severity"),
        event.severity);

    query.bindValue(
        QStringLiteral(":title"),
        event.title);

    query.bindValue(
        QStringLiteral(":message"),
        nonNullText(event.message));

    query.bindValue(
        QStringLiteral(":previous_value"),
        static_cast<qulonglong>(
            event.previousValue));

    query.bindValue(
        QStringLiteral(":current_value"),
        static_cast<qulonglong>(
            event.currentValue));

    query.bindValue(
        QStringLiteral(":active"),
        event.active ? 1 : 0);

    query.bindValue(
        QStringLiteral(":active_for_resolved"),
        event.active ? 1 : 0);

    query.bindValue(
        QStringLiteral(":acknowledged"),
        event.acknowledged ? 1 : 0);

    if (!query.exec())
    {
        qWarning()
        << "❌ Cannot insert SMART event for"
        << event.devicePath
        << event.eventType
        << ":"
        << query.lastError().text();

        return false;
    }

    if (insertedId)
    {
        *insertedId =
            query.lastInsertId().toLongLong();
    }

    return true;
}


// ============================================================
// UPDATE EVENTO ATTIVO
// ============================================================

bool SmartEventManager::updateActiveEvent(
    qint64 eventId,
    const SmartEventRecord &event)
{
    if (!m_ready ||
        eventId < 0 ||
        !isValidEventRecord(event))
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
        return false;
    }

    QSqlQuery query(db);

    query.prepare(
        QStringLiteral(R"(
            UPDATE smart_events
            SET
                serial_number =
                    COALESCE(NULLIF(:serial_number, ''), serial_number),

                model_name =
                    COALESCE(NULLIF(:model_name, ''), model_name),

                severity = :severity,
                title = :title,
                message = :message,

                previous_value = :previous_value,
                current_value = :current_value,

                occurrence_count = occurrence_count + 1,

                active = 1,
                last_seen = strftime('%s','now'),
                resolved_at = NULL,
                updated_at = strftime('%s','now')

            WHERE id = :id
              AND active = 1
        )"));

    query.bindValue(
        QStringLiteral(":serial_number"),
        nonNullText(event.serialNumber));

    query.bindValue(
        QStringLiteral(":model_name"),
        nonNullText(event.modelName));

    query.bindValue(
        QStringLiteral(":severity"),
        event.severity);

    query.bindValue(
        QStringLiteral(":title"),
        event.title);

    query.bindValue(
        QStringLiteral(":message"),
        nonNullText(event.message));

    query.bindValue(
        QStringLiteral(":previous_value"),
        static_cast<qulonglong>(
            event.previousValue));

    query.bindValue(
        QStringLiteral(":current_value"),
        static_cast<qulonglong>(
            event.currentValue));

    query.bindValue(
        QStringLiteral(":id"),
        eventId);

    if (!query.exec())
    {
        qWarning()
        << "❌ Cannot update SMART event"
        << eventId
        << ":"
        << query.lastError().text();

        return false;
    }

    return true;
}


// ============================================================
// RECORD / UPDATE ACTIVE EVENT
// ============================================================

bool SmartEventManager::recordOrUpdateActiveEvent(
    const SmartEventRecord &event)
{
    if (!m_ready ||
        !isValidEventRecord(event))
    {
        return false;
    }

    qint64 eventId = -1;

    if (!findActiveEventId(
            event.devicePath,
            event.eventType,
            eventId))
    {
        return false;
    }

    if (eventId >= 0)
    {
        return updateActiveEvent(
            eventId,
            event);
    }

    SmartEventRecord activeEvent = event;
    activeEvent.active = true;

    return insertEvent(
        activeEvent,
        nullptr);
}

// ============================================================
// RESOLVE ACTIVE EVENT
// ============================================================

bool SmartEventManager::resolveActiveEvent(const QString &devicePath, const QString &eventType)
{
    if (!m_ready || devicePath.isEmpty() || eventType.isEmpty()) { return false; }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isValid() || !db.isOpen()) { return false; }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(R"(UPDATE smart_events SET active = 0, resolved_at = strftime('%s','now'), updated_at = strftime('%s','now') WHERE device_path = :device_path AND event_type = :event_type AND active = 1)"));

    query.bindValue(QStringLiteral(":device_path"), devicePath);
    query.bindValue(QStringLiteral(":event_type"), eventType);

    if (!query.exec()) {
        qWarning() << "❌ Cannot resolve SMART event for" << devicePath << eventType << ":" << query.lastError().text();
        return false;
    }

    /*
     * numRowsAffected() == 0 NON è un errore.
     * Il metodo è intenzionalmente idempotente.
     *
     * last_seen non viene modificato: rappresenta l'ultima volta
     * in cui la condizione era ancora presente.
     */
    return true;
}

// ============================================================
// RESOLVE EVENT BY ID
// ============================================================

bool SmartEventManager::resolveEvent(qint64 eventId)
{
    if (!m_ready || eventId <= 0) {
        qWarning() << "SmartEventManager::resolveEvent:" << "invalid event id" << eventId;
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isValid() || !db.isOpen()) {
        qWarning() << "Cannot resolve SMART event:" << "database unavailable";
        return false;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(R"(UPDATE smart_events SET active = 0, resolved_at = strftime('%s','now'), updated_at = strftime('%s','now') WHERE id = :id AND active = 1)"));
    query.bindValue(QStringLiteral(":id"), eventId);

    if (!query.exec()) {
        qWarning().noquote() << "Cannot resolve SMART event" << eventId << "-" << query.lastError().text();
        return false;
    }

    if (query.numRowsAffected() > 0) {
        qInfo() << "SMART event resolved:" << eventId;
        return true;
    }

    QSqlQuery checkQuery(db);
    checkQuery.prepare(QStringLiteral(R"(SELECT active FROM smart_events WHERE id = :id LIMIT 1)"));
    checkQuery.bindValue(QStringLiteral(":id"), eventId);

    if (!checkQuery.exec()) {
        qWarning().noquote() << "Cannot verify resolved SMART event" << eventId << "-" << checkQuery.lastError().text();
        return false;
    }

    if (!checkQuery.next()) {
        qWarning() << "SMART event not found:" << eventId;
        return false;
    }

    return checkQuery.value(0).toInt() == 0;
}

// ============================================================
// ACKNOWLEDGE EVENT
// ============================================================

bool SmartEventManager::acknowledgeEvent(qint64 eventId)
{
    if (!m_ready || eventId <= 0) {
        qWarning() << "❌ SmartEventManager::acknowledgeEvent: invalid event id" << eventId;
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isValid() || !db.isOpen()) { return false; }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(R"(UPDATE smart_events SET acknowledged = 1, updated_at = strftime('%s','now') WHERE id = :id)"));
    query.bindValue(QStringLiteral(":id"), eventId);

    if (!query.exec()) {
        qWarning() << "❌ Cannot acknowledge SMART event" << eventId << ":" << query.lastError().text();
        return false;
    }

    if (query.numRowsAffected() > 0) { return true; }

    /*
     * Se non risultano righe modificate, distinguiamo:
     *  - evento già acknowledged  -> true
     *  - id inesistente           -> false
     */
    QSqlQuery checkQuery(db);
    checkQuery.prepare(QStringLiteral(R"(SELECT acknowledged FROM smart_events WHERE id = :id LIMIT 1)"));
    checkQuery.bindValue(QStringLiteral(":id"), eventId);

    if (!checkQuery.exec()) {
        qWarning() << "❌ Cannot verify acknowledged SMART event" << eventId << ":" << checkQuery.lastError().text();
        return false;
    }

    if (!checkQuery.next()) {
        qWarning() << "❌ SMART event not found:" << eventId;
        return false;
    }

    return checkQuery.value(0).toInt() == 1;
}

// ============================================================
// CLEAR RESOLVED EVENTS
// ============================================================

bool SmartEventManager::clearResolvedEvents()
{
    if (!m_ready)
    {
        qWarning() << "SmartEventManager::clearResolvedEvents:" << "manager not ready";
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);

    if (!db.isValid() || !db.isOpen())
    {
        qWarning() << "Cannot clear SMART event history:" << "database unavailable";
        return false;
    }

    QSqlQuery query(db);

    if (!query.exec(QStringLiteral("DELETE FROM smart_events WHERE active = 0")))
    {
        qWarning().noquote() << "Cannot clear SMART event history:" << query.lastError().text();
        return false;
    }

    qInfo() << "SMART resolved event history cleared:" << query.numRowsAffected() << "row(s)";
    return true;
}