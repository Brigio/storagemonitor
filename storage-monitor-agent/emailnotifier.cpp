// SPDX-License-Identifier: GPL-3.0-or-later

#include "emailnotifier.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>

EmailNotifier::EmailNotifier(QObject *parent) : QObject(parent), m_configPath(QStringLiteral("/etc/storage-monitor-agent/email.conf"))
{
    loadConfiguration();

    if (!m_enabled)
    {
        qInfo()  << "Email notifications disabled";
        return;
    }

    if (m_recipient.isEmpty())
    {
        qWarning() << "⚠ Email notifications enabled but recipient is missing";
        return;
    }

    if (m_sender.isEmpty())
    {
        qWarning() << "⚠ Email notifications enabled but sender is missing";
        return;
    }

    if (m_commandPath.isEmpty())
    {
        qWarning() << "⚠ Email notifications enabled but msmtp/sendmail was not found";
        return;
    }

    qInfo() << "✅ Email notification backend available:" << m_commandPath
        << "recipient:" << m_recipient;
}


// ============================================================
// CONFIGURATION
// ============================================================

void EmailNotifier::loadConfiguration()
{
    QSettings settings(m_configPath, QSettings::IniFormat);

    settings.beginGroup(QStringLiteral("Email"));

    m_enabled = settings.value(QStringLiteral("enabled"), false).toBool();

    m_recipient = settings.value(QStringLiteral("recipient")).toString().trimmed();

    m_sender = settings.value(QStringLiteral("sender")).toString().trimmed();

    const QString configuredCommand = settings.value(QStringLiteral("command")).toString().trimmed();

    m_timeoutMs = settings.value(QStringLiteral("timeout_ms"), 15000).toInt();

    settings.endGroup();


    if (m_timeoutMs < 1000)
    {
        m_timeoutMs = 1000;
    }

    if (m_timeoutMs > 60000)
    {
        m_timeoutMs = 60000;
    }

    m_commandPath = findSendmailCommand(configuredCommand);
}


// ============================================================
// FIND BACKEND
// ============================================================

QString EmailNotifier::findSendmailCommand(const QString &configuredCommand) const
{
    /*
     * Se l'utente ha specificato esplicitamente
     * un comando, usiamo quello.
     */
    if (!configuredCommand.isEmpty())
    {
        QFileInfo info(configuredCommand);

        if (info.isAbsolute())
        {
            if (info.exists() && info.isFile() && info.isExecutable())
            {
                return info.absoluteFilePath();
            }

            return QString();
        }

        const QString found = QStandardPaths::findExecutable(configuredCommand);

        if (!found.isEmpty())
        {
            return found;
        }

        return QString();
    }


    /*
     * Preferiamo msmtp.
     */
    QString command = QStandardPaths::findExecutable(QStringLiteral("msmtp"));

    if (!command.isEmpty())
    {
        return command;
    }


    /*
     * Poi cerchiamo un sendmail compatibile.
     */
    command =  QStandardPaths::findExecutable(QStringLiteral("sendmail"));

    if (!command.isEmpty())
    {
        return command;
    }


    static const QStringList fallbackPaths = {
        QStringLiteral("/usr/sbin/sendmail"),
        QStringLiteral("/usr/bin/sendmail")
    };

    for (const QString &path : fallbackPaths)
    {
        QFileInfo info(path);

        if (info.exists() && info.isFile() && info.isExecutable())
        {
            return path;
        }
    }

    return QString();
}


// ============================================================
// STATUS
// ============================================================

bool EmailNotifier::isEnabled() const
{
    return m_enabled;
}


bool EmailNotifier::isAvailable() const
{
    return
        m_enabled &&
        !m_recipient.isEmpty() &&
        !m_sender.isEmpty() &&
        !m_commandPath.isEmpty();
}


// ============================================================
// HEADER HELPERS
// ============================================================

QString EmailNotifier::sanitizeHeader(const QString &value)
{
    QString result = value;

    result.replace(QLatin1Char('\r'), QLatin1Char(' '));

    result.replace(QLatin1Char('\n'), QLatin1Char(' '));

    return result.trimmed();
}


QByteArray EmailNotifier::encodeSubject(
    const QString &subject)
{
    /*
     * I titoli delle nostre notifiche contengono anche
     * caratteri UTF-8 ed emoji.
     *
     * Usiamo quindi MIME encoded-word.
     */
    const QByteArray encoded = subject.toUtf8().toBase64();

    return
        QByteArrayLiteral("=?UTF-8?B?") + encoded + QByteArrayLiteral("?=");
}


// ============================================================
// BUILD EMAIL
// ============================================================

QByteArray EmailNotifier::buildMessage(
    const QString &title,
    const QString &message,
    const QString &severity) const
{
    const QString host =
        QSysInfo::machineHostName();

    const QDateTime now =
        QDateTime::currentDateTime();

    const QString safeTitle =
        sanitizeHeader(title);

    const QString safeSeverity =
        sanitizeHeader(severity)
            .toUpper();


    // ========================================================
    // SUBJECT
    // ========================================================

    /*
     * Manteniamo le emoji nelle notifiche desktop,
     * ma le rimuoviamo dall'oggetto email.
     *
     * L'oggetto risulta così più tradizionale:
     *
     * [cachyos][WARNING] BTRFS Warning
     * [cachyos][CRITICAL] SMART CRITICAL
     */
    QString subjectTitle =
        safeTitle;

    subjectTitle.remove(
        QStringLiteral("⚠️"));

    subjectTitle.remove(
        QStringLiteral("⚠"));

    subjectTitle.remove(
        QStringLiteral("❌"));

    subjectTitle.remove(
        QStringLiteral("✅"));

    subjectTitle =
        subjectTitle.trimmed();


    const QString subject =
        QStringLiteral("[%1][%2] %3")
            .arg(
                host,
                safeSeverity,
                subjectTitle);


    // ========================================================
    // MESSAGE-ID
    // ========================================================

    /*
     * Generiamo un Message-ID univoco.
     *
     * Come dominio utilizziamo quello del mittente configurato.
     */
    QString messageIdDomain =
        m_sender.section(
                    QLatin1Char('@'),
                    1,
                    1)
            .trimmed();

    if (messageIdDomain.isEmpty())
    {
        messageIdDomain =
            QStringLiteral("localhost");
    }


    const QString messageId =
        QStringLiteral("<%1@%2>")
            .arg(
                QUuid::createUuid()
                    .toString(
                        QUuid::WithoutBraces),
                messageIdDomain);


    // ========================================================
    // NORMALIZZA BODY
    // ========================================================

    QString bodyMessage =
        message;

    /*
     * Prima normalizziamo eventuali CRLF/CR,
     * poi ricreiamo righe RFC-style CRLF.
     */
    bodyMessage.replace(
        QStringLiteral("\r\n"),
        QStringLiteral("\n"));

    bodyMessage.replace(
        QLatin1Char('\r'),
        QLatin1Char('\n'));

    bodyMessage.replace(
        QStringLiteral("\n"),
        QStringLiteral("\r\n"));


    // ========================================================
    // COSTRUZIONE EMAIL
    // ========================================================

    QByteArray mail;


    // --------------------------------------------------------
    // FROM
    // --------------------------------------------------------

    mail += "From: ";
    mail +=
        sanitizeHeader(
            m_sender)
            .toUtf8();

    mail += "\r\n";


    // --------------------------------------------------------
    // TO
    // --------------------------------------------------------

    mail += "To: ";
    mail +=
        sanitizeHeader(
            m_recipient)
            .toUtf8();

    mail += "\r\n";


    // --------------------------------------------------------
    // SUBJECT
    // --------------------------------------------------------

    /*
     * Dopo la rimozione delle emoji i nostri subject
     * sono normalmente ASCII.
     *
     * encodeSubject() rimane comunque utile nel caso
     * hostname o titolo contengano caratteri non ASCII.
     */
    bool subjectIsAscii = true;

    for (const QChar ch : subject)
    {
        if (ch.unicode() > 127)
        {
            subjectIsAscii = false;
            break;
        }
    }


    mail += "Subject: ";

    if (subjectIsAscii)
    {
        mail += subject.toUtf8();
    }
    else
    {
        mail += encodeSubject(subject);
    }

    mail += "\r\n";


    // --------------------------------------------------------
    // DATE
    // --------------------------------------------------------

    mail += "Date: ";

    mail +=
        now.toString(
               Qt::RFC2822Date)
            .toUtf8();

    mail += "\r\n";


    // --------------------------------------------------------
    // MESSAGE-ID
    // --------------------------------------------------------

    mail += "Message-ID: ";
    mail += messageId.toUtf8();
    mail += "\r\n";


    // --------------------------------------------------------
    // AUTOMATED MESSAGE HEADERS
    // --------------------------------------------------------

    mail +=
        "Auto-Submitted: auto-generated\r\n";

    mail +=
        "X-Mailer: storage-monitor-agent\r\n";

    mail +=
        "X-Storage-Monitor-Severity: ";

    mail += safeSeverity.toUtf8();
    mail += "\r\n";


    // --------------------------------------------------------
    // MIME
    // --------------------------------------------------------

    mail += "MIME-Version: 1.0\r\n";

    mail +=
        "Content-Type: text/plain; "
        "charset=UTF-8\r\n";

    mail +=
        "Content-Transfer-Encoding: 8bit\r\n";


    /*
     * Fine degli header.
     */
    mail += "\r\n";


    // ========================================================
    // BODY
    // ========================================================

    mail +=
        "Storage Monitor Agent\r\n"
        "\r\n";


    mail += "Host: ";
    mail += host.toUtf8();
    mail += "\r\n";


    mail += "Severity: ";
    mail += safeSeverity.toUtf8();
    mail += "\r\n";


    mail += "Time: ";

    mail +=
        now.toString(
               Qt::ISODate)
            .toUtf8();

    mail += "\r\n"
            "\r\n";


    mail += "Event:\r\n";
    mail += subjectTitle.toUtf8();

    mail += "\r\n"
            "\r\n";


    mail += "Details:\r\n";
    mail += bodyMessage.toUtf8();

    mail += "\r\n";


    return mail;
}

// ============================================================
// SEND
// ============================================================

bool EmailNotifier::sendNotification(const QString &title, const QString &message, const QString &severity)
{
    /*
     * Email disabilitata:
     * non è un errore.
     */
    if (!m_enabled)
    {
        return true;
    }

    if (!isAvailable())
    {
        qWarning() << "⚠ Cannot send email notification:" << "email backend is not available";
        return false;
    }


    QStringList arguments;

    const QString executableName = QFileInfo(m_commandPath)
            .fileName()
            .toLower();


    if (executableName.contains(
            QStringLiteral("msmtp")))
    {
        /*
         * msmtp in modalità sendmail.
         *
         * -t:
         * legge il destinatario dal campo To:
         */
        arguments
            << QStringLiteral("-t");
    }
    else
    {
        /*
         * sendmail compatibile.
         *
         * -t:
         * legge To:
         *
         * -i:
         * non interpreta una riga contenente solo "."
         * come terminatore del messaggio.
         */
        arguments
            << QStringLiteral("-t")
            << QStringLiteral("-i");
    }

    const QByteArray mail = buildMessage(title, message, severity);

    QProcess process;

    process.start(m_commandPath, arguments, QIODevice::ReadWrite);

    if (!process.waitForStarted(5000))
    {
        qWarning() << "❌ Cannot start email backend:" << m_commandPath;

        return false;
    }

    if (process.write(mail) < 0)
    {
        qWarning() << "❌ Cannot write email message";

        process.kill();
        process.waitForFinished();

        return false;
    }

    process.closeWriteChannel();

    if (!process.waitForFinished(m_timeoutMs))
    {
        qWarning() << "❌ Email backend timeout";

        process.kill();
        process.waitForFinished();

        return false;
    }

    const QByteArray standardError = process.readAllStandardError();

    if (process.exitStatus() != QProcess::NormalExit)
    {
        qWarning() << "❌ Email backend crashed:" << standardError;
        return false;
    }


    if (process.exitCode() != 0)
    {
        qWarning() << "❌ Email backend failed" << "exit:" << process.exitCode() << "error:" << QString::fromUtf8(standardError);
        return false;
    }

    qInfo() << "📧 Email notification sent to" << m_recipient;

    return true;
}