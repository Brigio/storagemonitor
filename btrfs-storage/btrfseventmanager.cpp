// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfseventmanager.h"

#include <QDebug>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QVariant>
#include <algorithm>


/*
 * ============================================================
 * SELECT COLUMNS
 * ============================================================
 *
 * IMPORTANTE:
 *
 * eventFromQuery() dipende esattamente da questo ordine:
 *
 *  0  id
 *  1  fs_uuid
 *  2  mount_point
 *  3  device_id
 *  4  device_path
 *  5  event_type
 *  6  severity
 *  7  title
 *  8  message
 *  9  previous_value
 * 10  current_value
 * 11  delta
 * 12  occurrence_count
 * 13  active
 * 14  acknowledged
 * 15  first_seen
 * 16  last_seen
 * 17  resolved_at
 */

static const QString eventSelectColumns()
{
    return QStringLiteral(
        "id, "
        "fs_uuid, "
        "mount_point, "
        "device_id, "
        "device_path, "
        "event_type, "
        "severity, "
        "title, "
        "message, "
        "previous_value, "
        "current_value, "
        "delta, "
        "occurrence_count, "
        "active, "
        "acknowledged, "
        "first_seen, "
        "last_seen, "
        "resolved_at"
        );
}

static const QString smartEventSelectColumns()
{
    return QStringLiteral(
        "id, "
        "device_path, "
        "serial_number, "
        "model_name, "
        "event_type, "
        "severity, "
        "title, "
        "message, "
        "previous_value, "
        "current_value, "
        "occurrence_count, "
        "active, "
        "acknowledged, "
        "first_seen, "
        "last_seen, "
        "resolved_at"
        );
}

/*
 * ============================================================
 * COSTRUTTORE
 * ============================================================
 */

BtrfsEventManager::BtrfsEventManager(QObject *parent) : QObject(parent)
{
    /*
     * Il timer NON interroga Btrfs.
     *
     * Controlla solamente se SQLite è stato modificato
     * dall'altra applicazione, storage-monitor-agent.
     */

    m_refreshTimer = new QTimer(this);

    m_refreshTimer->setInterval(2000);

    connect(m_refreshTimer, &QTimer::timeout, this, &BtrfsEventManager::checkForDatabaseChanges);

    /*
     * Manteniamo semplice l'utilizzo:
     *     new BtrfsEventManager(this)
     * tenta automaticamente di aprire il DB standard.
     */

    if (!openDatabase())
    {
        qWarning().noquote() << "BtrfsEventManager:" << "database not available";
    }
}


/*
 * ============================================================
 * DISTRUTTORE
 * ============================================================
 */

BtrfsEventManager::~BtrfsEventManager()
{
    closeDatabase();
}


/*
 * ============================================================
 * OPEN DATABASE
 * ============================================================
 */

bool BtrfsEventManager::openDatabase(const QString &databasePath)
{
    /*
     * Chiudi un'eventuale connessione precedente.
     */

    closeDatabase();

    const QFileInfo databaseInfo(databasePath);

    if (!databaseInfo.exists())
    {
        qWarning().noquote() << "Btrfs events database does not exist:" << databasePath;
        return false;
    }


    if (!databaseInfo.isFile())
    {
        qWarning().noquote() << "Btrfs events database path is not a file:" << databasePath;
        return false;
    }


    if (!databaseInfo.isReadable())
    {
        qWarning().noquote() << "Btrfs events database is not readable:" << databasePath;
        return false;
    }

    /*
     * Connessione privata e distinta dalle altre eventuali
     * connessioni SQLite utilizzate da btrfs-storage.
     */

    m_connectionName = QStringLiteral("btrfs-events-reader-%1").arg(reinterpret_cast<quintptr>(this), 0, 16);

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);

    m_db.setDatabaseName(databasePath);

    /*
     * Protezione principale:
     *
     * la GUI apre SQLite in sola lettura.
     *
     * QSQLITE_BUSY_TIMEOUT evita inoltre di fallire
     * immediatamente se l'agent sta completando una
     * transazione.
     */

    m_db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;"
            "QSQLITE_BUSY_TIMEOUT=3000")
        );


    if (!m_db.open())
    {
        qWarning().noquote() << "Cannot open Btrfs events database:" << m_db.lastError().text();

        const QString connectionName = m_connectionName;

        m_db = QSqlDatabase();

        QSqlDatabase::removeDatabase(connectionName);

        m_connectionName.clear();

        return false;
    }


    m_databasePath = databasePath;


    /*
     * Seconda protezione:
     *
     * anche se la connessione è già READ ONLY,
     * query_only documenta e rafforza l'intenzione
     * di questo componente.
     */

    {
        QSqlQuery query(m_db);

        if (!query.exec(QStringLiteral("PRAGMA query_only = ON")))
        {
            qWarning() << "Cannot enable SQLite query_only:" << query.lastError().text();
        }
    }

    /*
     * Memorizza la versione corrente del DB.
     */

    if (!readDataVersion(m_lastDataVersion))
    {
        m_lastDataVersion = -1;
    }

    /*
     * Costruisce la baseline.
     *
     * Gli eventi già presenti all'avvio NON devono essere
     * interpretati come nuovi eventi.
     */

    rebuildEventStateCache(false);

    m_refreshTimer->start();


    qInfo().noquote() << "BtrfsEventManager:" << "opened read-only database" << databasePath;


    if (!hasEventsTable())
    {
        qWarning().noquote() << "BtrfsEventManager:" << "table btrfs_events is not present yet";
    }

    //Non è un errore bloccante: il manager deve poter funzionare anche quando una delle due tabelle non esiste.
    if (!hasSmartEventsTable())
    {
        qWarning().noquote() << "BtrfsEventManager:" << "table smart_events is not present yet";
    }

    return true;
}


/*
 * ============================================================
 * CLOSE DATABASE
 * ============================================================
 */

void BtrfsEventManager::closeDatabase()
{
    if (m_refreshTimer)
    {
        m_refreshTimer->stop();
    }


    m_knownEventStates.clear();
    m_eventStateCacheInitialized = false;

    m_lastDataVersion = -1;


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
     * È necessario eliminare prima l'handle QSqlDatabase
     * e solamente dopo chiamare removeDatabase().
     */

    m_db =
        QSqlDatabase();


    if (!connectionName.isEmpty())
    {
        QSqlDatabase::removeDatabase(connectionName);
    }

    m_connectionName.clear();
    m_databasePath.clear();
}


/*
 * ============================================================
 * IS OPEN
 * ============================================================
 */

bool BtrfsEventManager::isOpen() const
{
    return m_db.isValid() && m_db.isOpen();
}


/*
 * ============================================================
 * DATABASE PATH
 * ============================================================
 */

QString BtrfsEventManager::databasePath() const
{
    return m_databasePath;
}


/*
 * ============================================================
 * HAS EVENTS TABLE
 * ============================================================
 */

bool BtrfsEventManager::hasEventsTable() const
{
    if (!isOpen())
        return false;


    QSqlQuery query(m_db);

    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'btrfs_events' LIMIT 1")
        );


    if (!query.exec())
    {
        qWarning() << "Cannot check btrfs_events table:" << query.lastError().text();
        return false;
    }

    return query.next();
}

bool BtrfsEventManager::hasSmartEventsTable() const
{
    if (!isOpen())
        return false;

    QSqlQuery query(m_db);

    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'smart_events' LIMIT 1")
        );

    if (!query.exec())
    {
        qWarning() << "Cannot check smart_events table:" << query.lastError().text();
        return false;
    }

    return query.next();
}

/*
 * ============================================================
 * ACTIVE EVENTS
 * ============================================================
 */

QList<BtrfsEvent>
BtrfsEventManager::activeEvents() const
{
    QList<BtrfsEvent> result;

    if (!isOpen())
        return result;

    // ============================================================
    // BTRFS EVENTS
    // ============================================================

    if (hasEventsTable())
    {
        QSqlQuery query(m_db);

        const QString sql = QStringLiteral("SELECT %1 FROM btrfs_events WHERE active = 1 ORDER BY last_seen DESC").arg(eventSelectColumns());

        if (!query.exec(sql))
        {
            qWarning() << "Cannot read active Btrfs events:" << query.lastError().text();
        }
        else
        {
            while (query.next())
            {
                result.append(eventFromQuery(query));
            }
        }
    }

    // ============================================================
    // SMART EVENTS
    // ============================================================

    if (hasSmartEventsTable())
    {
        QSqlQuery query(m_db);

        const QString sql = QStringLiteral("SELECT %1 FROM smart_events WHERE active = 1 ORDER BY last_seen DESC").arg(smartEventSelectColumns());

        if (!query.exec(sql))
        {
            qWarning() << "Cannot read active SMART events:" << query.lastError().text();
        }
        else
        {
            while (query.next())
            {
                result.append(smartEventFromQuery(query));
            }
        }
    }

    /*
     * Ordine globale:
     *
     * Critical
     * Warning
     * Info
     *
     * e, a parità di severità,
     * evento più recente prima.
     */
    std::sort(result.begin(), result.end(), [](const BtrfsEvent &left, const BtrfsEvent &right)
        {
            const int leftSeverity = static_cast<int>(left.severity);

            const int rightSeverity = static_cast<int>(right.severity);

            if (leftSeverity != rightSeverity)
            {
                return leftSeverity > rightSeverity;
            }

            return left.lastSeen > right.lastSeen;
        });

    return result;
}

/*
 * ============================================================
 * RECENT EVENTS
 * ============================================================
 */

QList<BtrfsEvent>
BtrfsEventManager::recentEvents(int maxEvents) const
{
    QList<BtrfsEvent> result;

    if (!isOpen())
        return result;

    // ============================================================
    // BTRFS EVENTS
    // ============================================================

    if (hasEventsTable())
    {
        QSqlQuery query(m_db);

        if (maxEvents > 0)
        {
            query.prepare(QStringLiteral("SELECT %1 FROM btrfs_events ORDER BY last_seen DESC LIMIT :limit").arg(eventSelectColumns()));

            query.bindValue(QStringLiteral(":limit"), maxEvents);
        }
        else
        {
            query.prepare(QStringLiteral("SELECT %1 FROM btrfs_events ORDER BY last_seen DESC").arg(eventSelectColumns()));
        }

        if (!query.exec())
        {
            qWarning() << "Cannot read Btrfs event history:" << query.lastError().text();
        }
        else
        {
            while (query.next())
            {
                result.append(eventFromQuery(query));
            }
        }
    }

    // ============================================================
    // SMART EVENTS
    // ============================================================

    if (hasSmartEventsTable())
    {
        QSqlQuery query(m_db);

        if (maxEvents > 0)
        {
            query.prepare(QStringLiteral("SELECT %1 FROM smart_events ORDER BY last_seen DESC LIMIT :limit").arg(smartEventSelectColumns()));

            query.bindValue(QStringLiteral(":limit"), maxEvents);
        }
        else
        {
            query.prepare(QStringLiteral("SELECT %1 FROM smart_events ORDER BY last_seen DESC").arg(smartEventSelectColumns()));
        }

        if (!query.exec())
        {
            qWarning() << "Cannot read SMART event history:" << query.lastError().text();
        }
        else
        {
            while (query.next())
            {
                result.append(smartEventFromQuery(query));
            }
        }
    }

    // ============================================================
    // ORDINE GLOBALE
    // ============================================================

    std::sort(result.begin(), result.end(), [](const BtrfsEvent &left, const BtrfsEvent &right)
        {
            return left.lastSeen >
                   right.lastSeen;
        });

    /*
     * Il LIMIT SQL è applicato per sorgente.
     *
     * Dopo il merge applichiamo il limite
     * globale richiesto dalla GUI.
     */
    if (maxEvents > 0 &&
        result.size() > maxEvents)
    {
        result.resize(maxEvents);
    }

    return result;
}


/*
 * ============================================================
 * EVENTS FOR FILESYSTEM
 * ============================================================
 */

QList<BtrfsEvent>
BtrfsEventManager::eventsForFilesystem(const QString &fsUuid, int maxEvents) const
{
    QList<BtrfsEvent> result;


    if (!isOpen() ||
        !hasEventsTable() ||
        fsUuid.trimmed().isEmpty())
    {
        return result;
    }


    QSqlQuery query(m_db);


    if (maxEvents > 0)
    {
        query.prepare(
            QStringLiteral(
                "SELECT %1 "
                "FROM btrfs_events "
                "WHERE fs_uuid = :fs_uuid "
                "ORDER BY last_seen DESC "
                "LIMIT :limit"
                )
                .arg(eventSelectColumns())
            );


        query.bindValue(
            QStringLiteral(":limit"),
            maxEvents);
    }
    else
    {
        query.prepare(
            QStringLiteral(
                "SELECT %1 "
                "FROM btrfs_events "
                "WHERE fs_uuid = :fs_uuid "
                "ORDER BY last_seen DESC"
                )
                .arg(eventSelectColumns())
            );
    }


    query.bindValue(
        QStringLiteral(":fs_uuid"),
        fsUuid.trimmed());


    if (!query.exec())
    {
        qWarning()
        << "Cannot read filesystem Btrfs events:"
        << query.lastError().text();

        return result;
    }


    while (query.next())
    {
        result.append(
            eventFromQuery(query));
    }


    return result;
}


/*
 * ============================================================
 * EVENT FROM QUERY
 * ============================================================
 */

BtrfsEvent
BtrfsEventManager::eventFromQuery(const QSqlQuery &query) const
{
    BtrfsEvent event;

    event.source = StorageEventSource::Btrfs;

    event.id = query.value(0).toULongLong();

    event.fsUuid = query.value(1).toString();

    event.mountPoint = query.value(2).toString();

    event.deviceId = query.value(3).toString();

    event.devicePath = query.value(4).toString();

    event.eventTypeKey = normalizeDatabaseKey(query.value(5).toString());

    event.type = eventTypeFromDatabase(event.eventTypeKey);

    event.severity = severityFromDatabase(query.value(6).toString());

    event.title = query.value(7).toString();

    event.message = query.value(8).toString();

    event.previousValue = query.value(9).toULongLong();

    event.currentValue = query.value(10).toULongLong();

    event.delta = query.value(11).toULongLong();

    event.occurrenceCount = query.value(12).toULongLong();

    event.active = query.value(13).toInt() != 0;

    event.acknowledged = query.value(14).toInt() != 0;

    const qint64 firstSeen = query.value(15).toLongLong();

    if (firstSeen > 0)
    {
        event.firstSeen = QDateTime::fromSecsSinceEpoch(firstSeen);
    }

    const qint64 lastSeen = query.value(16).toLongLong();

    if (lastSeen > 0)
    {
        event.lastSeen = QDateTime::fromSecsSinceEpoch(lastSeen);
    }


    /*
     * resolved_at può essere NULL.
     */

    if (!query.value(17).isNull())
    {
        const qint64 resolvedAt = query.value(17).toLongLong();

        if (resolvedAt > 0)
        {
            event.resolvedAt = QDateTime::fromSecsSinceEpoch(resolvedAt);
        }
    }


    return event;
}

BtrfsEvent
BtrfsEventManager::smartEventFromQuery(const QSqlQuery &query) const
{
    BtrfsEvent event;

    event.source = StorageEventSource::Smart;

    event.id = query.value(0).toULongLong();

    event.devicePath = query.value(1).toString();

    event.serialNumber = query.value(2).toString();

    event.modelName = query.value(3).toString();

    event.eventTypeKey = normalizeDatabaseKey(query.value(4).toString());

    /*
     * BtrfsEventType è intenzionalmente
     * Btrfs-specifico.
     */
    event.type = BtrfsEventType::Unknown;

    event.severity = severityFromDatabase(query.value(5).toString());

    event.title = query.value(6).toString();

    event.message = query.value(7).toString();

    event.previousValue = query.value(8).toULongLong();

    event.currentValue = query.value(9).toULongLong();

    /*
     * smart_events non possiede una colonna delta.
     * Non inventiamo il valore.
     */
    event.delta = 0;

    event.occurrenceCount = query.value(10).toULongLong();

    event.active = query.value(11).toInt() != 0;

    event.acknowledged = query.value(12).toInt() != 0;

    const qint64 firstSeen = query.value(13).toLongLong();

    if (firstSeen > 0)
    {
        event.firstSeen = QDateTime::fromSecsSinceEpoch(firstSeen);
    }

    const qint64 lastSeen = query.value(14).toLongLong();

    if (lastSeen > 0)
    {
        event.lastSeen = QDateTime::fromSecsSinceEpoch(lastSeen);
    }

    if (!query.value(15).isNull())
    {
        const qint64 resolvedAt = query.value(15).toLongLong();

        if (resolvedAt > 0)
        {
            event.resolvedAt = QDateTime::fromSecsSinceEpoch(resolvedAt);
        }
    }

    return event;
}

/*
 * ============================================================
 * EVENT BY ID
 * ============================================================
 */

bool BtrfsEventManager::eventById(quint64 eventId, BtrfsEvent &event) const
{
    if (!isOpen() || !hasEventsTable())
    {
        return false;
    }

    QSqlQuery query(m_db);

    query.prepare(QStringLiteral("SELECT %1 FROM btrfs_events WHERE id = :id LIMIT 1").arg(eventSelectColumns()));


    query.bindValue(QStringLiteral(":id"), QVariant::fromValue<qulonglong>(eventId));

    if (!query.exec())
    {
        qWarning() << "Cannot read Btrfs event:" << query.lastError().text();
        return false;
    }

    if (!query.next())
    {
        return false;
    }

    event = eventFromQuery(query);

    return true;
}


/*
 * ============================================================
 * SQLITE DATA VERSION
 * ============================================================
 */

bool BtrfsEventManager::readDataVersion(
    qint64 &version) const
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
 * REBUILD EVENT STATE CACHE
 * ============================================================
 */

void BtrfsEventManager::rebuildEventStateCache(
    bool emitSignals)
{
    if (!isOpen() ||
        !hasEventsTable())
    {
        if (emitSignals &&
            !m_knownEventStates.isEmpty())
        {
            m_knownEventStates.clear();

            emit eventsChanged();
        }

        return;
    }


    QHash<quint64, bool> currentStates;


    QSqlQuery query(m_db);


    if (!query.exec(
            QStringLiteral(
                "SELECT id, active "
                "FROM btrfs_events")))
    {
        qWarning()
        << "Cannot rebuild Btrfs event cache:"
        << query.lastError().text();

        return;
    }


    while (query.next())
    {
        const quint64 eventId =
            query.value(0).toULongLong();

        const bool active =
            query.value(1).toInt() != 0;


        currentStates.insert(
            eventId,
            active);
    }


    /*
     * Primo caricamento:
     *
     * tutto ciò che esiste già rappresenta la baseline.
     *
     * Non emettiamo "eventCreated" per centinaia di righe
     * storiche appena si apre btrfs-storage.
     */

    if (!m_eventStateCacheInitialized)
    {
        m_knownEventStates =
            currentStates;

        m_eventStateCacheInitialized =
            true;

        return;
    }


    if (emitSignals)
    {
        /*
         * Nuove righe.
         */

        for (auto it = currentStates.constBegin();
             it != currentStates.constEnd();
             ++it)
        {
            if (!m_knownEventStates.contains(
                    it.key()))
            {
                BtrfsEvent event;

                if (eventById(
                        it.key(),
                        event))
                {
                    emit eventCreated(
                        event);
                }

                continue;
            }


            /*
             * Evento:
             *
             * active = 1
             *       ↓
             * active = 0
             */

            const bool wasActive =
                m_knownEventStates.value(
                    it.key());

            const bool isActive =
                it.value();


            if (wasActive &&
                !isActive)
            {
                BtrfsEvent event;

                if (eventById(
                        it.key(),
                        event))
                {
                    emit eventResolved(
                        event);
                }
            }
        }
    }


    m_knownEventStates =
        currentStates;


    if (emitSignals)
    {
        /*
         * Viene emesso anche per:
         *
         * occurrence_count cambiato
         * last_seen cambiato
         * severity cambiata
         * ACK modificato dall'agent
         * riga eliminata
         * ecc.
         */

        emit eventsChanged();
    }
}


/*
 * ============================================================
 * CHECK DATABASE CHANGES
 * ============================================================
 */

void BtrfsEventManager::checkForDatabaseChanges()
{
    if (!isOpen())
        return;


    qint64 currentVersion = -1;


    if (!readDataVersion(
            currentVersion))
    {
        return;
    }


    /*
     * Prima lettura.
     */

    if (m_lastDataVersion < 0)
    {
        m_lastDataVersion =
            currentVersion;

        rebuildEventStateCache(
            false);

        return;
    }


    /*
     * Nessun commit effettuato da altre connessioni SQLite.
     */

    if (currentVersion ==
        m_lastDataVersion)
    {
        return;
    }


    m_lastDataVersion =
        currentVersion;


    /*
     * Qualcosa è cambiato nel DB.
     */

    rebuildEventStateCache(
        true);
}


/*
 * ============================================================
 * MANUAL REFRESH
 * ============================================================
 */

void BtrfsEventManager::refresh()
{
    if (!isOpen())
        return;


    qint64 version = -1;


    if (readDataVersion(
            version))
    {
        m_lastDataVersion =
            version;
    }


    rebuildEventStateCache(
        true);
}


/*
 * ============================================================
 * READ-ONLY COMPATIBILITY: ACKNOWLEDGE
 * ============================================================
 */

bool BtrfsEventManager::acknowledgeEvent(
    quint64 eventId)
{
    Q_UNUSED(eventId);


    qWarning().noquote()
        << "BtrfsEventManager::acknowledgeEvent():"
        << "btrfs-storage opens the database read-only."
        << "ACK must be delegated to storage-monitor-agent.";


    return false;
}


/*
 * ============================================================
 * READ-ONLY COMPATIBILITY: RESOLVE
 * ============================================================
 */

bool BtrfsEventManager::resolveEvent(
    quint64 eventId)
{
    Q_UNUSED(eventId);


    qWarning().noquote()
        << "BtrfsEventManager::resolveEvent():"
        << "btrfs-storage opens the database read-only."
        << "Resolve must be delegated to storage-monitor-agent.";


    return false;
}


/*
 * ============================================================
 * READ-ONLY COMPATIBILITY: CLEAR HISTORY
 * ============================================================
 */

void BtrfsEventManager::clearHistory()
{
    qWarning().noquote()
    << "BtrfsEventManager::clearHistory():"
    << "btrfs-storage opens the database read-only."
    << "History modification must be delegated"
    << "to storage-monitor-agent.";
}


/*
 * ============================================================
 * NORMALIZE DATABASE KEY
 * ============================================================
 */

QString BtrfsEventManager::normalizeDatabaseKey(
    const QString &value)
{
    QString key =
        value.trimmed().toLower();


    key.replace(
        QChar(' '),
        QChar('_'));

    key.replace(
        QChar('-'),
        QChar('_'));


    return key;
}


/*
 * ============================================================
 * EVENT TYPE FROM DATABASE
 * ============================================================
 */

BtrfsEventType
BtrfsEventManager::eventTypeFromDatabase(
    const QString &value)
{
    const QString key =
        normalizeDatabaseKey(value);


    if (key == QStringLiteral("device_added"))
        return BtrfsEventType::DeviceAdded;

    if (key == QStringLiteral("device_removed"))
        return BtrfsEventType::DeviceRemoved;

    if (key == QStringLiteral("device_missing"))
        return BtrfsEventType::DeviceMissing;

    if (key == QStringLiteral("device_recovered"))
        return BtrfsEventType::DeviceRecovered;


    if (key == QStringLiteral("read_error"))
        return BtrfsEventType::ReadError;

    if (key == QStringLiteral("write_error"))
        return BtrfsEventType::WriteError;

    if (key == QStringLiteral("flush_error"))
        return BtrfsEventType::FlushError;

    if (key == QStringLiteral("corruption_error"))
        return BtrfsEventType::CorruptionError;

    if (key == QStringLiteral("generation_error"))
        return BtrfsEventType::GenerationError;


    if (key == QStringLiteral("filesystem_recovered"))
        return BtrfsEventType::FilesystemRecovered;


    if (key == QStringLiteral("scrub_started"))
        return BtrfsEventType::ScrubStarted;

    if (key == QStringLiteral("scrub_completed"))
        return BtrfsEventType::ScrubCompleted;

    if (key == QStringLiteral("scrub_error"))
        return BtrfsEventType::ScrubError;

    if (key == QStringLiteral("scrub_cancelled") ||
        key == QStringLiteral("scrub_canceled"))
    {
        return BtrfsEventType::ScrubCancelled;
    }


    if (key == QStringLiteral("balance_started"))
        return BtrfsEventType::BalanceStarted;

    if (key == QStringLiteral("balance_completed"))
        return BtrfsEventType::BalanceCompleted;

    if (key == QStringLiteral("balance_cancelled") ||
        key == QStringLiteral("balance_canceled"))
    {
        return BtrfsEventType::BalanceCancelled;
    }

    if (key == QStringLiteral("balance_paused"))
        return BtrfsEventType::BalancePaused;

    if (key == QStringLiteral("balance_resumed"))
        return BtrfsEventType::BalanceResumed;


    /*
     * Compatibilità eventuale con un vecchio DB che abbia
     * salvato il valore numerico dell'enum.
     */

    bool ok = false;

    const int numericValue =
        key.toInt(&ok);


    if (ok)
    {
        switch (numericValue)
        {
        case 1:
            return BtrfsEventType::DeviceAdded;

        case 2:
            return BtrfsEventType::DeviceRemoved;

        case 3:
            return BtrfsEventType::DeviceMissing;

        case 4:
            return BtrfsEventType::ReadError;

        case 5:
            return BtrfsEventType::WriteError;

        case 6:
            return BtrfsEventType::FlushError;

        case 7:
            return BtrfsEventType::CorruptionError;

        case 8:
            return BtrfsEventType::GenerationError;

        case 9:
            return BtrfsEventType::FilesystemRecovered;

        case 10:
            return BtrfsEventType::ScrubStarted;

        case 11:
            return BtrfsEventType::ScrubCompleted;

        case 12:
            return BtrfsEventType::ScrubError;

        case 13:
            return BtrfsEventType::BalanceStarted;

        case 14:
            return BtrfsEventType::BalanceCompleted;

        default:
            break;
        }
    }


    return BtrfsEventType::Unknown;
}


/*
 * ============================================================
 * SEVERITY FROM DATABASE
 * ============================================================
 */

BtrfsEventSeverity
BtrfsEventManager::severityFromDatabase(
    const QString &value)
{
    const QString key =
        normalizeDatabaseKey(value);


    if (key == QStringLiteral("warning") ||
        key == QStringLiteral("warn") ||
        key == QStringLiteral("1"))
    {
        return BtrfsEventSeverity::Warning;
    }


    if (key == QStringLiteral("critical") ||
        key == QStringLiteral("error") ||
        key == QStringLiteral("2"))
    {
        return BtrfsEventSeverity::Critical;
    }


    return BtrfsEventSeverity::Info;
}


/*
 * ============================================================
 * EVENT TYPE TO STRING
 * ============================================================
 */

QString BtrfsEventManager::eventTypeToString(
    BtrfsEventType type)
{
    switch (type)
    {
    case BtrfsEventType::DeviceAdded:
        return QStringLiteral(
            "Device Added");

    case BtrfsEventType::DeviceRemoved:
        return QStringLiteral(
            "Device Removed");

    case BtrfsEventType::DeviceMissing:
        return QStringLiteral(
            "Device Missing");

    case BtrfsEventType::DeviceRecovered:
        return QStringLiteral(
            "Device Recovered");


    case BtrfsEventType::ReadError:
        return QStringLiteral(
            "Read Error");

    case BtrfsEventType::WriteError:
        return QStringLiteral(
            "Write Error");

    case BtrfsEventType::FlushError:
        return QStringLiteral(
            "Flush Error");

    case BtrfsEventType::CorruptionError:
        return QStringLiteral(
            "Corruption Error");

    case BtrfsEventType::GenerationError:
        return QStringLiteral(
            "Generation Error");


    case BtrfsEventType::FilesystemRecovered:
        return QStringLiteral(
            "Filesystem Recovered");


    case BtrfsEventType::ScrubStarted:
        return QStringLiteral(
            "Scrub Started");

    case BtrfsEventType::ScrubCompleted:
        return QStringLiteral(
            "Scrub Completed");

    case BtrfsEventType::ScrubError:
        return QStringLiteral(
            "Scrub Error");

    case BtrfsEventType::ScrubCancelled:
        return QStringLiteral(
            "Scrub Cancelled");


    case BtrfsEventType::BalanceStarted:
        return QStringLiteral(
            "Balance Started");

    case BtrfsEventType::BalanceCompleted:
        return QStringLiteral(
            "Balance Completed");

    case BtrfsEventType::BalanceCancelled:
        return QStringLiteral(
            "Balance Cancelled");

    case BtrfsEventType::BalancePaused:
        return QStringLiteral(
            "Balance Paused");

    case BtrfsEventType::BalanceResumed:
        return QStringLiteral(
            "Balance Resumed");


    case BtrfsEventType::Unknown:
    default:
        return QStringLiteral(
            "Unknown");
    }
}


/*
 * ============================================================
 * SEVERITY TO STRING
 * ============================================================
 */

QString BtrfsEventManager::severityToString(
    BtrfsEventSeverity severity)
{
    switch (severity)
    {
    case BtrfsEventSeverity::Info:
        return QStringLiteral(
            "INFO");

    case BtrfsEventSeverity::Warning:
        return QStringLiteral(
            "WARNING");

    case BtrfsEventSeverity::Critical:
        return QStringLiteral(
            "CRITICAL");
    }


    return QStringLiteral(
        "INFO");
}