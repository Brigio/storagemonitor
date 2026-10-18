// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef BTRFSEVENTMANAGER_H
#define BTRFSEVENTMANAGER_H

#include <QObject>
#include <QString>
#include <QtGlobal>


class QSqlDatabase;


/*
 * ============================================================
 * RECORD EVENTO BTRFS
 * ============================================================
 *
 * Struttura utilizzata da BtrfsMonitor per passare un evento
 * al BtrfsEventManager.
 *
 * Il manager è l'unico responsabile della persistenza nella
 * tabella btrfs_events.
 */
struct BtrfsEventRecord
{
    QString fsUuid;
    QString mountPoint;

    QString deviceId;
    QString devicePath;

    /*
     * Chiave persistente.
     *
     * Esempi:
     *
     * raid_imbalance
     * read_error
     * write_error
     * device_missing
     * scrub_started
     * balance_started
     */
    QString eventType;

    /*
     * info
     * warning
     * critical
     */
    QString severity;

    QString title;
    QString message;

    quint64 previousValue = 0;
    quint64 currentValue = 0;

    bool active = true;
    bool acknowledged = false;
};


/*
 * ============================================================
 * BTRFS EVENT MANAGER
 * ============================================================
 */
class BtrfsEventManager : public QObject
{
    Q_OBJECT

public:

    explicit BtrfsEventManager(const QSqlDatabase &database, QObject *parent = nullptr);

    ~BtrfsEventManager() override = default;


    /*
     * Il manager è utilizzabile solo se:
     *
     * - la connessione DB esiste
     * - la connessione è aperta
     * - la tabella btrfs_events è disponibile
     */
    bool isReady() const;


    /*
     * Inserisce un nuovo evento.
     *
     * Usabile anche per eventi storici/non persistenti.
     */
    bool insertEvent(const BtrfsEventRecord &event, qint64 *insertedId = nullptr);


    /*
     * Condizione persistente.
     *
     * Se non esiste un evento attivo con:
     *
     * fs_uuid + device_id + event_type
     *
     * viene eseguito INSERT.
     *
     * Se esiste già:
     *
     * viene eseguito UPDATE.
     */
    bool recordOrUpdateActiveEvent(const BtrfsEventRecord &event);


    /*
     * Risolve tutti gli eventi attivi corrispondenti.
     *
     * È intenzionalmente idempotente:
     * se non esiste alcun evento attivo restituisce true.
     */
    bool resolveActiveEvent(const QString &fsUuid, const QString &deviceId, const QString &eventType);

    /*Risolve una singola riga di btrfs_events tramite id.
     *È idempotente: un evento già risolto resta risolto.
     *Restituisce false se l'id non esiste o se la query fallisce.*/
    bool resolveEvent(qint64 eventId);

    /*Segna come acknowledged una riga di btrfs_events
     *tramite il suo id. È idempotente: un evento già acknowledged resta tale.
     * Restituisce false se l'id non esiste o se la query fallisce.*/
    bool acknowledgeEvent(qint64 eventId);

    /*
    * Elimina dallo storico soltanto gli eventi risolti.
    * Gli eventi active = 1 vengono preservati
    * È idempotente:
    * se non esistono eventi risolti restituisce true.
    */
    bool clearResolvedEvents();

private:

    /*
     * Crea tabella e indici se mancanti.
     */
    bool ensureSchema();


    /*
     * Cerca l'evento attivo.
     *
     * true  = query eseguita correttamente
     * false = errore SQL
     *
     * eventId:
     *   >= 0 evento trovato
     *   -1   nessun evento trovato
     */
    bool findActiveEventId(const QString &fsUuid, const QString &deviceId, const QString &eventType, qint64 &eventId) const;


    /*
     * Aggiorna un evento già attivo.
     */
    bool updateActiveEvent(qint64 eventId, const BtrfsEventRecord &event);


private:

    /*
     * Conserviamo solo il nome della connessione.
     *
     * NON conserviamo un QSqlDatabase come membro.
     *
     * Recuperiamo l'handle della connessione solo localmente
     * quando dobbiamo eseguire una query.
     */
    QString m_connectionName;

    bool m_ready = false;
};


#endif // BTRFSEVENTMANAGER_H
