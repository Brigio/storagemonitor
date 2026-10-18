// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SMARTEVENTMANAGER_H
#define SMARTEVENTMANAGER_H

#include <QObject>
#include <QString>
#include <QtGlobal>

class QSqlDatabase;


/*
 * ============================================================
 * SMART EVENT RECORD
 * ============================================================
 *
 * Rappresenta un evento SMART da persistere in smart_events.
 *
 * eventType deve essere una chiave stabile, per esempio:
 *
 *   smart_warning
 *   smart_critical
 */
struct SmartEventRecord
{
    QString devicePath;

    QString serialNumber;
    QString modelName;

    QString eventType;
    QString severity;

    QString title;
    QString message;

    quint64 previousValue = 0;
    quint64 currentValue = 0;

    bool active = true;
    bool acknowledged = false;
};


class SmartEventManager : public QObject
{
    Q_OBJECT

public:
    explicit SmartEventManager(
        const QSqlDatabase &database,
        QObject *parent = nullptr);

    ~SmartEventManager() override = default;

    bool isReady() const;

    /*
     * Inserisce un nuovo evento.
     *
     * Normalmente SmartMonitor userà recordOrUpdateActiveEvent(),
     * ma questo metodo resta disponibile anche per eventuali eventi
     * storici/non persistenti.
     */
    bool insertEvent(
        const SmartEventRecord &event,
        qint64 *insertedId = nullptr);

    /*
     * Condizione persistente.
     *
     * Chiave logica:
     *
     *   device_path + event_type + active = 1
     *
     * Se non esiste un evento attivo viene eseguito INSERT.
     * Se esiste viene eseguito UPDATE e occurrence_count aumenta.
     */
    bool recordOrUpdateActiveEvent(const SmartEventRecord &event);

    /*
     * Risolve tutti gli eventi attivi corrispondenti.
     *
     * È intenzionalmente idempotente: se non esiste alcun evento
     * attivo, il metodo restituisce comunque true.
     */
    bool resolveActiveEvent(const QString &devicePath, const QString &eventType);

    /*
    * Risolve un singolo evento SMART tramite id.
    *
    * È idempotente:
    * un evento già risolto resta risolto.
    *
    * Restituisce false se l'id non esiste
    * o se la query fallisce.
    */
    bool resolveEvent(qint64 eventId);

    /*
     * Segna come acknowledged un evento SMART tramite il suo id.
     *
     * Funziona sia per eventi attivi sia per eventi già risolti.
     * È idempotente: un evento già acknowledged resta tale.
     *
     * Restituisce false se l'id non esiste o se la query fallisce.
     */
    bool acknowledgeEvent(qint64 eventId);

    /*
    * Elimina dallo storico soltanto gli eventi risolti.
    *
    * Gli eventi active = 1 vengono preservati.
    *
    * È idempotente:
    * se non esistono eventi risolti restituisce true.
    */
    bool clearResolvedEvents();

private:
    bool ensureSchema();

    bool findActiveEventId(
        const QString &devicePath,
        const QString &eventType,
        qint64 &eventId) const;

    bool updateActiveEvent(
        qint64 eventId,
        const SmartEventRecord &event);

private:
    /*
     * Conserviamo soltanto il nome della connessione SQLite.
     *
     * Non manteniamo un secondo QSqlDatabase come membro, così
     * SmartEventManager non ostacola QSqlDatabase::removeDatabase()
     * quando BtrfsMonitor chiude la connessione principale.
     */
    QString m_connectionName;
    bool m_ready = false;
};

#endif // SMARTEVENTMANAGER_H
