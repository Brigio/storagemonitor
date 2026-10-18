// SPDX-License-Identifier: GPL-3.0-or-later

#include "agentdbusservice.h"
#include "btrfsmonitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>
#include <limits>
#include <QFile>
#include <sys/stat.h>
#include <QThread>

static QProcessEnvironment commandEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));

    environment.insert(QStringLiteral("LANG"), QStringLiteral("C"));

    return environment;
}

static quint64 scrubValueToBytes(double value, const QString &unit)
{
    const QString normalized = unit.trimmed().toUpper();

    quint64 multiplier = 1;

    if (normalized == QStringLiteral("KIB") || normalized == QStringLiteral("KB"))
    {
        multiplier = 1024ULL;
    }
    else if (normalized == QStringLiteral("MIB") || normalized == QStringLiteral("MB"))
    {
        multiplier = 1024ULL * 1024ULL;
    }
    else if (normalized == QStringLiteral("GIB") || normalized == QStringLiteral("GB"))
    {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    }
    else if (normalized == QStringLiteral("TIB") || normalized == QStringLiteral("TB"))
    {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    }

    return static_cast<quint64>(value * static_cast<double>(multiplier));
}

AgentDbusService::AgentDbusService(BtrfsMonitor *monitor,QObject *parent) : QObject(parent), m_monitor(monitor)
{
    m_btrfsPath = QStandardPaths::findExecutable(QStringLiteral("btrfs"));

    if (m_btrfsPath.isEmpty())
    {
        m_btrfsPath = QStringLiteral("/usr/bin/btrfs");
    }

    m_pkcheckPath = QStandardPaths::findExecutable(QStringLiteral("pkcheck"));

    if (m_pkcheckPath.isEmpty())
    {
        m_pkcheckPath = QStringLiteral("/usr/bin/pkcheck");
    }

    // ============================================================
    // START PROCESS
    // ============================================================

    m_startProcess = new QProcess(this);

    m_startProcess->setProcessEnvironment(commandEnvironment());

    connect(m_startProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AgentDbusService::onStartFinished);

    // ============================================================
    // CANCEL PROCESS
    // ============================================================

    m_cancelProcess = new QProcess(this);

    m_cancelProcess->setProcessEnvironment(commandEnvironment());

    connect(m_cancelProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AgentDbusService::onCancelFinished);

    // ============================================================
    // REMOVE DEVICE PROCESS
    // ============================================================

    m_removeProcess = new QProcess(this);
    m_removeProcess->setProcessEnvironment(commandEnvironment());
    connect(m_removeProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AgentDbusService::onRemoveFinished);

    // ============================================================
    // REMOVE DEVICE PROGRESS TIMER
    // ============================================================

    m_removeProgressTimer = new QTimer(this);
    m_removeProgressTimer->setInterval(5000);
    connect(m_removeProgressTimer, &QTimer::timeout, this, &AgentDbusService::pollRemoveProgress);

    // ============================================================
    // REPLACE START PROCESS
    // ============================================================
    m_replaceStartProcess = new QProcess(this);
    m_replaceStartProcess->setProcessEnvironment(commandEnvironment());
    connect(m_replaceStartProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AgentDbusService::onReplaceStartFinished);

    // ============================================================
    // REPLACE CANCEL PROCESS
    // ============================================================
    m_replaceCancelProcess = new QProcess(this);
    m_replaceCancelProcess->setProcessEnvironment(commandEnvironment());
    connect(m_replaceCancelProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AgentDbusService::onReplaceCancelFinished);

    // ============================================================
    // REPLACE STATUS TIMER
    // ============================================================
    m_replaceTimer = new QTimer(this);
    m_replaceTimer->setInterval(2000);
    connect(m_replaceTimer, &QTimer::timeout, this, &AgentDbusService::pollReplace);


    // ============================================================
    // SCRUB START PROCESS
    // ============================================================

    m_scrubStartProcess = new QProcess(this);

    m_scrubStartProcess->setProcessEnvironment(commandEnvironment());

    connect(m_scrubStartProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AgentDbusService::onScrubStartFinished);

    // ============================================================
    // SCRUB CANCEL PROCESS
    // ============================================================

    m_scrubCancelProcess = new QProcess(this);

    m_scrubCancelProcess->setProcessEnvironment(commandEnvironment());

    connect(m_scrubCancelProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AgentDbusService::onScrubCancelFinished);

    // ============================================================
    // SCRUB STATUS TIMER
    // ============================================================

    m_scrubTimer = new QTimer(this);

    m_scrubTimer->setInterval(2000);

    connect(m_scrubTimer, &QTimer::timeout, this, &AgentDbusService::pollScrub);

    // ============================================================
    // BALANCE STATUS TIMER
    // ============================================================

    m_balanceTimer = new QTimer(this);

    m_balanceTimer->setInterval(2000);

    connect(m_balanceTimer, &QTimer::timeout, this, &AgentDbusService::pollBalance);

    // ============================================================
    // AUTOMATIC SCRUB SCHEDULER
    // ============================================================

    m_autoScrubTimer = new QTimer(this);

    /*
 * Un unico scheduler globale.
 *
 * Le scadenze sono dell'ordine di settimane:
 * un controllo ogni 30 minuti è più che sufficiente
 * e non richiede timer systemd per filesystem.
 * 30  min x 60 sec x 1000 millisec = 30 min
 */
    constexpr int autoScrubCheckMinutes = 30;
    m_autoScrubTimer->setInterval(autoScrubCheckMinutes * 60 * 1000);

    connect(m_autoScrubTimer, &QTimer::timeout, this, &AgentDbusService::checkAutoScrubSchedule);

    m_autoScrubTimer->start();

    /*
 * Primo controllo poco dopo l'avvio.
 *
 * BtrfsMonitor esegue già una scansione iniziale
 * tramite singleShot(0); 30 secondi lasciano il tempo
 * di popolare lo stato corrente dei filesystem.
 */
    QTimer::singleShot(30000, this, &AgentDbusService::checkAutoScrubSchedule);

    qInfo() << "Automatic scrub scheduler started;" << "check interval:" << autoScrubCheckMinutes << "minute(s)";
}


bool AgentDbusService::validateMountPoint(const QString &mountPoint) const
{
    if (mountPoint.isEmpty())
        return false;

    if (!QDir::isAbsolutePath(mountPoint))
        return false;

    const QFileInfo info(mountPoint);

    return info.exists() && info.isDir();
}

bool AgentDbusService::validateDevicePath(const QString &devicePath) const
{
    const QString path = devicePath.trimmed();

    if (path.isEmpty()) return false;
    if (!QDir::isAbsolutePath(path)) return false;
    if (!path.startsWith(QStringLiteral("/dev/"))) return false;

    const QByteArray encodedPath = QFile::encodeName(path);
    struct stat deviceStat {};

    if (::stat(encodedPath.constData(), &deviceStat) != 0) return false;

    return S_ISBLK(deviceStat.st_mode);
}

bool AgentDbusService::validateBtrfsDeviceId(qulonglong deviceId) const
{
    return deviceId > 0;
}

bool AgentDbusService::automaticScrubBlocked() const
{
    // ============================================================
    // SCRUB
    // ============================================================
    if (!m_activeScrubMount.isEmpty() || !m_pendingScrubStartMount.isEmpty() || !m_pendingScrubCancelMount.isEmpty()) return true;
    if (m_scrubStartProcess && m_scrubStartProcess->state() != QProcess::NotRunning) return true;
    if (m_scrubCancelProcess && m_scrubCancelProcess->state() != QProcess::NotRunning) return true;

    // ============================================================
    // BALANCE
    // ============================================================
    if (!m_activeBalanceMount.isEmpty() || !m_pendingStartMount.isEmpty() || !m_pendingCancelMount.isEmpty()) return true;
    if (m_startProcess && m_startProcess->state() != QProcess::NotRunning) return true;
    if (m_cancelProcess && m_cancelProcess->state() != QProcess::NotRunning) return true;

    // ============================================================
    // DEVICE REMOVE
    // ============================================================
    if (!m_pendingRemoveMount.isEmpty()) return true;
    if (m_removeProcess && m_removeProcess->state() != QProcess::NotRunning) return true;

    // ============================================================
    // DEVICE REPLACE
    // ============================================================
    if (!m_activeReplaceMount.isEmpty() || !m_pendingReplaceStartMount.isEmpty() || !m_pendingReplaceCancelMount.isEmpty()) return true;
    if (m_replaceStartProcess && m_replaceStartProcess->state() != QProcess::NotRunning) return true;
    if (m_replaceCancelProcess && m_replaceCancelProcess->state() != QProcess::NotRunning) return true;

    return false;
}

bool AgentDbusService::reattachRunningReplace()
{
    if (!m_monitor)
        return false;

    const QStringList mountPoints = m_monitor->currentBtrfsMountPoints();

    for (const QString &mountPoint : mountPoints)
    {
        const QString path = mountPoint.trimmed();

        if (!validateMountPoint(path))
            continue;

        const ReplaceRuntimeInfo info = queryReplaceState(path);

        if (info.state != QStringLiteral("running"))
        {
            continue;
        }

        m_activeReplaceMount = path;
        m_lastReplaceState = QStringLiteral("running");
        m_lastReplacePercent = info.completedPercent;
        m_replaceCancelRequested = false;
        m_replaceStatusErrorCount = 0;

        if (m_replaceTimer && !m_replaceTimer->isActive())
        {
            m_replaceTimer->start();
        }

        emit ReplaceStateChanged(path, QStringLiteral("running"), info.details);

        if (info.completedPercent >= 0)
        {
            emit ReplaceProgressChanged(path, info.completedPercent);
        }

        qInfo().noquote() << "Reattached to existing Btrfs replace:" << path << "progress:" << info.completedPercent << "%";

        return true;
    }

    return false;
}

bool AgentDbusService::reattachRunningScrub()
{
    if (!m_monitor) return false;

    const QStringList mountPoints = m_monitor->currentBtrfsMountPoints();

    for (const QString &mountPoint : mountPoints) {
        const QString path = mountPoint.trimmed();
        if (!validateMountPoint(path)) continue;

        const ScrubRuntimeInfo info = queryScrubState(path);
        if (info.state != QStringLiteral("running")) continue;

        m_activeScrubMount = path;
        m_lastScrubState = QStringLiteral("running");
        m_lastScrubPercent = info.completedPercent;
        m_scrubCancelRequested = false;
        m_scrubStatusErrorCount = 0;

        if (m_scrubTimer && !m_scrubTimer->isActive()) m_scrubTimer->start();

        emit ScrubStateChanged(path, QStringLiteral("running"), info.details);

        if (info.completedPercent >= 0) {
            emit ScrubProgressChanged(path, info.completedPercent, info.processedBytes, info.totalBytes, info.speedBytesPerSec, info.errorCount);
        }

        qInfo().noquote() << "Automatic scrub scheduler reattached" << "to existing scrub:" << path;
        return true;
    }

    return false;
}

void AgentDbusService::checkAutoScrubSchedule()
{
    if (!m_monitor) {
        qWarning() << "Automatic scrub scheduler:" << "monitor unavailable";
        return;
    }

    /*
    * Dopo un restart del daemon lo stato RAM del replace
    * è vuoto, ma Btrfs può avere ancora un replace attivo.
    *
    * Va riconosciuto PRIMA di interrogare lo scrub:
    * device replace utilizza internamente il motore scrub.
    */
    if (reattachRunningReplace())
    {
        qInfo() << "Automatic scrub scheduler:" << "existing device replace detected; deferred";
        return;
    }

    /* Se una manutenzione gestita dal daemon è già in corso, rimandiamo semplicemente al prossimo controllo. */
    if (automaticScrubBlocked()) {
        qDebug() << "Automatic scrub scheduler:" << "maintenance already active; deferred";
        return;
    }

    const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();
    QStringList dueMountPoints;

    if (!m_monitor->dueAutoScrubMountPoints(nowEpoch, dueMountPoints)) {
        qWarning() << "Automatic scrub scheduler:" << "cannot calculate due filesystems";
        return;
    }

    if (dueMountPoints.isEmpty()) return;

    /* Importante dopo un restart del daemon: potremmo non avere ancora m_activeScrubMount, mentre lo scrub transient continua a girare. */
    if (reattachRunningScrub()) {
        qInfo() << "Automatic scrub scheduler:" << "existing scrub detected; deferred";
        return;
    }

    /* La lista è ordinata dalla scadenza più vecchia. Avviamo un solo scrub alla volta. */
    const QString mountPoint = dueMountPoints.constFirst();

    // ============================================================
    // BALANCE REALE SUL FILESYSTEM CANDIDATO
    // ============================================================
    QString balanceDetails;
    const QString balanceState = queryBalanceState(mountPoint, balanceDetails);

    if (balanceState == QStringLiteral("running") || balanceState == QStringLiteral("paused")) {
        qInfo().noquote() << "Automatic scrub deferred:" << mountPoint << "balance is" << balanceState;
        return;
    }

    /* Se non riusciamo neppure a verificare il balance, scegliamo la strada conservativa e non avviamo una manutenzione automatica. */
    if (balanceState == QStringLiteral("error")) {
        qWarning().noquote() << "Automatic scrub deferred:" << mountPoint << "cannot determine balance state:" << balanceDetails;
        return;
    }

    qInfo().noquote() << "Automatic scrub is due:" << mountPoint;

    if (!startScrubAuthorized(mountPoint)) { qWarning().noquote() << "Automatic scrub could not be started:" << mountPoint; return; }

    // ============================================================
    // PERSISTENT RETRY / BACKOFF
    // ============================================================

    constexpr qint64 autoScrubRetrySeconds = 24LL * 60LL * 60LL;
    const qint64 retryAfterEpoch = nowEpoch + autoScrubRetrySeconds;

    /* Il backoff viene scritto solo dopo che il percorso di avvio dello scrub è stato accettato. Se lo scrub termina con successo, la nuova normale scadenza settimanale sarà comunque più lontana e quindi prevarrà automaticamente. Se invece viene cancellato/interrotto/fallisce, retryAfterEpoch impedisce un nuovo tentativo immediato. */
    if (!m_monitor->setAutoScrubRetryAfterForMountPoint(mountPoint, retryAfterEpoch)) { qWarning().noquote() << "Automatic scrub started but retry/backoff could not be persisted:" << mountPoint; }

    qInfo().noquote() << "Automatic scrub start requested:" << mountPoint << "retry/backoff until:" << retryAfterEpoch;

}

void AgentDbusService::authorizeAndRun(const QString &actionId, const QString &mountPoint, AuthorizedOperation operation)
{
    if (!calledFromDBus())
    {
        qWarning() << "Authorization requested outside D-Bus context";
        return;
    }

    const QDBusMessage request = message();

    const QDBusConnection replyConnection = connection();

    const QString caller = request.service();

    /*
     * La risposta a StartBalance/CancelBalance arriverà
     * dopo che Polkit avrà completato l'autorizzazione.
     */
    setDelayedReply(true);

    if (caller.isEmpty())
    {
        replyConnection.send(request.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"), QStringLiteral("Cannot determine D-Bus caller")));
        return;
    }

    const QFileInfo pkcheckInfo(
        m_pkcheckPath);

    if (!pkcheckInfo.exists() || !pkcheckInfo.isExecutable())
    {
        qCritical().noquote() << "pkcheck not available:" << m_pkcheckPath;

        replyConnection.send(request.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"),QStringLiteral("Polkit pkcheck is not available")));

        return;
    }

    auto *authProcess = new QProcess(this);

    QStringList arguments;

    arguments
        << QStringLiteral("--action-id")
        << actionId
        << QStringLiteral("--system-bus-name")
        << caller;

    /*
     * Rispettiamo il flag D-Bus:
     * il chiamante deve dichiarare che accetta
     * un'autenticazione interattiva.
     */
    if (request.isInteractiveAuthorizationAllowed())
    {
        arguments
            << QStringLiteral(
                   "--allow-user-interaction");
    }

    connect(
        authProcess,
        qOverload<int, QProcess::ExitStatus>(
            &QProcess::finished),
        this,
        [this,
         authProcess,
         request,
         replyConnection,
         actionId,
         mountPoint,
         operation]
        (int exitCode,
         QProcess::ExitStatus exitStatus)
        {
            const QString standardError =
                QString::fromUtf8(
                    authProcess
                        ->readAllStandardError())
                    .trimmed();

            authProcess->deleteLater();

            const bool authorized =
                exitStatus ==
                    QProcess::NormalExit &&
                exitCode == 0;

            if (!authorized)
            {
                qWarning().noquote()
                << "Polkit authorization denied:"
                << actionId
                << "mount:"
                << mountPoint
                << "exit:"
                << exitCode
                << standardError;

                QString errorMessage =
                    standardError;

                if (errorMessage.isEmpty())
                {
                    errorMessage =
                        QStringLiteral(
                            "Authorization denied");
                }

                replyConnection.send(
                    request.createErrorReply(
                        QStringLiteral(
                            "org.freedesktop.DBus.Error.AccessDenied"),
                        errorMessage));

                return;
            }

            qInfo().noquote() << "Polkit authorization granted:" << actionId << "mount:" << mountPoint;

            const bool result = operation();

            replyConnection.send(request.createReply(QVariant(result)));
        });

    authProcess->start(
        m_pkcheckPath,
        arguments);

    /*
     * Aspettiamo solamente che pkcheck venga avviato.
     * NON aspettiamo l'autenticazione.
     */
    if (!authProcess->waitForStarted(3000))
    {
        const QString error = authProcess->errorString();

        qWarning().noquote() << "Cannot start pkcheck:" << error;

        authProcess->deleteLater();

        replyConnection.send(request.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"), error));
    }
}

bool AgentDbusService::StartBalance(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        emit BalanceStateChanged(path, QStringLiteral("error"), QStringLiteral("Invalid mount point"));

        return false;
    }

    if (!calledFromDBus())
    {
        qWarning() << "StartBalance called outside D-Bus";

        return false;
    }

    authorizeAndRun(
        QStringLiteral("org.storage_monitor.agent.balance.start"),
        path,
        [this, path]()
        {
            return startBalanceAuthorized(path);
        });

    /*
     * Ignorato da QtDBus perché authorizeAndRun()
     * ha impostato delayed reply.
     */
    return false;
}

bool AgentDbusService::AddDevice(const QString &mountPoint, const QString &devicePath)
{
    const QString path = mountPoint.trimmed();
    const QString device = devicePath.trimmed();

    if (!validateMountPoint(path)) { qWarning() << "AddDevice: invalid mount point" << path; return false; }
    if (!validateDevicePath(device)) { qWarning() << "AddDevice: invalid device path" << device; return false; }
    if (!calledFromDBus()) { qWarning() << "AddDevice called outside D-Bus"; return false; }

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.device.add"), path, [this, path, device]() { return addDeviceAuthorized(path, device); });

    /* La vera risposta D-Bus viene inviata da authorizeAndRun() dopo Polkit. */
    return false;
}

bool AgentDbusService::RemoveDevice(const QString &mountPoint, qulonglong deviceId)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) {
        qWarning() << "RemoveDevice: invalid mount point" << path;
        return false;
    }

    if (!validateBtrfsDeviceId(deviceId)) {
        qWarning() << "RemoveDevice: invalid Btrfs device id" << deviceId;
        return false;
    }

    if (!calledFromDBus()) {
        qWarning() << "RemoveDevice called outside D-Bus";
        return false;
    }

    if (!m_monitor) {
        qWarning() << "RemoveDevice: monitor unavailable";
        return false;
    }

    bool present = true;

    if (!m_monitor->btrfsDevicePresence(path, deviceId, present)) {
        qWarning() << "RemoveDevice:" << "device is not a member of filesystem" << path << "devid" << deviceId;
        return false;
    }

    if (present) {
        qWarning() << "RemoveDevice:" << "device is still present" << path << "devid" << deviceId;
        return false;
    }

    qInfo().noquote() << "RemoveDevice validation passed:" << "mount:" << path << "devid:" << deviceId;

    /* Per prova Operazione intenzionalmente non implementata in questo checkpoint. Nessun comando Btrfs viene eseguito. */
    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.device.remove"), path, [this, path, deviceId]()
        {
            return removeDeviceAuthorized(path, deviceId);
        });

    /*
 * La vera risposta D-Bus viene inviata
 * da authorizeAndRun() dopo Polkit.
 */
    return false;authorizeAndRun(QStringLiteral("org.storage_monitor.agent.device.remove"), path, [this, path, deviceId]()
        {
            return removeDeviceAuthorized(path, deviceId);
        });

    /*
 * La vera risposta D-Bus viene inviata
 * da authorizeAndRun() dopo Polkit.
 */
    return false;
}

bool AgentDbusService::AcknowledgeEvent(const QString &source, qulonglong eventId)
{
    const QString normalizedSource = source.trimmed().toLower();

    if (normalizedSource != QStringLiteral("btrfs") && normalizedSource != QStringLiteral("smart"))
    {
        qWarning() << "AcknowledgeEvent:" << "invalid source" << source;
        return false;
    }

    if (eventId == 0 || eventId > static_cast<qulonglong>(std::numeric_limits<qint64>::max()))
    {
        qWarning() << "AcknowledgeEvent:" << "invalid event id" << eventId;
        return false;
    }

    if (!m_monitor)
    {
        qWarning() << "AcknowledgeEvent:" << "monitor unavailable";
        return false;
    }

    if (!calledFromDBus())
    {
        qWarning() << "AcknowledgeEvent called" << "outside D-Bus";
        return false;
    }

    const qint64 signedEventId = static_cast<qint64>(eventId);

    const QString authorizationContext = QStringLiteral("%1:%2").arg(normalizedSource).arg(eventId);

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.events.acknowledge"), authorizationContext, [this, normalizedSource, signedEventId]()
        {
            return m_monitor->acknowledgeEvent(normalizedSource, signedEventId);
        });

    /*
     * La vera risposta D-Bus viene inviata
     * da authorizeAndRun() dopo Polkit.
     */
    return false;
}

bool AgentDbusService::ResolveEvent(const QString &source, qulonglong eventId)
{
    const QString normalizedSource = source.trimmed().toLower();

    if (normalizedSource != QStringLiteral("btrfs") && normalizedSource != QStringLiteral("smart"))
    {
        qWarning() << "ResolveEvent:" << "invalid source" << source;
        return false;
    }

    if (eventId == 0 || eventId > static_cast<qulonglong>(std::numeric_limits<qint64>::max()))
    {
        qWarning() << "ResolveEvent:" << "invalid event id" << eventId;
        return false;
    }

    if (!m_monitor)
    {
        qWarning() << "ResolveEvent:" << "monitor unavailable";
        return false;
    }

    if (!calledFromDBus())
    {
        qWarning() << "ResolveEvent called" << "outside D-Bus";
        return false;
    }

    const qint64 signedEventId = static_cast<qint64>(eventId);
    const QString authorizationContext = QStringLiteral("%1:%2").arg(normalizedSource).arg(eventId);

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.events.resolve"), authorizationContext, [this, normalizedSource, signedEventId]() {
        return m_monitor->resolveEvent(normalizedSource, signedEventId);
    });

    /*
     * La vera risposta D-Bus viene inviata
     * da authorizeAndRun() dopo Polkit.
     */
    return false;
}

bool AgentDbusService::ClearEventHistory()
{
    if (!m_monitor)
    {
        qWarning() << "ClearEventHistory:" << "monitor unavailable";
        return false;
    }

    if (!calledFromDBus())
    {
        qWarning() << "ClearEventHistory called" << "outside D-Bus";
        return false;
    }

    const QString authorizationContext = QStringLiteral("event-history");

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.events.clear"), authorizationContext, [this]()
        {
            return m_monitor->clearEventHistory();
        });

    /*
     * La vera risposta D-Bus viene inviata
     * da authorizeAndRun() dopo Polkit.
     */
    return false;
}

bool AgentDbusService::startBalanceAuthorized(const QString &mountPoint)
{


    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        emit BalanceStateChanged(path, QStringLiteral("error"), QStringLiteral("Invalid mount point"));
        return false;
    }

    if (m_startProcess->state() != QProcess::NotRunning)
    {
        qWarning() << "Balance start command already running";
        return false;
    }

    if (m_cancelProcess->state() != QProcess::NotRunning)
    {
        qWarning() << "Balance cancel command is running";
        return false;
    }

    // ============================================================
    // DEVICE REPLACE CONFLICT
    // ============================================================

    if (!m_pendingReplaceStartMount.isEmpty() || !m_pendingReplaceCancelMount.isEmpty() || (m_replaceStartProcess && m_replaceStartProcess->state() != QProcess::NotRunning) || (m_replaceCancelProcess && m_replaceCancelProcess->state() != QProcess::NotRunning)) {
        qWarning() << "Balance blocked:" << "device replace command is active";
        return false;
    }

    const ReplaceRuntimeInfo replaceInfo = queryReplaceState(path);

    if (replaceInfo.state == QStringLiteral("running")) {
        qWarning().noquote() << "Balance blocked:" << "device replace is running on" << path;
        return false;
    }

    if (replaceInfo.state == QStringLiteral("error")) {
        qWarning().noquote() << "Balance blocked:" << "cannot determine replace state:" << replaceInfo.details;
        return false;
    }

    // ============================================================
    // CHECK EXISTING BALANCE
    // ============================================================

    QString currentDetails;

    const QString currentState = queryBalanceState(path, currentDetails);

    if (currentState == QStringLiteral("running"))
    {
        m_activeBalanceMount = path;
        m_lastBalanceState = QStringLiteral("running");

        m_lastBalancePercent = -1;

        if (!m_balanceTimer->isActive())
            m_balanceTimer->start();

        //Estrarre la percentuale corrente del balance
        updateBalanceProgressFromDetails(path, currentDetails);

        emit BalanceStateChanged(path, QStringLiteral("running"), currentDetails);

        return false;
    }


    // ============================================================
    // START BALANCE
    // ============================================================

    m_pendingStartMount = path;
    m_cancelRequested = false;

    m_startProcess->setProgram(m_btrfsPath);

    m_startProcess->setArguments({
        QStringLiteral("balance"),
        QStringLiteral("start"),
        QStringLiteral("--background"),
        QStringLiteral("--full-balance"),
        path
    });

    m_startProcess->start();

    if (!m_startProcess->waitForStarted(3000))
    {
        const QString error =
            m_startProcess->errorString();

        qWarning().noquote()
            << "Cannot start Btrfs balance command:"
            << error;

        emit BalanceStateChanged(
            path,
            QStringLiteral("error"),
            error);

        m_pendingStartMount.clear();

        return false;
    }

    qInfo().noquote()
        << "Btrfs balance requested:"
        << path;

    emit BalanceStateChanged(
        path,
        QStringLiteral("starting"),
        QStringLiteral(
            "Balance start requested"));

    return true;
}


void AgentDbusService::onStartFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString mountPoint = m_pendingStartMount;

    const QString standardOutput = QString::fromUtf8(m_startProcess->readAllStandardOutput()).trimmed();

    const QString standardError =
        QString::fromUtf8(
            m_startProcess
                ->readAllStandardError())
            .trimmed();

    m_pendingStartMount.clear();


    if (exitStatus != QProcess::NormalExit || exitCode != 0)
    {
        QString error = standardError;

        if (error.isEmpty())
            error = standardOutput;

        if (error.isEmpty())
            error =
                QStringLiteral(
                    "btrfs balance start failed");

        qWarning().noquote()
            << "Btrfs balance start failed:"
            << error;

        emit BalanceStateChanged(
            mountPoint,
            QStringLiteral("error"),
            error);

        return;
    }


    // ============================================================
    // BALANCE STARTED
    // ============================================================

    m_activeBalanceMount = mountPoint;

    m_lastBalanceState = QStringLiteral("running");

    m_lastBalancePercent = -1;

    qInfo().noquote() << "Btrfs balance started:" << mountPoint;

    if (!m_balanceTimer->isActive())
        m_balanceTimer->start();

    QTimer::singleShot(500, this, [this, mountPoint, standardOutput]()
        {
            refreshMonitor();

            emit BalanceStateChanged(
                mountPoint,
                QStringLiteral("running"),
                standardOutput.isEmpty()
                    ? QStringLiteral("Balance is running")
                    : standardOutput);
        });
}

bool AgentDbusService::prepareDeviceForBtrfs(const QString &devicePath, QString &preparedDevicePath, QString &errorMessage) const
{
    preparedDevicePath.clear();
    errorMessage.clear();

    const QString device = devicePath.trimmed();

    if (!validateDevicePath(device))
    {
        errorMessage = QStringLiteral("Invalid block device: %1").arg(device);
        return false;
    }

    // ============================================================
    // UTILITY PER ESEGUIRE COMANDI
    // ============================================================

    auto runCommand =
        [](const QString &program, const QStringList &arguments, const QByteArray &standardInput,
           QString &standardOutput, QString &standardError, int timeoutMs) -> bool
    {
        QProcess process;

        process.setProcessEnvironment(commandEnvironment());
        process.start(program, arguments);

        if (!process.waitForStarted(3000))
        {
            standardError = process.errorString();
            return false;
        }

        if (!standardInput.isEmpty())
        {
            process.write(standardInput);
            process.closeWriteChannel();
        }

        if (!process.waitForFinished(timeoutMs))
        {
            process.kill();
            process.waitForFinished();

            standardError = QStringLiteral("Command timed out");
            return false;
        }

        standardOutput = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        standardError  = QString::fromUtf8(process.readAllStandardError()).trimmed();

        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };

    // ============================================================
    // PROGRAMMI NECESSARI
    // ============================================================

    const QString lsblkPath = QStandardPaths::findExecutable(QStringLiteral("lsblk"));

    if (lsblkPath.isEmpty())
    {
        errorMessage = QStringLiteral("lsblk is not available");
        return false;
    }

    // ============================================================
    // DETERMINA IL TIPO DEL DEVICE
    // ============================================================

    QString output;
    QString error;

    if (!runCommand(lsblkPath, {QStringLiteral("-ndo"), QStringLiteral("TYPE"), device},
                    {}, output, error, 5000))
    {
        errorMessage = QStringLiteral("Cannot determine device type: %1").arg(error);
        return false;
    }

    const QString deviceType = output.trimmed();

    // ============================================================
    // PARTIZIONE GIA' ESISTENTE
    //
    // Non dobbiamo creare un'altra partition table.
    // ============================================================

    if (deviceType == QStringLiteral("part"))
    {
        preparedDevicePath = device;
        return true;
    }

    // ============================================================
    // L'AUTO-PARTIZIONAMENTO E' CONSENTITO SOLO SU UN DISCO
    // FISICO INTERO.
    // ============================================================

    if (deviceType != QStringLiteral("disk"))
    {
        errorMessage = QStringLiteral("Unsupported block device type '%1' for %2").arg(deviceType, device);
        return false;
    }

    // ============================================================
    // VERIFICA CHE IL DISCO NON ABBIA PARTIZIONI
    // ============================================================

    output.clear();
    error.clear();

    if (!runCommand(lsblkPath, {QStringLiteral("-nrpo"), QStringLiteral("NAME,TYPE"), device},
                    {}, output, error, 5000))
    {
        errorMessage = QStringLiteral("Cannot inspect device layout: %1").arg(error);
        return false;
    }

    const QStringList layoutLines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    /*
     * Un disco realmente vuoto deve produrre
     * una sola riga:
     *
     * /dev/sdd disk
     */
    if (layoutLines.size() != 1)
    {
        errorMessage = QStringLiteral("Device %1 already contains partitions").arg(device);
        return false;
    }

    // ============================================================
    // VERIFICA CHE NON SIA MONTATO
    // ============================================================

    output.clear();
    error.clear();

    if (!runCommand(lsblkPath, {QStringLiteral("-ndo"), QStringLiteral("MOUNTPOINTS"), device},
                    {}, output, error, 5000))
    {
        errorMessage = QStringLiteral("Cannot inspect device mount state: %1").arg(error);
        return false;
    }

    if (!output.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Device %1 is mounted").arg(device);
        return false;
    }

    // ============================================================
    // VERIFICA FIRME ESISTENTI
    // ============================================================

    const QString wipefsPath = QStandardPaths::findExecutable(QStringLiteral("wipefs"));

    if (wipefsPath.isEmpty())
    {
        errorMessage = QStringLiteral("wipefs is not available");
        return false;
    }

    output.clear();
    error.clear();

    if (!runCommand(wipefsPath, {QStringLiteral("-n"), device},
                    {}, output, error, 5000))
    {
        errorMessage = QStringLiteral("Cannot inspect device signatures: %1").arg(error);
        return false;
    }

    if (!output.trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Device %1 contains an existing signature").arg(device);
        return false;
    }

    // ============================================================
    // CREA GPT + UNA PARTIZIONE CHE OCCUPA LO SPAZIO DISPONIBILE
    // ============================================================

    const QString sfdiskPath = QStandardPaths::findExecutable(QStringLiteral("sfdisk"));

    if (sfdiskPath.isEmpty())
    {
        errorMessage = QStringLiteral("sfdisk is not available");
        return false;
    }

    /*
     * Nessun label-id, UUID o geometria viene copiata
     * da un altro disco.
     *
     * sfdisk sceglie:
     * - start correttamente allineato
     * - nuova GPT
     * - nuova partition UUID
     * - tutto lo spazio disponibile
     * - Linux filesystem partition type
     */
    const QByteArray partitionScript = QByteArrayLiteral("label: gpt\n"
                                                         "size=+, type=L\n");

    output.clear();
    error.clear();

    if (!runCommand(sfdiskPath, {QStringLiteral("--lock"), device}, partitionScript, output, error, 30000))
    {
        errorMessage = QStringLiteral("Cannot create partition table on %1: %2").arg(device, error);
        return false;
    }

    qInfo().noquote() << "Created GPT partition table on" << device;

    // ============================================================
    // CHIEDI AL KERNEL DI RILEGGERE LA TABELLA
    // ============================================================

    const QString partprobePath = QStandardPaths::findExecutable(QStringLiteral("partprobe"));

    if (!partprobePath.isEmpty())
    {
        output.clear();
        error.clear();

        if (!runCommand(partprobePath, {device}, {}, output, error, 10000))
        {
            errorMessage = QStringLiteral("partprobe failed for %1: %2").arg(device, error);
            return false;
        }
    }
    else
    {
        /*
         * Fallback per sistemi dove GNU parted
         * non è installato.
         */
        const QString blockdevPath = QStandardPaths::findExecutable(QStringLiteral("blockdev"));

        if (!blockdevPath.isEmpty())
        {
            output.clear();
            error.clear();

            if (!runCommand(blockdevPath, {QStringLiteral("--rereadpt"), device},
                            {}, output, error, 10000))
            {
                errorMessage = QStringLiteral("Cannot reread partition table on %1: %2").arg(device, error);
                return false;
            }
        }
    }

    /*
     * partprobe informa il kernel della modifica
     * della partition table.
     */
    // ============================================================
    // UDEV: ASPETTA GLI EVENTI SE DISPONIBILE
    // ============================================================

    const QString udevadmPath = QStandardPaths::findExecutable(QStringLiteral("udevadm"));

    if (!udevadmPath.isEmpty())
    {
        QString ignoredOutput;
        QString ignoredError;

        runCommand(udevadmPath, {QStringLiteral("settle"), QStringLiteral("--timeout=10")},
                   {}, ignoredOutput, ignoredError, 12000);
    }

    // ============================================================
    // TROVA LA PARTIZIONE REALE
    //
    // NON costruiamo "/dev/sdd1" manualmente:
    //
    // /dev/sdd      -> /dev/sdd1
    // /dev/nvme0n1  -> /dev/nvme0n1p1
    // /dev/mmcblk0   -> /dev/mmcblk0p1
    // ============================================================

    for (int attempt = 0; attempt < 30; ++attempt)
    {
        output.clear();
        error.clear();

        if (runCommand(lsblkPath, {QStringLiteral("-nrpo"), QStringLiteral("NAME,TYPE"), device},
                       {}, output, error, 5000))
        {
            const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

            QStringList partitions;

            for (const QString &rawLine : lines)
            {
                const QString line = rawLine.simplified();

                const int separator = line.lastIndexOf(QLatin1Char(' '));

                if (separator <= 0)
                    continue;

                const QString path = line.left(separator).trimmed();
                const QString type = line.mid(separator + 1).trimmed();

                if (type == QStringLiteral("part"))
                {
                    partitions.append(path);
                }
            }

            if (partitions.size() == 1)
            {
                const QString partition = partitions.constFirst();

                if (validateDevicePath(partition))
                {
                    preparedDevicePath = partition;

                    qInfo().noquote() << "Prepared Btrfs device:" << device << "->" << preparedDevicePath;

                    return true;
                }
            }
        }

        QThread::msleep(100);
    }

    errorMessage = QStringLiteral("Partition was created on %1 but the kernel did not expose exactly one partition").arg(device);

    return false;
}

bool AgentDbusService::prepareReplacementTarget(const QString &devicePath, QString &preparedDevicePath, QString &errorMessage) const
{
    preparedDevicePath.clear();
    errorMessage.clear();
    const QString device = devicePath.trimmed();

    if (!validateDevicePath(device))
    {
        errorMessage = QStringLiteral("Invalid replacement target: %1").arg(device);
        return false;
    }

    const QString lsblkPath = QStandardPaths::findExecutable(QStringLiteral("lsblk"));
    if (lsblkPath.isEmpty())
    {
        errorMessage = QStringLiteral("lsblk is not available");
        return false;
    }

    QProcess typeProcess;
    typeProcess.setProcessEnvironment(commandEnvironment());
    typeProcess.start(lsblkPath, { QStringLiteral("-ndo"), QStringLiteral("TYPE"), device });

    if (!typeProcess.waitForFinished(5000) || typeProcess.exitCode() != 0)
    {
        errorMessage = QStringLiteral("Cannot determine replacement target type");
        return false;
    }

    const QString type = QString::fromUtf8(typeProcess.readAllStandardOutput()).trimmed();

    // Disco intero: riutilizziamo la preparazione già usata da AddDevice.
    if (type == QStringLiteral("disk")) return prepareDeviceForBtrfs(device, preparedDevicePath, errorMessage);

    if (type != QStringLiteral("part"))
    {
        errorMessage = QStringLiteral("Unsupported replacement target type '%1'").arg(type);
        return false;
    }

    // ============================================================
    // PARTIZIONE: NON DEVE ESSERE MONTATA
    // ============================================================
    QProcess mountProcess;
    mountProcess.setProcessEnvironment(commandEnvironment());
    mountProcess.start(lsblkPath, { QStringLiteral("-ndo"), QStringLiteral("MOUNTPOINTS"), device });

    if (!mountProcess.waitForFinished(5000) || mountProcess.exitCode() != 0)
    {
        errorMessage = QStringLiteral("Cannot inspect replacement target mount state");
        return false;
    }

    if (!QString::fromUtf8(mountProcess.readAllStandardOutput()).trimmed().isEmpty())
    {
        errorMessage = QStringLiteral("Replacement target %1 is mounted").arg(device);
        return false;
    }

    // ============================================================
    // PARTIZIONE: NON DEVE AVERE FIRME
    // ============================================================
    const QString wipefsPath = QStandardPaths::findExecutable(QStringLiteral("wipefs"));
    if (wipefsPath.isEmpty())
    {
        errorMessage = QStringLiteral("wipefs is not available");
        return false;
    }

    QProcess signatureProcess;
    signatureProcess.setProcessEnvironment(commandEnvironment());
    signatureProcess.start(wipefsPath, { QStringLiteral("-n"), device });

    if (!signatureProcess.waitForFinished(5000) || signatureProcess.exitCode() != 0)
    {
        errorMessage = QStringLiteral("Cannot inspect replacement target signatures");
        return false;
    }

    const QString signatures = QString::fromUtf8(signatureProcess.readAllStandardOutput()).trimmed();
    if (!signatures.isEmpty())
    {
        errorMessage = QStringLiteral("Replacement target %1 contains an existing signature").arg(device);
        return false;
    }

    preparedDevicePath = device;
    return true;
}

bool AgentDbusService::blockDeviceSizeBytes(const QString &devicePath, quint64 &sizeBytes, QString &errorMessage) const
{
    sizeBytes = 0;
    errorMessage.clear();

    const QString blockdevPath = QStandardPaths::findExecutable(QStringLiteral("blockdev"));
    if (blockdevPath.isEmpty())
    {
        errorMessage = QStringLiteral("blockdev is not available");
        return false;
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.start(blockdevPath, { QStringLiteral("--getsize64"), devicePath });

    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        errorMessage = QStringLiteral("Cannot determine replacement target size");
        return false;
    }

    bool ok = false;
    sizeBytes = QString::fromUtf8(process.readAllStandardOutput()).trimmed().toULongLong(&ok);

    if (!ok || sizeBytes == 0)
    {
        errorMessage = QStringLiteral("Invalid replacement target size");
        return false;
    }

    return true;
}

bool AgentDbusService::addDeviceAuthorized(const QString &mountPoint, const QString &devicePath)
{
    const QString path = mountPoint.trimmed();
    const QString device = devicePath.trimmed();

    /* Ricontrolliamo dopo Polkit: non ci affidiamo alla sola validazione pre-autorizzazione. */
    if (!validateMountPoint(path)) { qWarning() << "AddDevice: invalid mount point" << path; return false; }
    if (!validateDevicePath(device)) { qWarning() << "AddDevice: invalid device path" << device; return false; }

    QString preparedDevice;
    QString preparationError;

    if (!prepareDeviceForBtrfs(device, preparedDevice, preparationError))
    {
        qWarning().noquote() << "AddDevice:" << preparationError;
        return false;
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.start(m_btrfsPath, {QStringLiteral("device"), QStringLiteral("add"), preparedDevice, path});

    if (!process.waitForStarted(3000)) { qWarning().noquote() << "Cannot start btrfs device add:" << process.errorString(); return false; }

    if (!process.waitForFinished(30000)) { process.kill(); process.waitForFinished(); qWarning().noquote() << "Timeout running btrfs device add:" << device << path; return false; }

    const QString standardOutput = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromUtf8(process.readAllStandardError()).trimmed();

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) { qWarning().noquote() << "btrfs device add failed:" << (!standardError.isEmpty() ? standardError : standardOutput); return false; }

    qInfo().noquote() << "Btrfs device added:" << preparedDevice << "to" << path;
    refreshMonitor(); /* Aggiorna SQLite prima che la GUI rilegga lo stato. */

    return true;
}

bool AgentDbusService::removeDeviceAuthorized(const QString &mountPoint, qulonglong deviceId)
{
    const QString path = mountPoint.trimmed();

    /* Ricontrolliamo tutto DOPO Polkit: lo stato del filesystem potrebbe essere cambiato mentre l'utente autorizzava l'operazione. */
    if (!validateMountPoint(path)) {
        qWarning() << "RemoveDevice: invalid mount point after authorization" << path;
        return false;
    }

    if (!validateBtrfsDeviceId(deviceId)) {
        qWarning() << "RemoveDevice: invalid devid after authorization" << deviceId;
        return false;
    }

    if (!m_monitor) {
        qWarning() << "RemoveDevice: monitor unavailable after authorization";
        return false;
    }

    // ============================================================
    // DEVICE REPLACE CONFLICT
    // ============================================================

    if (!m_pendingReplaceStartMount.isEmpty() || !m_pendingReplaceCancelMount.isEmpty() || (m_replaceStartProcess && m_replaceStartProcess->state() != QProcess::NotRunning) || (m_replaceCancelProcess && m_replaceCancelProcess->state() != QProcess::NotRunning))
    {
        qWarning() << "RemoveDevice blocked:" << "device replace command is active";
        return false;
    }

    const ReplaceRuntimeInfo replaceInfo = queryReplaceState(path);

    if (replaceInfo.state == QStringLiteral("running"))
    {
        qWarning().noquote() << "RemoveDevice blocked:" << "device replace is running on" << path;
        return false;
    }

    if (replaceInfo.state == QStringLiteral("error"))
    {
        qWarning().noquote() << "RemoveDevice blocked:" << "cannot determine replace state:" << replaceInfo.details;
        return false;
    }

    bool present = true;

    if (!m_monitor->btrfsDevicePresence(path, deviceId, present)) {
        qWarning() << "RemoveDevice:" << "device is no longer a member after authorization" << path << "devid" << deviceId;
        return false;
    }

    if (present) {
        qWarning() << "RemoveDevice:" << "device became present before removal" << path << "devid" << deviceId;
        return false;
    }

    qInfo().noquote() << "RemoveDevice authorized validation passed:" << "mount:" << path << "devid:" << deviceId;

    quint64 initialAllocatedBytes = 0;

    if (!m_monitor->btrfsDeviceAllocatedBytes(path, deviceId, initialAllocatedBytes))
    {
        qWarning() << "RemoveDevice:" << "cannot read initial allocated bytes" << path << "devid" << deviceId;

        initialAllocatedBytes = 0;
    }

    if (m_removeProcess->state() != QProcess::NotRunning) {
        qWarning() << "RemoveDevice: another remove operation is already running";
        return false;
    }

    m_pendingRemoveMount = path;
    m_pendingRemoveDeviceId = deviceId;

    m_pendingRemoveInitialAllocatedBytes = initialAllocatedBytes;

    m_removeProcess->setProgram(m_btrfsPath);
    m_removeProcess->setArguments({QStringLiteral("device"), QStringLiteral("remove"), QString::number(deviceId), path});

    qInfo().noquote() << "RemoveDevice initial allocation:" << "mount:" << path << "devid:" << deviceId << "bytes:" << m_pendingRemoveInitialAllocatedBytes;

    m_removeProcess->start();

    if (!m_removeProcess->waitForStarted(3000)) {

        const QString error = m_removeProcess->errorString();

        qWarning().noquote() << "Cannot start btrfs device remove:" << error;

        m_pendingRemoveMount.clear();
        m_pendingRemoveDeviceId = 0;
        m_pendingRemoveInitialAllocatedBytes = 0;

        m_lastRemovePercent = -1;

        return false;
    }

    m_lastRemovePercent = -1;

    if (m_removeProgressTimer)
    {
        m_removeProgressTimer->start();
    }

    qInfo().noquote() << "Btrfs device remove started:" << "mount:" << path << "devid:" << deviceId;
    return true;

}

void AgentDbusService::onRemoveFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString mountPoint = m_pendingRemoveMount;
    const qulonglong deviceId = m_pendingRemoveDeviceId;
    const QString standardOutput = QString::fromUtf8(m_removeProcess->readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromUtf8(m_removeProcess->readAllStandardError()).trimmed();

    if (m_removeProgressTimer)
    {
        m_removeProgressTimer->stop();
    }

    m_lastRemovePercent = -1;

    m_pendingRemoveMount.clear();
    m_pendingRemoveDeviceId = 0;
    m_pendingRemoveInitialAllocatedBytes = 0;

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        const QString error = !standardError.isEmpty() ? standardError : standardOutput;
        qWarning().noquote() << "Btrfs device remove failed:" << "mount:" << mountPoint << "devid:" << deviceId << error;
        refreshMonitor();
        return;
    }

    qInfo().noquote() << "RemoveDevice progress:" << "mount:" << mountPoint << "devid:" << deviceId << "completed:" << 100 << "%" << "remaining bytes:" << 0;

    emit RemoveDeviceProgress(mountPoint, deviceId, 100, 0);

    qInfo().noquote() << "Btrfs device removed:" << "mount:" << mountPoint << "devid:" << deviceId;
    refreshMonitor();
}

bool AgentDbusService::CancelBalance(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        emit BalanceStateChanged(path, QStringLiteral("error"), QStringLiteral("Invalid mount point"));
        return false;
    }

    if (!calledFromDBus())
    {
        qWarning() << "CancelBalance called outside D-Bus";
        return false;
    }

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.balance.cancel"), path, [this, path]()
        {
            return cancelBalanceAuthorized(path);
        });

    return false;
}

bool AgentDbusService::cancelBalanceAuthorized(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        emit BalanceStateChanged(path, QStringLiteral("error"), QStringLiteral("Invalid mount point"));
        return false;
    }

    if (m_cancelProcess->state() != QProcess::NotRunning)
    {
        qWarning() << "Balance cancellation already running";
        return false;
    }

    m_pendingCancelMount = path;
    m_cancelRequested = true;

    m_cancelProcess->setProgram(m_btrfsPath);

    m_cancelProcess->setArguments({
        QStringLiteral("balance"),
        QStringLiteral("cancel"),
        path
    });

    m_cancelProcess->start();

    if (!m_cancelProcess->waitForStarted(3000))
    {
        const QString error =
            m_cancelProcess->errorString();

        m_cancelRequested = false;
        m_pendingCancelMount.clear();

        emit BalanceStateChanged(path, QStringLiteral("error"), error);

        return false;
    }

    qInfo().noquote() << "Btrfs balance cancellation requested:" << path;

    emit BalanceStateChanged(
        path,
        QStringLiteral("stopping"),
        QStringLiteral(
            "Balance cancellation requested"));

    return true;
}


void AgentDbusService::onCancelFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString mountPoint = m_pendingCancelMount;

    const QString standardOutput = QString::fromUtf8(m_cancelProcess->readAllStandardOutput()).trimmed();

    const QString standardError = QString::fromUtf8(m_cancelProcess->readAllStandardError()).trimmed();

    m_pendingCancelMount.clear();


    if (exitStatus != QProcess::NormalExit ||
        exitCode != 0)
    {
        m_cancelRequested = false;

        QString error = standardError;

        if (error.isEmpty())
            error = standardOutput;

        if (error.isEmpty())
            error =
                QStringLiteral(
                    "btrfs balance cancel failed");

        qWarning().noquote()
            << "Btrfs balance cancellation failed:"
            << error;

        emit BalanceStateChanged(
            mountPoint,
            QStringLiteral("error"),
            error);

        return;
    }


    m_cancelRequested = false;

    if (m_activeBalanceMount == mountPoint)
    {
        m_activeBalanceMount.clear();
    }

    m_lastBalanceState = QStringLiteral("stopped");

    m_lastBalancePercent = -1;

    m_balanceTimer->stop();

    qInfo().noquote() << "Btrfs balance stopped:" << mountPoint;

    QTimer::singleShot(250, this, [this, mountPoint, standardOutput]()
        {
            refreshMonitor();

            emit BalanceStateChanged(mountPoint, QStringLiteral("stopped"),
                standardOutput.isEmpty()
                    ? QStringLiteral("Balance stopped")
                    : standardOutput);
        });
}

AgentDbusService::ScrubRuntimeInfo AgentDbusService::queryScrubState(const QString &mountPoint) const
{
    ScrubRuntimeInfo info;
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) {
        info.state = QStringLiteral("error");
        info.details = QStringLiteral("Invalid mount point");
        return info;
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.start(m_btrfsPath, {QStringLiteral("scrub"), QStringLiteral("status"), path});

    if (!process.waitForStarted(3000)) {
        info.state = QStringLiteral("error");
        info.details = process.errorString();
        return info;
    }

    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished();
        info.state = QStringLiteral("error");
        info.details = QStringLiteral("Timeout reading scrub status");
        return info;
    }

    const QString standardOutput = QString::fromUtf8(process.readAllStandardOutput());
    const QString standardError = QString::fromUtf8(process.readAllStandardError());
    info.details = (standardOutput + QStringLiteral("\n") + standardError).trimmed();

    const QStringList lines = info.details.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    static const QRegularExpression statusRegex(QStringLiteral("^Status:\\s*(.+)$"), QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression totalRegex(QStringLiteral("^Total to scrub:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?i?B|B)?"), QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression bytesRegex(QStringLiteral("^Bytes scrubbed:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?i?B|B)?.*?\\(([0-9]+(?:\\.[0-9]+)?)%\\)"), QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression rateRegex(QStringLiteral("^Rate:\\s*([0-9]+(?:\\.[0-9]+)?)\\s*([KMGTPE]?i?B|B)?/s"), QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression errorValueRegex(QStringLiteral("\\b(?:csum|super|verify|read)=(\\d+)"), QRegularExpression::CaseInsensitiveOption);

    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();

        {
            const auto match = statusRegex.match(line);
            if (match.hasMatch()) {
                const QString rawState = match.captured(1).trimmed().toLower();
                if (rawState.contains(QStringLiteral("running"))) {
                    info.state = QStringLiteral("running");
                } else if (rawState.contains(QStringLiteral("finished")) || rawState.contains(QStringLiteral("completed"))) {
                    info.state = QStringLiteral("finished");
                } else if (rawState.contains(QStringLiteral("interrupted"))) {
                    info.state = QStringLiteral("interrupted");
                } else if (rawState.contains(QStringLiteral("aborted")) || rawState.contains(QStringLiteral("cancelled")) || rawState.contains(QStringLiteral("canceled"))) {
                    info.state = QStringLiteral("cancelled");
                } else {
                    info.state = rawState;
                }
                continue;
            }
        }

        {
            const auto match = totalRegex.match(line);
            if (match.hasMatch()) {
                info.totalBytes = scrubValueToBytes(match.captured(1).toDouble(), match.captured(2));
                continue;
            }
        }

        {
            const auto match = bytesRegex.match(line);
            if (match.hasMatch()) {
                info.processedBytes = scrubValueToBytes(match.captured(1).toDouble(), match.captured(2));
                const double percent = match.captured(3).toDouble();
                info.completedPercent = qBound(0, static_cast<int>(percent + 0.5), 100);
                continue;
            }
        }

        {
            const auto match = rateRegex.match(line);
            if (match.hasMatch()) {
                info.speedBytesPerSec = scrubValueToBytes(match.captured(1).toDouble(), match.captured(2));
                continue;
            }
        }

        if (line.startsWith(QStringLiteral("Error summary:"), Qt::CaseInsensitive)) {
            if (line.contains(QStringLiteral("no errors found"), Qt::CaseInsensitive)) {
                info.errorCount = 0;
            } else {
                int errors = 0;
                auto it = errorValueRegex.globalMatch(line);
                while (it.hasNext()) {
                    errors += it.next().captured(1).toInt();
                }
                info.errorCount = errors;
            }
        }
    }

    /* A scrub concluso btrfs-progs non stampa più "Bytes scrubbed". */
    if (info.state == QStringLiteral("finished") && info.processedBytes == 0 && info.totalBytes > 0) {
        info.processedBytes = info.totalBytes;
        info.completedPercent = 100;
    }

    /* Filesystem che non ha statistiche scrub. */
    if (info.state.isEmpty()) {
        const QString lower = info.details.toLower();
        if (lower.contains(QStringLiteral("no stats available"))) {
            info.state = QStringLiteral("not running");
        } else {
            info.state = QStringLiteral("error");
        }
    }

    qInfo().noquote() << "Scrub status:" << path << "state:" << info.state << "progress:" << info.completedPercent << "%" << "processed:" << info.processedBytes << "total:" << info.totalBytes << "speed:" << info.speedBytesPerSec << "errors:" << info.errorCount;

    return info;
}

AgentDbusService::ReplaceRuntimeInfo AgentDbusService::queryReplaceState(const QString &mountPoint) const
{
    ReplaceRuntimeInfo info;
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        info.state = QStringLiteral("error");
        info.details = QStringLiteral("Invalid mount point");
        return info;
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.start(m_btrfsPath, { QStringLiteral("replace"), QStringLiteral("status"), QStringLiteral("-1"), path });

    if (!process.waitForStarted(3000))
    {
        info.state = QStringLiteral("error");
        info.details = process.errorString();
        return info;
    }

    if (!process.waitForFinished(5000))
    {
        process.kill();
        process.waitForFinished();
        info.state = QStringLiteral("error");
        info.details = QStringLiteral("Timeout reading replace status");
        return info;
    }

    const QString standardOutput = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromUtf8(process.readAllStandardError()).trimmed();
    info.details = !standardOutput.isEmpty() ? standardOutput : standardError;

    const QString lower = info.details.trimmed().toLower();

    // NEVER STARTED
    if (lower.contains(QStringLiteral("never started")))
    {
        info.state = QStringLiteral("idle");
        info.completedPercent = -1;
        return info;
    }

    // PROGRESS
    static const QRegularExpression progressRegex(QStringLiteral(R"((\d+(?:\.\d+)?)\s*%\s*done)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch progressMatch = progressRegex.match(info.details);

    if (progressMatch.hasMatch())
    {
        bool ok = false;
        const double percent = progressMatch.captured(1).toDouble(&ok);
        if (ok) info.completedPercent = qBound(0, static_cast<int>(percent + 0.5), 100);
    }

    // FINISHED
    if (lower.contains(QStringLiteral("finished on")) || lower.contains(QStringLiteral("finished")))
    {
        info.state = QStringLiteral("finished");
        info.completedPercent = 100;
        return info;
    }

    // CANCELLED
    if (lower.contains(QStringLiteral("cancelled")) || lower.contains(QStringLiteral("canceled")))
    {
        info.state = QStringLiteral("cancelled");
        return info;
    }

    // RUNNING
    if (info.completedPercent >= 0 || lower.contains(QStringLiteral("started on")))
    {
        info.state = QStringLiteral("running");
        return info;
    }

    // COMMAND ERROR / UNKNOWN OUTPUT
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        info.state = QStringLiteral("error");
        if (info.details.isEmpty()) info.details = QStringLiteral("btrfs replace status failed");
        return info;
    }

    info.state = QStringLiteral("error");
    if (info.details.isEmpty()) info.details = QStringLiteral("Unable to determine replace status");
    return info;
}

QString AgentDbusService::queryBalanceState(const QString &mountPoint, QString &details) const
{
    QProcess process;

    process.setProcessEnvironment(commandEnvironment());

    process.start(m_btrfsPath,
        {
            QStringLiteral("balance"),
            QStringLiteral("status"),
            mountPoint
        });

    if (!process.waitForStarted(3000))
    {
        details = QStringLiteral("Cannot start btrfs balance status");

        return QStringLiteral("error");
    }

    if (!process.waitForFinished(5000))
    {
        process.kill();
        process.waitForFinished();

        details = QStringLiteral("Timeout reading balance status");

        return QStringLiteral("error");
    }

    const QString standardOutput = QString::fromUtf8(process.readAllStandardOutput()).trimmed();

    const QString standardError = QString::fromUtf8(process.readAllStandardError()).trimmed();

    details =
        !standardOutput.isEmpty()
            ? standardOutput
            : standardError;

    const QString lower = details.toLower();

    if (lower.contains(QStringLiteral("running")))
    {
        return QStringLiteral("running");
    }

    if (lower.contains(QStringLiteral("paused")))
    {
        return QStringLiteral("paused");
    }

    if (lower.contains(QStringLiteral("no balance found")))
    {
        return QStringLiteral("idle");
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        return QStringLiteral("error");
    }

    return QStringLiteral("idle");
}

void AgentDbusService::updateBalanceProgressFromDetails(const QString &mountPoint, const QString &details)
{
    static const QRegularExpression leftRegex(
        QStringLiteral(
            R"((\d{1,3})\s*%\s*left)"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match =
        leftRegex.match(details);

    if (!match.hasMatch())
        return;

    bool ok = false;

    const int leftPercent =
        match.captured(1).toInt(&ok);

    if (!ok ||
        leftPercent < 0 ||
        leftPercent > 100)
    {
        return;
    }

    const int completedPercent =
        100 - leftPercent;

    /*
     * Il timer gira ogni 2 secondi, ma non inviamo
     * segnali D-Bus duplicati se la percentuale
     * non è cambiata.
     */
    if (completedPercent ==
        m_lastBalancePercent)
    {
        return;
    }

    m_lastBalancePercent =
        completedPercent;

    qInfo().noquote()
        << "Btrfs balance progress:"
        << mountPoint
        << completedPercent << "% completed,"
        << leftPercent << "% left";

    emit BalanceProgressChanged(
        mountPoint,
        completedPercent,
        leftPercent);
}

QString AgentDbusService::BalanceStatus(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    QString details;

    const QString state = queryBalanceState(path, details);

    /*
     * Se il daemon è stato riavviato mentre un
     * balance era già in corso, una richiesta
     * BalanceStatus della GUI riaggancia il
     * monitoraggio periodico.
     */
    if (state == QStringLiteral("running") ||
        state == QStringLiteral("paused"))
    {
        if (m_activeBalanceMount != path)
        {
            m_activeBalanceMount =
                path;

            m_lastBalancePercent =
                -1;
        }

        m_lastBalanceState =
            state;

        if (!m_balanceTimer->isActive())
            m_balanceTimer->start();

        if (state ==
            QStringLiteral("running"))
        {
            updateBalanceProgressFromDetails(
                path,
                details);
        }
    }

    return state;
}


void AgentDbusService::pollBalance()
{
    if (m_activeBalanceMount.isEmpty())
    {
        m_balanceTimer->stop();
        return;
    }

    QString details;

    const QString state = queryBalanceState(m_activeBalanceMount, details);


    // ============================================================
    // RUNNING
    // ============================================================

    if (state == QStringLiteral("running"))
    {
        /* Durante la cancellazione continuiamo a
         * controllare Btrfs, ma non riportiamo la GUI
         * da "stopping" a "running". */

        if (m_cancelRequested)
            return;

        /* Estrae, ad esempio: 52% left e invia: completed = 48 left = 52*/

        updateBalanceProgressFromDetails(m_activeBalanceMount, details);

        if (m_lastBalanceState != state)
        {
            m_lastBalanceState = state;

            emit BalanceStateChanged(m_activeBalanceMount, state, details);
        }

        return;
    }


    // ============================================================
    // PAUSED
    // ============================================================

    if (state == QStringLiteral("paused"))
    {
        if (m_lastBalanceState != state)
        {
            m_lastBalanceState = state;

            emit BalanceStateChanged(m_activeBalanceMount, state, details);
        }

        return;
    }


    // ============================================================
    // IDLE = BALANCE COMPLETED
    // ============================================================

    if (state == QStringLiteral("idle"))
    {
        if (m_cancelRequested)
            return;

        const QString completedMount = m_activeBalanceMount;

        m_activeBalanceMount.clear();

        m_balanceTimer->stop();

        m_lastBalanceState = QStringLiteral("completed");

        m_lastBalancePercent = -1;

        qInfo().noquote() << "Btrfs balance completed:" << completedMount;

        QTimer::singleShot(
            250,
            this,
            [this, completedMount]()
            {

             // Prima aggiorniamo SQLite.
        refreshMonitor();

             /* Solo dopo notifichiamo i client D-Bus.
             * Chi riceve il segnale può quindi rileggere il DB.  */
            emit BalanceStateChanged(completedMount, QStringLiteral("completed"),  QStringLiteral("Balance completed"));
            });

        return;
    }


    // ============================================================
    // STATUS ERROR
    // ============================================================

    qWarning().noquote() << "Cannot determine balance status:" << details;
}


void AgentDbusService::refreshMonitor()
{
    if (!m_monitor)
    {
        qWarning() << "Cannot refresh Btrfs monitor: monitor is null";
        return;
    }

    qInfo().noquote() << "Immediate Btrfs refresh requested by D-Bus operation";

    m_monitor->refreshNow();
}

void AgentDbusService::pollRemoveProgress()
{
    if (!m_removeProcess || m_removeProcess->state() == QProcess::NotRunning)
    {
        m_removeProgressTimer->stop();
        return;
    }

    if (!m_monitor || m_pendingRemoveMount.isEmpty() || m_pendingRemoveDeviceId == 0)
    {
        return;
    }

    /*
     * Forziamo un refresh così l'allocazione del device
     * missing viene riletta durante la relocation.
     */
    m_monitor->refreshNow();

    quint64 currentAllocatedBytes = 0;

    if (!m_monitor->btrfsDeviceAllocatedBytes(m_pendingRemoveMount, m_pendingRemoveDeviceId, currentAllocatedBytes))
    {
        qWarning() << "RemoveDevice progress:" << "cannot read current allocated bytes";
        return;
    }

    int percent = 0;

    if (m_pendingRemoveInitialAllocatedBytes > 0)
    {
        const double completed =
            1.0 -
            (static_cast<double>(currentAllocatedBytes) /
             static_cast<double>(
                 m_pendingRemoveInitialAllocatedBytes));

        percent = qBound(0, static_cast<int>(completed * 100.0), 100);
    }

    if (percent == m_lastRemovePercent)
        return;

    m_lastRemovePercent = percent;

    qInfo().noquote() << "RemoveDevice progress:" << "mount:" << m_pendingRemoveMount << "devid:" << m_pendingRemoveDeviceId << "completed:" << percent << "%" << "remaining bytes:" << currentAllocatedBytes;

    emit RemoveDeviceProgress(m_pendingRemoveMount, m_pendingRemoveDeviceId, percent, currentAllocatedBytes);
}

QString AgentDbusService::ScrubStatus(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();
    if (!validateMountPoint(path)) {
        qWarning() << "ScrubStatus: invalid mount point" << path;
        return QStringLiteral("error");
    }

    const ReplaceRuntimeInfo replaceInfo = queryReplaceState(path);

    if (replaceInfo.state == QStringLiteral("running"))
    {
        /*
     * btrfs scrub status può apparire "running"
     * durante device replace perché il kernel usa
     * internamente il motore scrub.
     *
     * Per l'API ScrubStatus non è un vero scrub
     * di manutenzione.
     */
        qDebug().noquote() << "ScrubStatus:" << path << "device replace is active;" << "ignoring internal scrub state";

        return QStringLiteral("not running");
    }

    const ScrubRuntimeInfo info = queryScrubState(path);

    // Se lo scrub è in esecuzione, riallinea lo stato interno e il timer
    if (info.state == QStringLiteral("running")) {
        if (m_activeScrubMount != path) {
            m_activeScrubMount = path;
            m_lastScrubState = QStringLiteral("running");
            m_lastScrubPercent = info.completedPercent;

            qInfo().noquote() << "Reattached Btrfs scrub monitoring:" << path
                              << "progress:" << info.completedPercent << "%";
        }

        if (m_scrubTimer && !m_scrubTimer->isActive()) {
            m_scrubTimer->start();
        }

        // Notifica subito il progresso se non è stata richiesta una cancellazione
        if (!m_scrubCancelRequested && info.completedPercent >= 0) {
            emit ScrubProgressChanged(path, info.completedPercent, info.processedBytes,
                                      info.totalBytes, info.speedBytesPerSec, info.errorCount);
        }
    }

    return info.state;
}

QStringList AgentDbusService::ReplaceStatus(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        qWarning() << "ReplaceStatus:" << "invalid mount point" << path;
        return { QStringLiteral("error"), QStringLiteral("-1"), QStringLiteral("Invalid mount point") };
    }

    const ReplaceRuntimeInfo info = queryReplaceState(path);

    if (info.state == QStringLiteral("running")) {
        const bool stateChanged = m_lastReplaceState != QStringLiteral("running");
        const bool progressChanged = info.completedPercent >= 0 && info.completedPercent != m_lastReplacePercent;

        m_activeReplaceMount = path;
        m_replaceStatusErrorCount = 0;

        /* Non alteriamo lo stato di cancellazione. ReplaceStatus può essere chiamato dalla GUI mentre CancelReplace è ancora in corso. */
        if (!m_replaceCancelRequested && (stateChanged || progressChanged)) {
            if (m_monitor && !m_monitor->updateDeviceReplacement(path, QStringLiteral("running"), info.completedPercent, info.details)) { qWarning().noquote() << "ReplaceStatus:" << "cannot persist running replacement:" << path; }

            m_lastReplaceState = QStringLiteral("running");
            m_lastReplacePercent = info.completedPercent;
        }

        if (m_replaceTimer && !m_replaceTimer->isActive()) { m_replaceTimer->start(); }
    }


    qInfo().noquote() << "Replace status:" << path << "state:" << info.state << "progress:" << info.completedPercent << "%" << "details:" << info.details;

    return { info.state, QString::number(info.completedPercent), info.details };
}

bool AgentDbusService::StartReplace(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath)
{
    const QString path = mountPoint.trimmed();
    const QString target = targetDevicePath.trimmed();

    if (!validateMountPoint(path)) { qWarning() << "StartReplace:" << "invalid mount point" << path; return false; }
    if (!validateBtrfsDeviceId(sourceDeviceId)) { qWarning() << "StartReplace:" << "invalid source devid" << sourceDeviceId; return false; }
    if (!validateDevicePath(target)) { qWarning() << "StartReplace:" << "invalid target" << target; return false; }
    if (!m_monitor) { qWarning() << "StartReplace:" << "monitor unavailable"; return false; }

    bool sourcePresent = false;
    if (!m_monitor->btrfsDevicePresence(path, sourceDeviceId, sourcePresent)) { qWarning() << "StartReplace:" << "source devid is not a member" << sourceDeviceId; return false; }
    if (!calledFromDBus()) { qWarning() << "StartReplace called outside D-Bus"; return false; }

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.replace.start"), path, [this, path, sourceDeviceId, target]() { return startReplaceAuthorized(path, sourceDeviceId, target); });

    return false;
}

QStringList AgentDbusService::AutoScrubConfig(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) {
        qWarning() << "AutoScrubConfig: invalid mount point" << path;
        return {};
    }

    if (!m_monitor) {
        qWarning() << "AutoScrubConfig: monitor unavailable";
        return {};
    }

    BtrfsMonitor::AutoScrubConfig config;
    if (!m_monitor->autoScrubConfigForMountPoint(path, config)) {
        qWarning().noquote() << "AutoScrubConfig: cannot read configuration for" << path;
        return {};
    }

    return { config.enabled ? QStringLiteral("1") : QStringLiteral("0"), QString::number(config.intervalWeeks) };
}

QStringList AgentDbusService::AutoScrubSchedule(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) return qWarning() << "AutoScrubSchedule: invalid mount point" << path, QStringList{};
    if (!m_monitor) return qWarning() << "AutoScrubSchedule: monitor unavailable", QStringList{};

    BtrfsMonitor::AutoScrubSchedule schedule;
    if (!m_monitor->autoScrubScheduleForMountPoint(path, schedule)) return qWarning().noquote() << "AutoScrubSchedule: cannot read schedule for" << path, QStringList{};

    return { QString::number(schedule.lastSuccessfulEpoch), QString::number(schedule.nextDueEpoch) };
}

bool AgentDbusService::SetAutoScrubConfig(const QString &mountPoint, bool enabled, int intervalWeeks)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) {
        qWarning() << "SetAutoScrubConfig: invalid mount point" << path;
        return false;
    }

    if (intervalWeeks < 1 || intervalWeeks > 6) {
        qWarning() << "SetAutoScrubConfig: invalid interval" << intervalWeeks;
        return false;
    }

    if (!calledFromDBus()) {
        qWarning() << "SetAutoScrubConfig called outside D-Bus";
        return false;
    }

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.scrub.configure"), path, [this, path, enabled, intervalWeeks]() {
        return setAutoScrubConfigAuthorized(path, enabled, intervalWeeks);
    });

    // La risposta reale viene inviata dopo Polkit tramite delayed reply
    return false;
}

bool AgentDbusService::setAutoScrubConfigAuthorized(const QString &mountPoint, bool enabled, int intervalWeeks)
{
    const QString path = mountPoint.trimmed();

    /* Ricontrolliamo i parametri DOPO Polkit. */
    if (!validateMountPoint(path)) { qWarning() << "SetAutoScrubConfig: invalid mount point after authorization" << path; return false; }

    if (intervalWeeks < 1 || intervalWeeks > 6) { qWarning() << "SetAutoScrubConfig: invalid interval after authorization" << intervalWeeks; return false; }

    if (!m_monitor) { qWarning() << "SetAutoScrubConfig: monitor unavailable"; return false; }

    if (!m_monitor->setAutoScrubConfigForMountPoint(path, enabled, intervalWeeks)) { qWarning().noquote() << "Cannot save automatic scrub configuration:" << path << "enabled:" << enabled << "interval:" << intervalWeeks; return false; }

    qInfo().noquote() << "Automatic scrub configuration updated:" << path << "enabled:" << enabled << "interval:" << intervalWeeks << "week(s)";

    if (enabled)
    {
        QTimer::singleShot(0, this, &AgentDbusService::checkAutoScrubSchedule);
    }

    return true;
}

bool AgentDbusService::StartScrub(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        emit ScrubStateChanged(path, QStringLiteral("error"), QStringLiteral("Invalid mount point"));
        return false;
    }

    if (!calledFromDBus())
    {
        qWarning() << "StartScrub called outside D-Bus";
        return false;
    }

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.scrub.start"), path, [this, path]() { return startScrubAuthorized(path); });

    /*
     * La risposta reale viene inviata
     * da authorizeAndRun() dopo Polkit.
     */
    return false;
}

bool AgentDbusService::startScrubAuthorized(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        emit ScrubStateChanged(path, QStringLiteral("error"), QStringLiteral("Invalid mount point"));
        return false;
    }

    if (m_scrubStartProcess->state() != QProcess::NotRunning)
    {
        qWarning() << "Scrub start command already running";
        return false;
    }

    if (m_scrubCancelProcess->state() != QProcess::NotRunning)
    {
        qWarning() << "Scrub cancel command is running";
        return false;
    }

    // ============================================================
    // DEVICE REPLACE CONFLICT
    // ============================================================

    if (!m_pendingReplaceStartMount.isEmpty() || !m_pendingReplaceCancelMount.isEmpty() || (m_replaceStartProcess && m_replaceStartProcess->state() != QProcess::NotRunning) || (m_replaceCancelProcess && m_replaceCancelProcess->state() != QProcess::NotRunning))
    {
        qWarning() << "Scrub blocked:" << "device replace command is active";
        return false;
    }

    const ReplaceRuntimeInfo replaceInfo = queryReplaceState(path);

    if (replaceInfo.state == QStringLiteral("running"))
    {
        qWarning().noquote() << "Scrub blocked:" << "device replace is running on" << path;
        return false;
    }

    if (replaceInfo.state == QStringLiteral("error"))
    {
        qWarning().noquote() << "Scrub blocked:" << "cannot determine replace state:" << replaceInfo.details;
        return false;
    }

    // ============================================================
    // CHECK EXISTING SCRUB
    // ============================================================
    const ScrubRuntimeInfo current = queryScrubState(path);

    if (current.state == QStringLiteral("running"))
    {
        m_activeScrubMount = path;
        m_lastScrubState = QStringLiteral("running");
        m_lastScrubPercent = current.completedPercent;

        emit ScrubStateChanged(path, QStringLiteral("running"), current.details);
        emit ScrubProgressChanged(path, current.completedPercent, current.processedBytes, current.totalBytes, current.speedBytesPerSec, current.errorCount);

        qWarning().noquote() << "Scrub already running:" << path;
        return false;
    }

    // ============================================================
    // START SCRUB: Eseguito in transient service systemd separata
    // per evitare interruzioni al restart del daemon.
    // ============================================================

    const QString systemdRunPath = QStandardPaths::findExecutable(QStringLiteral("systemd-run"));

    if (systemdRunPath.isEmpty()) {
        const QString error = QStringLiteral("systemd-run is not available");
        qWarning().noquote() << "Cannot start Btrfs scrub:" << error;
        emit ScrubStateChanged(path, QStringLiteral("error"), error);
        return false;
    }

    m_pendingScrubStartMount = path;
    m_scrubCancelRequested = false;
    m_lastScrubPercent = -1;
    m_scrubStatusErrorCount = 0;

    m_scrubStartProcess->setProgram(systemdRunPath);
    m_scrubStartProcess->setArguments({
        QStringLiteral("--unit=storage-monitor-scrub.service"),
        QStringLiteral("--collect"),           // Rimuove automaticamente la unit quando ha finito
        QStringLiteral("--quiet"),             // Silenzia l'output superfluo verso il daemon
        QStringLiteral("--service-type=exec"), // Avviata solo dopo che exec() di btrfs è riuscito
        m_btrfsPath,
        QStringLiteral("scrub"),
        QStringLiteral("start"),
        QStringLiteral("-B"),                  // Resta in foreground nella transient service
        path
    });

    m_scrubStartProcess->start();

    if (!m_scrubStartProcess->waitForStarted(3000)) {
        const QString error = m_scrubStartProcess->errorString();
        m_pendingScrubStartMount.clear();

        qWarning().noquote() << "Cannot start transient Btrfs scrub service:" << error;
        emit ScrubStateChanged(path, QStringLiteral("error"), error);
        return false;
    }

    emit ScrubStateChanged(path, QStringLiteral("starting"), QStringLiteral("Scrub start requested"));

    return true;
}

void AgentDbusService::onScrubStartFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString mountPoint = m_pendingScrubStartMount;
    const QString standardOutput = QString::fromUtf8(m_scrubStartProcess->readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromUtf8(m_scrubStartProcess->readAllStandardError()).trimmed();

    m_pendingScrubStartMount.clear();

    if (exitStatus != QProcess::NormalExit || exitCode != 0)
    {
        QString error = standardError;

        if (error.isEmpty()) error = standardOutput;
        if (error.isEmpty()) error = QStringLiteral("btrfs scrub start failed");

        qWarning().noquote() << "Btrfs scrub start failed:" << error;
        emit ScrubStateChanged(mountPoint, QStringLiteral("error"), error);
        return;
    }

    /*
    * systemd-run ritorna dopo che la transient service
    * è stata avviata.
    *
    * "btrfs scrub start -B" continua invece a girare
    * dentro storage-monitor-scrub.service.
    *
    * Verifichiamo quindi lo stato reale dello scrub.
    */
    const ScrubRuntimeInfo info = queryScrubState(mountPoint);

    if (info.state == QStringLiteral("running"))
    {
        m_activeScrubMount = mountPoint;
        m_lastScrubState = QStringLiteral("running");
        m_lastScrubPercent = info.completedPercent;

        if (m_scrubTimer && !m_scrubTimer->isActive()) m_scrubTimer->start();

        /*
        * Facciamo osservare immediatamente lo stato running
        * anche al BtrfsMonitor, così viene aperta la riga
        * btrfs_operations senza aspettare il normale tick
        * da 30 secondi.
        */
        refreshMonitor();

        emit ScrubStateChanged(mountPoint, QStringLiteral("running"), info.details);
        emit ScrubProgressChanged(mountPoint, info.completedPercent, info.processedBytes, info.totalBytes, info.speedBytesPerSec, info.errorCount);

        qInfo().noquote() << "Btrfs scrub started:" << mountPoint;
        return;
    }

    /*
     * Caso raro: il comando è riuscito ma
     * lo status non è ancora passato a running.
     * Non lo marchiamo come errore prematuramente.
     */
    m_activeScrubMount = mountPoint;
    m_lastScrubState = QStringLiteral("starting");

    if (m_scrubTimer && !m_scrubTimer->isActive()) m_scrubTimer->start();

    qInfo().noquote() << "Btrfs scrub command completed:" << mountPoint << "current state:" << info.state;
}

void AgentDbusService::pollScrub()
{
    if (m_activeScrubMount.isEmpty())
    {
        if (m_scrubTimer) m_scrubTimer->stop();
        return;
    }

    const QString mountPoint = m_activeScrubMount;
    const ScrubRuntimeInfo info = queryScrubState(mountPoint);

    // ============================================================
    // REPEATED STATUS FAILURE
    // ============================================================

    if (info.state == QStringLiteral("error")) {
        ++m_scrubStatusErrorCount;
        qWarning().noquote() << "Cannot read active scrub status:" << mountPoint << "failure" << m_scrubStatusErrorCount << "of 3:" << info.details;

        /*
     * Un singolo errore può essere transitorio.
     * Dopo tre errori consecutivi, però, smettiamo
     * di bloccare il daemon ogni 5 secondi.
     */
        if (m_scrubStatusErrorCount < 3) return;

        const bool cancellationWasRequested = m_scrubCancelRequested;
        const QString failedMount = m_activeScrubMount;

        m_activeScrubMount.clear(); m_scrubCancelRequested = false; m_scrubStatusErrorCount = 0; m_lastScrubPercent = -1;
        if (m_scrubTimer) m_scrubTimer->stop();

        if (cancellationWasRequested) {
            m_lastScrubState = QStringLiteral("cancelled");
            qWarning().noquote() << "Btrfs scrub status became unavailable" << "after cancellation:" << failedMount;

            QTimer::singleShot(250, this, [this, failedMount]() { refreshMonitor(); emit ScrubStateChanged(failedMount, QStringLiteral("cancelled"), QStringLiteral("Scrub cancelled")); });
        } else {
            m_lastScrubState = QStringLiteral("interrupted");
            qWarning().noquote() << "Btrfs scrub monitoring terminated" << "after repeated status failures:" << failedMount;

            QTimer::singleShot(250, this, [this, failedMount]() { refreshMonitor(); emit ScrubStateChanged(failedMount, QStringLiteral("interrupted"), QStringLiteral("Scrub interrupted: status unavailable")); });
        }
        return;
    }

    /*
    * Qualsiasi risposta valida interrompe la sequenza
    * degli errori consecutivi.
    */
    m_scrubStatusErrorCount = 0;

    // ============================================================
    // RUNNING
    // ============================================================
    if (info.state == QStringLiteral("running"))
    {
        /*
         * Quando implementeremo CancelScrub,
         * durante "stopping" non dovremo riportare
         * la GUI indietro a "running".
         */
        if (m_scrubCancelRequested) return;

        if (m_lastScrubState != QStringLiteral("running"))
        {
            m_lastScrubState = QStringLiteral("running");
            /*
            * Copre il raro caso in cui systemd-run è terminato
            * quando btrfs non riportava ancora "running".
            */
            refreshMonitor();
            emit ScrubStateChanged(mountPoint, QStringLiteral("running"), info.details);
        }

        m_lastScrubPercent = info.completedPercent;

        /*
         * Per lo scrub inviamo il progresso ad ogni poll:
         * oltre alla percentuale cambiano anche velocità
         * e byte processati.
         */
        emit ScrubProgressChanged(mountPoint, info.completedPercent, info.processedBytes, info.totalBytes, info.speedBytesPerSec, info.errorCount);
        return;
    }

    // ============================================================
    // FINISHED
    // ============================================================
    if (info.state == QStringLiteral("finished"))
    {
        m_scrubCancelRequested = false;

        const QString completedMount = m_activeScrubMount;
        m_activeScrubMount.clear();

        if (m_scrubTimer) m_scrubTimer->stop();

        m_lastScrubState = QStringLiteral("finished");
        m_lastScrubPercent = 100;

        qInfo().noquote() << "Btrfs scrub completed:" << completedMount;

        /*
         * Come per il balance:
         * prima aggiorniamo SQLite,
         * poi notifichiamo i client D-Bus.
         */
        QTimer::singleShot(250, this, [this, completedMount, info]() {
            refreshMonitor();
            emit ScrubProgressChanged(completedMount, 100, info.processedBytes, info.totalBytes, info.speedBytesPerSec, info.errorCount);
            emit ScrubStateChanged(completedMount, QStringLiteral("finished"), QStringLiteral("Scrub completed"));
        });

        return;
    }

    // ============================================================
    // CANCELLED
    // ============================================================
    if (info.state == QStringLiteral("cancelled"))
    {
        m_scrubCancelRequested = false;

        const QString cancelledMount = m_activeScrubMount;
        m_activeScrubMount.clear();

        if (m_scrubTimer) m_scrubTimer->stop();

        m_lastScrubState = QStringLiteral("cancelled");
        m_lastScrubPercent = -1;

        qInfo().noquote() << "Btrfs scrub cancelled:" << cancelledMount;

        QTimer::singleShot(250, this, [this, cancelledMount, info]() {
            refreshMonitor();
            /*
             * Dopo una cancellazione alcune versioni
             * di btrfs-progs non riportano più
             * Bytes scrubbed / percentuale.
             */
            if (info.completedPercent >= 0) {
                emit ScrubProgressChanged(cancelledMount, info.completedPercent, info.processedBytes, info.totalBytes, info.speedBytesPerSec, info.errorCount);
            }
            emit ScrubStateChanged(cancelledMount, QStringLiteral("cancelled"), QStringLiteral("Scrub cancelled"));
        });

        return;
    }

    // ============================================================
    // INTERRUPTED
    // ============================================================
    if (info.state == QStringLiteral("interrupted")) {
        /*
     * Alcune versioni/configurazioni di btrfs-progs
     * possono riportare "interrupted" anche come
     * risultato finale di una cancellazione richiesta.
     *
     * In quel caso, dal punto di vista dell'utente,
     * l'operazione resta una cancellazione volontaria.
     */
        const bool cancellationWasRequested = m_scrubCancelRequested;
        const QString interruptedMount = m_activeScrubMount;

        m_activeScrubMount.clear(); m_scrubCancelRequested = false; m_lastScrubPercent = -1;
        if (m_scrubTimer) m_scrubTimer->stop();

        if (cancellationWasRequested) {
            m_lastScrubState = QStringLiteral("cancelled");
            qInfo().noquote() << "Btrfs scrub cancelled:" << interruptedMount << "(reported by btrfs as interrupted)";

            QTimer::singleShot(250, this, [this, interruptedMount, info]() {
                refreshMonitor();
                if (info.completedPercent >= 0) emit ScrubProgressChanged(interruptedMount, info.completedPercent, info.processedBytes, info.totalBytes, info.speedBytesPerSec, info.errorCount);
                emit ScrubStateChanged(interruptedMount, QStringLiteral("cancelled"), QStringLiteral("Scrub cancelled"));
            });
            return;
        }

    /*
     * Nessuna cancellazione è stata richiesta:
     * l'interruzione è quindi inattesa e viene
     * mantenuta distinta da "cancelled".
     */
        m_lastScrubState = QStringLiteral("interrupted");
        qWarning().noquote() << "Btrfs scrub interrupted:" << interruptedMount;

        QTimer::singleShot(250, this, [this, interruptedMount, info]() {
            refreshMonitor();
            if (info.completedPercent >= 0) emit ScrubProgressChanged(interruptedMount, info.completedPercent, info.processedBytes, info.totalBytes, info.speedBytesPerSec, info.errorCount);
            emit ScrubStateChanged(interruptedMount, QStringLiteral("interrupted"), QStringLiteral("Scrub interrupted"));
        });
        return;
    }


    // ============================================================
    // STARTING TRANSIENT
    // ============================================================
    if (info.state == QStringLiteral("not running"))
    {
        /*
         * Lo abbiamo appena osservato realmente:
         *
         * btrfs scrub start ritorna con successo,
         * ma per un breve intervallo "scrub status"
         * può ancora rispondere "not running".
         *
         * Se siamo in starting continuiamo quindi
         * semplicemente a fare polling.
         */
        if (m_lastScrubState == QStringLiteral("starting")) return;
    }

    // ============================================================
    // STATUS ERROR / UNKNOWN
    // ============================================================
    qWarning().noquote() << "Cannot determine active scrub status:" << mountPoint << "state:" << info.state << info.details;
}

bool AgentDbusService::CancelScrub(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path))
    {
        emit ScrubStateChanged(path, QStringLiteral("error"), QStringLiteral("Invalid mount point"));
        return false;
    }

    if (!calledFromDBus())
    {
        qWarning() << "CancelScrub called outside D-Bus";
        return false;
    }

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.scrub.cancel"), path, [this, path]() {
        return cancelScrubAuthorized(path);
    });

    return false;
}

bool AgentDbusService::cancelScrubAuthorized(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) return false;

    if (m_scrubCancelProcess->state() != QProcess::NotRunning)
    {
        qWarning() << "Scrub cancellation already running";
        return false;
    }

    if (m_scrubStartProcess->state() != QProcess::NotRunning)
    {
        qWarning() << "Scrub start command still running";
        return false;
    }

    const ScrubRuntimeInfo current = queryScrubState(path);

    if (current.state != QStringLiteral("running"))
    {
        qWarning().noquote() << "CancelScrub:" << "scrub is not running:" << path << "state:" << current.state;
        return false;
    }

    /*
     * Permette anche di riagganciare un'operazione
     * avviata prima del riavvio del daemon.
     */
    m_activeScrubMount = path;
    m_pendingScrubCancelMount = path;
    m_scrubCancelRequested = true;
    m_lastScrubState = QStringLiteral("stopping");

    m_scrubCancelProcess->setProgram(m_btrfsPath);
    m_scrubCancelProcess->setArguments({QStringLiteral("scrub"), QStringLiteral("cancel"), path});
    m_scrubCancelProcess->start();

    if (!m_scrubCancelProcess->waitForStarted(3000))
    {
        const QString error = m_scrubCancelProcess->errorString();
        m_pendingScrubCancelMount.clear();
        m_scrubCancelRequested = false;

        emit ScrubStateChanged(path, QStringLiteral("error"), error);
        return false;
    }

    if (m_scrubTimer && !m_scrubTimer->isActive())
    {
        m_scrubTimer->start();
    }

    qInfo().noquote() << "Btrfs scrub cancellation requested:" << path;
    emit ScrubStateChanged(path, QStringLiteral("stopping"), QStringLiteral("Scrub cancellation requested"));

    return true;
}

void AgentDbusService::onScrubCancelFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString mountPoint = m_pendingScrubCancelMount;
    const QString standardOutput = QString::fromUtf8(m_scrubCancelProcess->readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromUtf8(m_scrubCancelProcess->readAllStandardError()).trimmed();

    m_pendingScrubCancelMount.clear();

    if (exitStatus != QProcess::NormalExit || exitCode != 0)
    {
        m_scrubCancelRequested = false;
        QString error = standardError;

        if (error.isEmpty()) error = standardOutput;
        if (error.isEmpty()) error = QStringLiteral("btrfs scrub cancel failed");

        qWarning().noquote() << "Btrfs scrub cancellation failed:" << error;

        /*
         * Verifichiamo lo stato reale:
         * il fallimento del comando non significa
         * necessariamente che lo scrub sia terminato.
         */
        const ScrubRuntimeInfo current = queryScrubState(mountPoint);

        if (current.state == QStringLiteral("running"))
        {
            m_lastScrubState = QStringLiteral("running");
            emit ScrubStateChanged(mountPoint, QStringLiteral("running"), QStringLiteral("Scrub cancellation failed: %1").arg(error));
        }
        else
        {
            emit ScrubStateChanged(mountPoint, QStringLiteral("error"), error);
        }

        return;
    }

    qInfo().noquote() << "Btrfs scrub cancellation command completed:" << mountPoint;

    /*
     * Non dichiariamo ancora "cancelled".
     * È pollScrub() che deve verificarlo realmente
     * interrogando Btrfs.
     */
    if (m_activeScrubMount.isEmpty()) m_activeScrubMount = mountPoint;

    if (m_scrubTimer && !m_scrubTimer->isActive()) m_scrubTimer->start();

    /*
     * Proviamo subito, senza aspettare il prossimo
     * tick da 2 secondi.
     */
    pollScrub();
}

bool AgentDbusService::startReplaceAuthorized(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath)
{
    const QString path = mountPoint.trimmed();
    const QString target = targetDevicePath.trimmed();

    if (!validateMountPoint(path) || !validateBtrfsDeviceId(sourceDeviceId) || !validateDevicePath(target) || !m_monitor) return false;

    bool sourcePresent = false;
    if (!m_monitor->btrfsDevicePresence(path, sourceDeviceId, sourcePresent)) { qWarning() << "StartReplace:" << "source devid disappeared before authorization completed"; return false; }

    if (m_replaceStartProcess->state() != QProcess::NotRunning || m_replaceCancelProcess->state() != QProcess::NotRunning) { qWarning() << "StartReplace:" << "replace command already active"; return false; }

    // ============================================================
    // REPLACE GIÀ ATTIVO?
    // ============================================================
    const ReplaceRuntimeInfo currentReplace = queryReplaceState(path);

    if (currentReplace.state == QStringLiteral("running"))
    {
        m_activeReplaceMount = path;
        m_lastReplaceState = QStringLiteral("running");
        m_lastReplacePercent = currentReplace.completedPercent;
        if (!m_replaceTimer->isActive()) m_replaceTimer->start();
        qWarning() << "StartReplace:" << "replace already running";
        return false;
    }

    if (currentReplace.state == QStringLiteral("error")) { qWarning() << "StartReplace:" << "cannot determine current replace state"; return false; }

    // ============================================================
    // ALTRE MANUTENZIONI
    // ============================================================
    const ScrubRuntimeInfo scrub = queryScrubState(path);
    if (scrub.state == QStringLiteral("running")) { qWarning() << "StartReplace:" << "scrub is running"; return false; }

    QString balanceDetails;
    const QString balanceState = queryBalanceState(path, balanceDetails);
    if (balanceState == QStringLiteral("running") || balanceState == QStringLiteral("paused") || balanceState == QStringLiteral("error")) { qWarning() << "StartReplace:" << "balance state prevents replace:" << balanceState; return false; }

    if (!m_pendingRemoveMount.isEmpty() || (m_removeProcess && m_removeProcess->state() != QProcess::NotRunning)) { qWarning() << "StartReplace:" << "device removal is active"; return false; }

    // ============================================================
    // PREPARAZIONE TARGET
    // ============================================================
    QString preparedTarget;
    QString preparationError;
    if (!prepareReplacementTarget(target, preparedTarget, preparationError)) { qWarning().noquote() << "StartReplace:" << preparationError; return false; }

    // ============================================================
    // CONTROLLO DIMENSIONE
    // ============================================================
    quint64 sourceBytes = 0;
    const bool sourceSizeKnown = m_monitor->btrfsDeviceTotalBytes(path, sourceDeviceId, sourceBytes) && sourceBytes > 0;

    quint64 targetBytes = 0;
    QString sizeError;
    if (!blockDeviceSizeBytes(preparedTarget, targetBytes, sizeError)) { qWarning().noquote() << "StartReplace:" << sizeError; return false; }

    if (sourceSizeKnown && targetBytes < sourceBytes) { qWarning().noquote() << "StartReplace:" << "target is smaller than source:" << "source" << sourceBytes << "target" << targetBytes; return false; }

    if (!sourceSizeKnown) { qWarning() << "StartReplace:" << "source size unavailable;" << "Btrfs will validate target size"; }

    // ============================================================
    // START
    // ============================================================
    m_pendingReplaceStartMount = path;
    m_pendingReplaceSourceDeviceId = sourceDeviceId;
    m_pendingReplaceTargetDevicePath = preparedTarget;

    m_replaceCancelRequested = false;
    m_lastReplacePercent = -1;
    m_replaceStatusErrorCount = 0;

    const QString systemdRunPath = QStandardPaths::findExecutable(QStringLiteral("systemd-run"));

    if (systemdRunPath.isEmpty())
    {
        const QString error = QStringLiteral("systemd-run is not available");

        qWarning().noquote() << "StartReplace:" << error;

        emit ReplaceStateChanged(path, QStringLiteral("error"), error);
        return false;
    }

    /*
 * Il replace deve vivere fuori dal cgroup
 * di storage-monitor-agent.service.
 *
 * Con -B btrfs NON fa daemon():
 * rimane in foreground dentro la transient
 * service systemd.
 *
 * In questo modo il restart dell'agent non
 * invia segnali al processo che esegue il replace.
 */
    m_replaceStartProcess->setProgram(systemdRunPath);

    m_replaceStartProcess->setArguments({
        QStringLiteral("--unit=storage-monitor-replace.service"),
        QStringLiteral("--collect"),
        QStringLiteral("--quiet"),
        QStringLiteral("--service-type=exec"),
        m_btrfsPath,
        QStringLiteral("replace"),
        QStringLiteral("start"),
        QStringLiteral("-B"),
        QString::number(sourceDeviceId),
        preparedTarget,
        path
    });

    m_replaceStartProcess->start();

    if (!m_replaceStartProcess->waitForStarted(3000))
    {
        const QString error = m_replaceStartProcess->errorString();
        m_pendingReplaceStartMount.clear();
        m_pendingReplaceSourceDeviceId = 0;
        m_pendingReplaceTargetDevicePath.clear();
        emit ReplaceStateChanged(path, QStringLiteral("error"), error);
        return false;
    }

    emit ReplaceStateChanged(path, QStringLiteral("starting"), QStringLiteral("Device replacement requested"));
    qInfo().noquote() << "Btrfs replace requested:" << "mount:" << path << "source devid:" << sourceDeviceId << "target:" << preparedTarget;

    return true;
}

void AgentDbusService::onReplaceStartFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString mountPoint = m_pendingReplaceStartMount;
    const qulonglong sourceDeviceId = m_pendingReplaceSourceDeviceId;
    const QString targetDevicePath = m_pendingReplaceTargetDevicePath;
    const QString standardOutput = QString::fromUtf8(m_replaceStartProcess->readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromUtf8(m_replaceStartProcess->readAllStandardError()).trimmed();

    m_pendingReplaceStartMount.clear();
    m_pendingReplaceSourceDeviceId = 0;
    m_pendingReplaceTargetDevicePath.clear();


    if (exitStatus != QProcess::NormalExit || exitCode != 0)
    {
        QString error = !standardError.isEmpty() ? standardError : standardOutput;
        if (error.isEmpty()) error = QStringLiteral("btrfs replace start failed");
        qWarning().noquote() << "Btrfs replace start failed:" << error;
        emit ReplaceStateChanged(mountPoint, QStringLiteral("error"), error);
        return;
    }

    if (!m_monitor->beginDeviceReplacement(mountPoint, sourceDeviceId, targetDevicePath, QStringLiteral("Device replacement started")))
    {
        qWarning().noquote() << "Cannot persist device replacement start:" << mountPoint << "source devid:" << sourceDeviceId << "target:"  << targetDevicePath;
    }

    m_activeReplaceMount = mountPoint;
    m_lastReplaceState = QStringLiteral("starting");
    m_lastReplacePercent = -1;
    m_replaceStatusErrorCount = 0;

    if (!m_replaceTimer->isActive()) m_replaceTimer->start();
    QTimer::singleShot(250, this, &AgentDbusService::pollReplace);
}

bool AgentDbusService::CancelReplace(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) return false;
    if (!calledFromDBus()) { qWarning() << "CancelReplace called outside D-Bus"; return false; }

    authorizeAndRun(QStringLiteral("org.storage_monitor.agent.replace.cancel"), path, [this, path]() { return cancelReplaceAuthorized(path); });

    return false;
}

bool AgentDbusService::cancelReplaceAuthorized(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (!validateMountPoint(path)) return false;
    if (m_replaceCancelProcess->state() != QProcess::NotRunning) { qWarning() << "Replace cancellation already running"; return false; }

    const ReplaceRuntimeInfo current = queryReplaceState(path);
    if (current.state != QStringLiteral("running")) { qWarning() << "CancelReplace:" << "replace is not running:" << current.state; return false; }

    m_activeReplaceMount = path;
    m_pendingReplaceCancelMount = path;
    m_replaceCancelRequested = true;
    m_lastReplaceState = QStringLiteral("stopping");

    m_replaceCancelProcess->setProgram(m_btrfsPath);
    m_replaceCancelProcess->setArguments({ QStringLiteral("replace"), QStringLiteral("cancel"), path });
    m_replaceCancelProcess->start();

    if (!m_replaceCancelProcess->waitForStarted(3000))
    {
        const QString error = m_replaceCancelProcess->errorString();
        m_pendingReplaceCancelMount.clear();
        m_replaceCancelRequested = false;
        emit ReplaceStateChanged(path, QStringLiteral("error"), error);
        return false;
    }

    emit ReplaceStateChanged(path, QStringLiteral("stopping"), QStringLiteral("Device replacement cancellation requested"));
    return true;
}

void AgentDbusService::onReplaceCancelFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QString mountPoint = m_pendingReplaceCancelMount;
    const QString standardOutput = QString::fromUtf8(m_replaceCancelProcess->readAllStandardOutput()).trimmed();
    const QString standardError = QString::fromUtf8(m_replaceCancelProcess->readAllStandardError()).trimmed();

    m_pendingReplaceCancelMount.clear();

    if (exitStatus != QProcess::NormalExit || exitCode != 0)
    {
        m_replaceCancelRequested = false;
        const QString error = !standardError.isEmpty() ? standardError : standardOutput;
        emit ReplaceStateChanged(mountPoint, QStringLiteral("error"), error);
        return;
    }

    m_replaceCancelRequested = false;
    if (m_activeReplaceMount == mountPoint) m_activeReplaceMount.clear();

    m_replaceTimer->stop();
    m_lastReplaceState = QStringLiteral("cancelled");
    m_lastReplacePercent = -1;

    QTimer::singleShot(250, this, [this, mountPoint]() {
        const ReplaceRuntimeInfo finalInfo = queryReplaceState(mountPoint);
        QString details = QStringLiteral("Device replacement cancelled");

        if (finalInfo.state == QStringLiteral("cancelled") && !finalInfo.details.trimmed().isEmpty()) { details = finalInfo.details.trimmed(); }

        if (!m_monitor || !m_monitor->finishDeviceReplacement(mountPoint, QStringLiteral("cancelled"), -1, details)) { qWarning().noquote() << "Cannot persist cancelled device replacement:" << mountPoint; }

        refreshMonitor();
        emit ReplaceStateChanged(mountPoint, QStringLiteral("cancelled"), details);
    });
}

void AgentDbusService::pollReplace()
{
    if (m_activeReplaceMount.isEmpty()) { m_replaceTimer->stop(); return; }

    const QString mountPoint = m_activeReplaceMount;
    const ReplaceRuntimeInfo info = queryReplaceState(mountPoint);

    if (info.state == QStringLiteral("running"))
    {
        m_replaceStatusErrorCount = 0;
        bool persistRunningState = false;

        if (info.completedPercent >= 0 && info.completedPercent != m_lastReplacePercent)
        {
            m_lastReplacePercent = info.completedPercent;
            persistRunningState = true;
            emit ReplaceProgressChanged(mountPoint, info.completedPercent);
        }

        if (!m_replaceCancelRequested && m_lastReplaceState != QStringLiteral("running"))
        {
            m_lastReplaceState = QStringLiteral("running");
            persistRunningState = true;
            emit ReplaceStateChanged(mountPoint, QStringLiteral("running"), info.details);
        }

        /* Scriviamo SQLite solamente quando cambia lo stato o la percentuale, non a ogni polling da 2 secondi. */
        if (persistRunningState && m_monitor && !m_monitor->updateDeviceReplacement(mountPoint, QStringLiteral("running"), info.completedPercent, info.details))
        {
            qWarning().noquote() << "Cannot persist running device replacement:" << mountPoint;
        }

        return;
    }

    if (info.state == QStringLiteral("finished")) {
        QString details = info.details.trimmed();

        if (details.isEmpty()) { details = QStringLiteral("Device replacement completed"); }

        if (m_monitor && !m_monitor->finishDeviceReplacement(mountPoint, QStringLiteral("finished"), 100, details)) { qWarning().noquote() << "Cannot persist finished device replacement:" << mountPoint; }

        m_activeReplaceMount.clear();
        m_replaceTimer->stop();
        m_lastReplaceState = QStringLiteral("finished");
        m_lastReplacePercent = 100;

        QTimer::singleShot(250, this, [this, mountPoint, details]() { refreshMonitor(); emit ReplaceProgressChanged(mountPoint, 100); emit ReplaceStateChanged(mountPoint, QStringLiteral("finished"), details); });

        return;
    }


    if (info.state == QStringLiteral("cancelled")) {
        QString details = info.details.trimmed();

        if (details.isEmpty()) { details = QStringLiteral("Device replacement cancelled"); }

        if (m_monitor && !m_monitor->finishDeviceReplacement(mountPoint, QStringLiteral("cancelled"), m_lastReplacePercent, details)) { qWarning().noquote() << "Cannot persist cancelled device replacement:" << mountPoint; }

        m_activeReplaceMount.clear();
        m_replaceTimer->stop();
        m_lastReplaceState = QStringLiteral("cancelled");
        m_lastReplacePercent = -1;

        refreshMonitor();
        emit ReplaceStateChanged(mountPoint, QStringLiteral("cancelled"), details);

        return;
    }

    if (info.state == QStringLiteral("error")) {
        ++m_replaceStatusErrorCount;

        qWarning().noquote() << "Replace status error:" << mountPoint << m_replaceStatusErrorCount << info.details;

        if (m_replaceStatusErrorCount >= 3) {
            QString details = info.details.trimmed();

            if (details.isEmpty()) { details = QStringLiteral("Unable to determine device replacement status"); }

            /* Dopo tre errori consecutivi il daemon considera terminato il proprio tracking del replacement. Persistiamo lo stesso stato terminale che esponiamo via D-Bus. */
            if (m_monitor && !m_monitor->finishDeviceReplacement(mountPoint, QStringLiteral("error"), m_lastReplacePercent, details)) { qWarning().noquote() << "Cannot persist device replacement error:" << mountPoint; }

            m_activeReplaceMount.clear();
            m_replaceTimer->stop();
            m_lastReplaceState = QStringLiteral("error");

            emit ReplaceStateChanged(mountPoint, QStringLiteral("error"), details);
        }

        return;
    }

}
