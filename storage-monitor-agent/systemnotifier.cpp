// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemnotifier.h"

#include <QDebug>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QMap>


SystemNotifier::SystemNotifier(QObject *parent) : QObject(parent), m_available(false), m_appName(QStringLiteral("storage-monitor-agent"))
{
    /*
     * Trova gli eseguibili necessari.
     *
     * Non usiamo "which": QStandardPaths::findExecutable()
     * è il metodo Qt appropriato.
     */
    m_notifySendPath = QStandardPaths::findExecutable(QStringLiteral("notify-send"));

    m_loginctlPath = QStandardPaths::findExecutable(QStringLiteral("loginctl"));

    m_runuserPath = QStandardPaths::findExecutable(QStringLiteral("runuser"));

    m_envPath = QStandardPaths::findExecutable(QStringLiteral("env"));

    if (m_notifySendPath.isEmpty()) {
        qWarning().noquote() << "notify-send non trovato";
    }

    if (m_loginctlPath.isEmpty()) {
        qWarning().noquote() << "loginctl non trovato";
    }

    if (m_runuserPath.isEmpty()) {
        qWarning().noquote() << "runuser non trovato";
    }

    if (m_envPath.isEmpty()) {
        qWarning().noquote() << "env non trovato";
    }

    m_available =
        !m_notifySendPath.isEmpty() &&
        !m_loginctlPath.isEmpty() &&
        !m_runuserPath.isEmpty() &&
        !m_envPath.isEmpty();

    if (m_available) {
        qInfo().noquote() << "Desktop notification backend disponibile";
    } else {
        qWarning().noquote() << "Desktop notification backend non disponibile";
    }

    /*
     * L'icona rimane incorporata nel binario:
     *
     *     :/icons/app.png
     *
     * Viene materializzata solamente durante il runtime
     * perché notify-send necessita di un vero path filesystem.
     */
    m_appIcon = extractAppIcon();
}


SystemNotifier::~SystemNotifier()
{
    cleanupTempIcon();
}


QString SystemNotifier::extractAppIcon()
{
    /*
     * Se è già stata estratta, riutilizzala.
     */
    if (!m_tempIconPath.isEmpty() && QFileInfo::exists(m_tempIconPath)) {
        return m_tempIconPath;
    }

    QFile resourceFile(QStringLiteral(":/icons/app.png"));

    if (!resourceFile.exists()) {
        qWarning().noquote() << "Resource icona non trovata:" << ":/icons/app.png";
        return QString();
    }

    if (!resourceFile.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "Impossibile aprire la resource:" << ":/icons/app.png";
        return QString();
    }

    const QByteArray iconData = resourceFile.readAll();

    resourceFile.close();

    if (iconData.isEmpty()) {
        qWarning().noquote() << "Resource icona vuota";
        return QString();
    }

    /*
     * Directory runtime del daemon.
     *
     * Con systemd verrà normalmente creata tramite:
     *
     * RuntimeDirectory=storage-monitor-agent
     *
     * Questo fallback permette comunque di eseguire
     * manualmente il programma come root.
     */
    const QString runtimeDir = QStringLiteral("/run/storage-monitor-agent");

    QDir dir;

    if (!dir.mkpath(runtimeDir)) {
        qWarning().noquote()
        << "Impossibile creare:"
        << runtimeDir;

        return QString();
    }


    /*
     * Il processo notify-send verrà eseguito come
     * utente desktop, quindi la directory deve essere
     * attraversabile e l'icona leggibile.
     */
    QFile::setPermissions(
        runtimeDir,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner |
            QFileDevice::ReadGroup |
            QFileDevice::ExeGroup |
            QFileDevice::ReadOther |
            QFileDevice::ExeOther
        );


    m_tempIconPath =
        runtimeDir +
        QStringLiteral(
            "/notification-icon.png");


    QFile outFile(m_tempIconPath);

    if (!outFile.open(
            QIODevice::WriteOnly |
            QIODevice::Truncate)) {

        qWarning().noquote()
        << "Impossibile creare icona runtime:"
        << m_tempIconPath;

        m_tempIconPath.clear();

        return QString();
    }


    const qint64 bytesWritten = outFile.write(iconData);

    outFile.close();


    if (bytesWritten != iconData.size()) {

        qWarning().noquote() << "Scrittura incompleta icona:" << bytesWritten << "/" << iconData.size();

        QFile::remove(m_tempIconPath);

        m_tempIconPath.clear();

        return QString();
    }

    /*
     * Root può scrivere.
     * L'utente desktop può leggere.
     */
    QFile::setPermissions(m_tempIconPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);

    qInfo().noquote() << "Icona notifiche caricata dalla resource interna:" << m_tempIconPath;

    return m_tempIconPath;
}


void SystemNotifier::cleanupTempIcon()
{
    if (m_tempIconPath.isEmpty()) {
        return;
    }


    if (QFileInfo::exists(m_tempIconPath)) {

        if (!QFile::remove(m_tempIconPath)) {

            qWarning().noquote() << "Impossibile rimuovere icona runtime:" << m_tempIconPath;
        }
    }

    m_tempIconPath.clear();
}


bool SystemNotifier::isAvailable() const
{
    return m_available;
}


QString SystemNotifier::getPriorityIcon(NotificationPriority priority) const
{
    /*
     * Priorità assoluta all'icona incorporata
     * nell'applicazione.
     */
    if (!m_appIcon.isEmpty() && QFileInfo::exists(m_appIcon)) {
        return m_appIcon;
    }

    /*
     * Se per qualche motivo la resource non fosse
     * disponibile, usa un'icona del tema desktop.
     */
    switch (priority) {

    case Critical:
        return QStringLiteral("dialog-error");

    case Normal:
        return QStringLiteral("dialog-warning");

    case Low:
        return QStringLiteral("dialog-information");

    default:
        return QStringLiteral("drive-harddisk");
    }
}


bool SystemNotifier::findActiveGraphicalSession(QString &userName, QString &uid) const
{
    userName.clear();
    uid.clear();


    if (m_loginctlPath.isEmpty()) {
        return false;
    }


    QProcess listProcess;

    listProcess.start(
        m_loginctlPath,
        QStringList() << QStringLiteral("list-sessions") << QStringLiteral("--no-legend") << QStringLiteral("--no-pager")
        );


    if (!listProcess.waitForStarted(1000)) {

        qWarning().noquote() << "Impossibile avviare loginctl";
        return false;
    }


    if (!listProcess.waitForFinished(3000)) {

        qWarning().noquote() << "Timeout loginctl list-sessions";

        listProcess.kill();
        listProcess.waitForFinished();

        return false;
    }


    if (listProcess.exitStatus() != QProcess::NormalExit || listProcess.exitCode() != 0) {

        qWarning().noquote() << "loginctl list-sessions fallito:" << listProcess.readAllStandardError().trimmed();
        return false;
    }


    const QString output = QString::fromUtf8(listProcess.readAllStandardOutput());

    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    for (const QString &rawLine : lines) {

        const QString line = rawLine.simplified();

        if (line.isEmpty()) {
            continue;
        }

        const QStringList fields = line.split(' ', Qt::SkipEmptyParts);

        if (fields.isEmpty()) {
            continue;
        }


        /*
         * Il primo campo di list-sessions
         * è l'ID della sessione.
         */
        const QString sessionId =
            fields.first();


        QProcess showProcess;

        QStringList args;

        args
            << QStringLiteral("show-session")
            << sessionId
            << QStringLiteral("--no-pager")
            << QStringLiteral("-p")
            << QStringLiteral("Active")
            << QStringLiteral("-p")
            << QStringLiteral("Remote")
            << QStringLiteral("-p")
            << QStringLiteral("Type")
            << QStringLiteral("-p")
            << QStringLiteral("Class")
            << QStringLiteral("-p")
            << QStringLiteral("User")
            << QStringLiteral("-p")
            << QStringLiteral("Name");


        showProcess.start(
            m_loginctlPath,
            args);


        if (!showProcess.waitForStarted(1000)) {
            continue;
        }


        if (!showProcess.waitForFinished(2000)) {

            showProcess.kill();
            showProcess.waitForFinished();

            continue;
        }


        if (showProcess.exitStatus() !=
                QProcess::NormalExit ||
            showProcess.exitCode() != 0) {

            continue;
        }


        const QString sessionOutput =
            QString::fromUtf8(
                showProcess
                    .readAllStandardOutput());


        QMap<QString, QString> properties;


        const QStringList propertyLines =
            sessionOutput.split(
                '\n',
                Qt::SkipEmptyParts);


        for (const QString &propertyLine :
             propertyLines) {

            const qsizetype separator =
                propertyLine.indexOf('=');

            if (separator <= 0) {
                continue;
            }


            const QString key =
                propertyLine
                    .left(separator)
                    .trimmed();

            const QString value =
                propertyLine
                    .mid(separator + 1)
                    .trimmed();


            properties.insert(
                key,
                value);
        }


        const QString active =
            properties.value(
                QStringLiteral("Active"));

        const QString remote =
            properties.value(
                QStringLiteral("Remote"));

        const QString type =
            properties.value(
                QStringLiteral("Type"));

        const QString sessionClass =
            properties.value(
                QStringLiteral("Class"));

        const QString sessionUid =
            properties.value(
                QStringLiteral("User"));

        const QString sessionUser =
            properties.value(
                QStringLiteral("Name"));


        /*
         * Vogliamo esclusivamente una sessione:
         *
         * - attiva
         * - locale
         * - grafica
         * - appartenente ad un normale utente
         */
        const bool graphical =
            type == QStringLiteral("x11") ||
            type == QStringLiteral("wayland");


        if (active != QStringLiteral("yes")) {
            continue;
        }

        if (remote == QStringLiteral("yes")) {
            continue;
        }

        if (!graphical) {
            continue;
        }

        if (sessionClass !=
            QStringLiteral("user")) {

            continue;
        }

        if (sessionUid.isEmpty() ||
            sessionUid ==
                QStringLiteral("0")) {

            continue;
        }

        if (sessionUser.isEmpty()) {
            continue;
        }


        /*
         * Verifica che il session bus esista.
         */
        const QString busPath =
            QStringLiteral(
                "/run/user/%1/bus")
                .arg(sessionUid);


        if (!QFileInfo::exists(busPath)) {

            qDebug().noquote()
            << "Session bus non disponibile:"
            << busPath;

            continue;
        }


        userName = sessionUser;
        uid = sessionUid;


        qDebug().noquote()
            << "Sessione grafica attiva:"
            << "session=" << sessionId
            << "user=" << userName
            << "uid=" << uid
            << "type=" << type;


        return true;
    }


    return false;
}


void SystemNotifier::sendFallbackNotification(
    const QString &title,
    const QString &message)
{
    qInfo().noquote()
    << "[NOTIFICATION FALLBACK]"
    << title
    << "-"
    << message;
}


void SystemNotifier::sendNotification(
    const QString &title,
    const QString &message,
    NotificationPriority priority,
    int timeout)
{
    if (title.isEmpty() &&
        message.isEmpty()) {

        return;
    }


    QString priorityStr;

    switch (priority) {

    case Critical:
        priorityStr =
            QStringLiteral("CRITICAL");
        break;

    case Normal:
        priorityStr =
            QStringLiteral("NORMAL");
        break;

    case Low:
        priorityStr =
            QStringLiteral("LOW");
        break;

    default:
        priorityStr =
            QStringLiteral("UNKNOWN");
        break;
    }


    qInfo().noquote()
        << QStringLiteral(
               "[%1] %2: %3")
               .arg(
                   priorityStr,
                   title,
                   message);


    if (!m_available) {

        sendFallbackNotification(
            title,
            message);

        return;
    }


    QString userName;
    QString uid;


    if (!findActiveGraphicalSession(
            userName,
            uid)) {

        qWarning().noquote()
        << "Nessuna sessione grafica "
           "utente attiva; "
           "notifica desktop non inviata";

        sendFallbackNotification(
            title,
            message);

        return;
    }


    QString urgency;
    int actualTimeout = timeout;


    switch (priority) {

    case Critical:

        urgency =
            QStringLiteral("critical");

        /*
         * NOTIFY_EXPIRES_NEVER = 0.
         */
        actualTimeout = 0;

        break;


    case Normal:

        urgency =
            QStringLiteral("normal");

        break;


    case Low:

        urgency =
            QStringLiteral("low");

        break;


    default:

        urgency =
            QStringLiteral("normal");

        break;
    }


    const QString iconPath =
        getPriorityIcon(priority);


    QStringList notifyArgs;

    notifyArgs
        << QStringLiteral("-a")
        << m_appName
        << QStringLiteral("-u")
        << urgency
        << QStringLiteral("-t")
        << QString::number(actualTimeout);


    if (!iconPath.isEmpty()) {

        notifyArgs
            << QStringLiteral("-i")
            << iconPath;
    }


    /*
     * Categoria standard freedesktop.
     */
    if (priority == Critical) {

        notifyArgs
            << QStringLiteral("-c")
            << QStringLiteral("device.error");

    } else {

        notifyArgs
            << QStringLiteral("-c")
            << QStringLiteral("device");
    }


    notifyArgs
        << title
        << message;


    /*
     * Il servizio principale gira come root.
     *
     * notify-send deve invece essere eseguito
     * come utente della sessione grafica e sul
     * suo D-Bus session bus.
     *
     * Equivalentemente:
     *
     * runuser -u USER -- env
     *   DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/UID/bus
     *   XDG_RUNTIME_DIR=/run/user/UID
     *   notify-send ...
     */
    QStringList runArgs;

    runArgs
        << QStringLiteral("-u")
        << userName
        << QStringLiteral("--")
        << m_envPath
        << QStringLiteral(
               "DBUS_SESSION_BUS_ADDRESS="
               "unix:path=/run/user/%1/bus")
               .arg(uid)
        << QStringLiteral(
               "XDG_RUNTIME_DIR="
               "/run/user/%1")
               .arg(uid)
        << m_notifySendPath;


    runArgs.append(notifyArgs);


    qDebug().noquote()
        << "Invio notifica desktop a:"
        << userName
        << "(uid"
        << uid + ")";


    QProcess process;

    process.start(
        m_runuserPath,
        runArgs);


    if (!process.waitForStarted(1000)) {

        qWarning().noquote()
        << "Impossibile avviare notify-send:"
        << process.errorString();

        sendFallbackNotification(
            title,
            message);

        return;
    }


    if (!process.waitForFinished(3000)) {

        qWarning().noquote()
        << "Timeout durante notify-send";

        process.kill();
        process.waitForFinished();

        sendFallbackNotification(
            title,
            message);

        return;
    }


    if (process.exitStatus() ==
            QProcess::NormalExit &&
        process.exitCode() == 0) {

        qInfo().noquote()
        << "Notifica desktop inviata a"
        << userName;

        return;
    }


    const QString error =
        QString::fromUtf8(
            process
                .readAllStandardError())
            .trimmed();


    qWarning().noquote()
        << "Errore notify-send:"
        << error;


    sendFallbackNotification(
        title,
        message);
}