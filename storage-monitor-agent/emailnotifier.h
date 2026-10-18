// SPDX-License-Identifier: GPL-3.0-or-laterv

#ifndef EMAILNOTIFIER_H
#define EMAILNOTIFIER_H

#include <QObject>
#include <QString>
#include <QByteArray>

class EmailNotifier : public QObject
{
    Q_OBJECT

public:
    explicit EmailNotifier(QObject *parent = nullptr);

    ~EmailNotifier() override = default;

    bool isEnabled() const;
    bool isAvailable() const;

    bool sendNotification(const QString &title, const QString &message, const QString &severity);

private:
    void loadConfiguration();

    QString findSendmailCommand(const QString &configuredCommand) const;

    QByteArray buildMessage(const QString &title, const QString &message, const QString &severity) const;

    static QString sanitizeHeader(const QString &value);

    static QByteArray encodeSubject(const QString &subject);

private:
    QString m_configPath;

    bool m_enabled = false;

    QString m_recipient;
    QString m_sender;

    QString m_commandPath;

    int m_timeoutMs = 15000;
};

#endif // EMAILNOTIFIER_H
