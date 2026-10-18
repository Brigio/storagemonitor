// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef BTRFSEVENTMANAGER_H
#define BTRFSEVENTMANAGER_H

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QList>
#include <QHash>
#include <QSqlDatabase>

class QSqlQuery;
class QTimer;


enum class StorageEventSource
{
    Btrfs = 0,
    Smart
};

/*
 * ============================================================
 * BtrfsEventSeverity
 * ============================================================
 */

enum class BtrfsEventSeverity
{
    Info = 0,
    Warning,
    Critical
};


/*
 * ============================================================
 * BtrfsEventType
 * ============================================================
 *
 * I valori originari vengono mantenuti nello stesso ordine
 * per ridurre al minimo le modifiche alla GUI esistente.
 *
 * Nel database NON utilizziamo questi valori numerici come
 * formato persistente: l'agent salva chiavi testuali.
 */

enum class BtrfsEventType
{
    Unknown = 0,

    DeviceAdded,
    DeviceRemoved,
    DeviceMissing,

    ReadError,
    WriteError,
    FlushError,
    CorruptionError,
    GenerationError,

    FilesystemRecovered,

    ScrubStarted,
    ScrubCompleted,
    ScrubError,

    BalanceStarted,
    BalanceCompleted,

    /*
     * Estensioni aggiunte in fondo per non modificare
     * i valori degli enum precedenti.
     */
    DeviceRecovered,

    ScrubCancelled,

    BalanceCancelled,
    BalancePaused,
    BalanceResumed
};


/*
 * ============================================================
 * BtrfsEvent
 * ============================================================
 *
 * Rappresentazione GUI di una riga delle tabelle
 * btrfs_events. btrfs_smart
 */

struct BtrfsEvent
{
    StorageEventSource source = StorageEventSource::Btrfs;

    quint64 id = 0;

    /*
     * Chiave testuale originale del database.
     * Esempi:
     *   read_error
     *   balance_started
     *   smart_warning
     *   smart_critical
     */
    QString eventTypeKey;

    // ============================================================
    // BTRFS
    // ============================================================

    QString fsUuid;
    QString mountPoint;
    QString deviceId;

    // ============================================================
    // COMUNE BTRFS / SMART
    // ============================================================

    QString devicePath;

    // ============================================================
    // SMART
    // ============================================================

    QString serialNumber;
    QString modelName;

    // ============================================================
    // TIPO / SEVERITA'
    // ============================================================

    BtrfsEventType type = BtrfsEventType::Unknown;

    BtrfsEventSeverity severity = BtrfsEventSeverity::Info;

    QString title;
    QString message;

    QDateTime firstSeen;
    QDateTime lastSeen;

    QDateTime resolvedAt;

    quint64 previousValue = 0;
    quint64 currentValue = 0;
    quint64 delta = 0;

    quint64 occurrenceCount = 1;

    bool active = false;
    bool acknowledged = false;
};


/*
 * ============================================================
 * BtrfsEventManager
 * ============================================================
 *
 * btrfs-storage:
 *
 *     SOLO LETTURA
 *
 * Gli eventi vengono prodotti e persistiti da:
 *
 *     storage-monitor-agent
 *
 * Questa classe legge:
 *
 *     /var/lib/storage-monitor-agent/btrfs.db
 *
 * e mantiene quanto più possibile compatibile l'interfaccia
 * utilizzata dalla schermata eventi esistente.
 */

class BtrfsEventManager : public QObject
{
    Q_OBJECT

public:

    explicit BtrfsEventManager(QObject *parent = nullptr);

    ~BtrfsEventManager() override;


    /*
     * ========================================================
     * DATABASE
     * ========================================================
     */

    bool openDatabase(const QString &databasePath = QStringLiteral("/var/lib/storage-monitor-agent/btrfs.db"));

    void closeDatabase();

    bool isOpen() const;

    QString databasePath() const;

    bool hasEventsTable() const;

    bool hasSmartEventsTable() const;

    /*
     * ========================================================
     * CONSULTAZIONE
     * ========================================================
     *
     * Questi due metodi mantengono la stessa API del vecchio
     * BtrfsEventManager.
     */

    QList<BtrfsEvent> activeEvents() const;

    QList<BtrfsEvent> recentEvents(
        int maxEvents = 100) const;


    /*
     * Utile per mostrare gli eventi relativi ad un singolo
     * filesystem.
     */

    QList<BtrfsEvent> eventsForFilesystem(
        const QString &fsUuid,
        int maxEvents = 100) const;


    /*
     * ========================================================
     * REFRESH
     * ========================================================
     *
     * Forza il controllo del database.
     *
     * Normalmente non è necessario chiamarlo manualmente:
     * il manager controlla periodicamente PRAGMA data_version.
     */

    void refresh();


    /*
     * ========================================================
     * COMPATIBILITA' TEMPORANEA
     * ========================================================
     *
     * Questi metodi appartenevano al vecchio manager.
     *
     * Vengono mantenuti temporaneamente per evitare errori
     * di compilazione nella GUI esistente, ma NON modificano
     * il database.
     *
     * In futuro ACK / Resolve potranno essere inviati
     * all'agent tramite D-Bus.
     */

    bool acknowledgeEvent(
        quint64 eventId);

    bool resolveEvent(
        quint64 eventId);

    void clearHistory();


    /*
     * ========================================================
     * UTILITY
     * ========================================================
     */

    static QString eventTypeToString(
        BtrfsEventType type);

    static QString severityToString(
        BtrfsEventSeverity severity);


signals:

    /*
     * Conservati per compatibilità con la schermata esistente.
     *
     * Vengono generati quando vengono rilevate modifiche
     * effettuate dall'agent sul database.
     */

    void eventCreated(
        const BtrfsEvent &event);

    void eventResolved(
        const BtrfsEvent &event);

    void eventsChanged();


private slots:

    void checkForDatabaseChanges();


private:

    /*
     * ========================================================
     * QUERY / CONVERSIONE
     * ========================================================
     */

    BtrfsEvent eventFromQuery(const QSqlQuery &query) const;

    BtrfsEvent smartEventFromQuery(const QSqlQuery &query) const;

    bool eventById(quint64 eventId, BtrfsEvent &event) const;


    /*
     * ========================================================
     * DATABASE CHANGE TRACKING
     * ========================================================
     */

    bool readDataVersion(
        qint64 &version) const;

    void rebuildEventStateCache(
        bool emitSignals);


    /*
     * ========================================================
     * CONVERSIONE DATI DB
     * ========================================================
     */

    static QString normalizeDatabaseKey(
        const QString &value);

    static BtrfsEventType eventTypeFromDatabase(
        const QString &value);

    static BtrfsEventSeverity severityFromDatabase(
        const QString &value);


private:

    QSqlDatabase m_db;

    QString m_connectionName;
    QString m_databasePath;

    QTimer *m_refreshTimer = nullptr;

    qint64 m_lastDataVersion = -1;

    /*
     * id evento -> active
     *
     * Serve solamente per rilevare:
     *
     * nuova riga         -> eventCreated()
     * active 1 -> 0      -> eventResolved()
     */
    QHash<quint64, bool> m_knownEventStates;

    bool m_eventStateCacheInitialized = false;
};

#endif // BTRFSEVENTMANAGER_H
