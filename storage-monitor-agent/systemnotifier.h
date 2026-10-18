// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef SYSTEMNOTIFIER_H
#define SYSTEMNOTIFIER_H

#include <QObject>
#include <QString>

class SystemNotifier : public QObject
{
    Q_OBJECT

public:
    enum NotificationPriority {
        Low = 0,
        Normal = 1,
        Critical = 2
    };
    Q_ENUM(NotificationPriority)

    explicit SystemNotifier(QObject *parent = nullptr);
    ~SystemNotifier() override;

    void sendNotification(
        const QString &title,
        const QString &message,
        NotificationPriority priority = Normal,
        int timeout = 5000
        );

    bool isAvailable() const;

private:
    bool m_available;

    QString m_appName;
    QString m_appIcon;
    QString m_tempIconPath;

    QString m_notifySendPath;
    QString m_loginctlPath;
    QString m_runuserPath;
    QString m_envPath;

    QString getPriorityIcon(NotificationPriority priority) const;

    QString extractAppIcon();
    void cleanupTempIcon();

    void sendFallbackNotification(
        const QString &title,
        const QString &message
        );

    bool findActiveGraphicalSession(
        QString &userName,
        QString &uid
        ) const;
};

#endif // SYSTEMNOTIFIER_H