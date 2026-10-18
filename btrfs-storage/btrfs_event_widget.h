// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef BTRFS_EVENT_WIDGET_H
#define BTRFS_EVENT_WIDGET_H

#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>

#include "btrfseventmanager.h"

class AgentDbusClient;

class BtrfsEventWidget : public QWidget
{
    Q_OBJECT

public:
    explicit BtrfsEventWidget(BtrfsEventManager *eventManager, AgentDbusClient *agentDbusClient, QWidget *parent = nullptr);
    ~BtrfsEventWidget();

private slots:
    void refreshEventList();
    void onEventsChanged();
    void onAcknowledgeClicked();
    void onResolveClicked();
    void onClearHistoryClicked();
    void onItemDoubleClicked(QListWidgetItem *item);
    void showEventDetails(const BtrfsEvent &event);
    void updateBadge();
    void onAcknowledgeFinished(const QString &source, qulonglong eventId, bool accepted, const QString &errorMessage);
    void onResolveFinished(const QString &source, qulonglong eventId, bool accepted, const QString &errorMessage);
    void onClearEventHistoryFinished(bool accepted, const QString &errorMessage);

private:
    void setupUI();
    void loadEvents();
    QWidget* createEventItem(const BtrfsEvent &event);
    QString eventTypeText(const BtrfsEvent &event) const;
    QString eventIcon(const BtrfsEvent &event) const;
    QString getEventIcon(BtrfsEventType type) const;
    QString getSeverityColor(BtrfsEventSeverity severity);
    QString getSeverityIcon(BtrfsEventSeverity severity);

    BtrfsEventManager *m_eventManager;

    AgentDbusClient *m_agentDbusClient = nullptr;
    bool m_acknowledgePending = false;
    bool m_resolvePending = false;
    bool m_clearHistoryPending = false;

    QListWidget *m_eventList;
    QLabel *m_badgeLabel;
    QPushButton *m_acknowledgeBtn;
    QPushButton *m_resolveBtn;
    QPushButton *m_clearBtn;
    QPushButton *m_refreshBtn;

    QHash<QString, QListWidgetItem*> m_eventItems;
    BtrfsEvent m_selectedEvent;
};

#endif // BTRFS_EVENT_WIDGET_H
