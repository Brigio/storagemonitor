// SPDX-License-Identifier: GPL-3.0-or-later

#include "btrfs_event_widget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QDateTime>
#include <QStyle>
#include <QMenu>
#include <QStringList>
#include "agentdbusclient.h"
#include <QBrush>
#include <QColor>
#include <QPalette>

BtrfsEventWidget::BtrfsEventWidget(BtrfsEventManager *eventManager, AgentDbusClient *agentDbusClient, QWidget *parent)
    : QWidget(parent)
    , m_eventManager(eventManager)
    , m_agentDbusClient(agentDbusClient)
{
    setupUI();

    // Connessioni segnali
    connect(m_eventManager,  &BtrfsEventManager::eventsChanged, this, &BtrfsEventWidget::onEventsChanged);

    if (m_agentDbusClient)
    {
        connect(m_agentDbusClient, &AgentDbusClient::acknowledgeEventFinished, this, &BtrfsEventWidget::onAcknowledgeFinished);

        connect(m_agentDbusClient, &AgentDbusClient::resolveEventFinished, this, &BtrfsEventWidget::onResolveFinished);

        connect(m_agentDbusClient, &AgentDbusClient::clearEventHistoryFinished, this, &BtrfsEventWidget::onClearEventHistoryFinished);
    }

    // Carica eventi iniziali
    loadEvents();
    updateBadge();
}

// Il distruttore
BtrfsEventWidget::~BtrfsEventWidget() = default;

void BtrfsEventWidget::setupUI()
{
    setMinimumSize(600, 400);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // ============================================================
    // HEADER
    // ============================================================
    QHBoxLayout *headerLayout = new QHBoxLayout();

    QLabel *titleLabel = new QLabel("📋 Storage Event Log", this);
    titleLabel->setStyleSheet("font-size: 16px; font-weight: bold;");

    m_badgeLabel = new QLabel(this);
    m_badgeLabel->setStyleSheet("font-weight: bold; padding: 2px 8px; border-radius: 10px;");

    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(m_badgeLabel);

    mainLayout->addLayout(headerLayout);

    // ============================================================
    // TOOLBAR
    // ============================================================
    QHBoxLayout *toolbarLayout = new QHBoxLayout();

    m_refreshBtn = new QPushButton("🔄 Refresh", this);
    m_acknowledgeBtn = new QPushButton("✓ Acknowledge", this);
    m_resolveBtn = new QPushButton("✕ Resolve", this);
    m_clearBtn = new QPushButton("🗑 Clear History", this);

    m_acknowledgeBtn->setEnabled(false);
    m_resolveBtn->setEnabled(false);

    toolbarLayout->addWidget(m_refreshBtn);
    toolbarLayout->addWidget(m_acknowledgeBtn);
    toolbarLayout->addWidget(m_resolveBtn);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(m_clearBtn);

    mainLayout->addLayout(toolbarLayout);

    // ============================================================
    // EVENT LIST
    // ============================================================
    m_eventList = new QListWidget(this);
    m_eventList->setAlternatingRowColors(true);
    m_eventList->setWordWrap(true);
    m_eventList->setSpacing(2);

    mainLayout->addWidget(m_eventList);

    // ============================================================
    // CONNESSIONI
    // ============================================================
    connect(m_eventList, &QListWidget::itemDoubleClicked, this, &BtrfsEventWidget::onItemDoubleClicked);

    connect(m_eventList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *previous)
        {
            Q_UNUSED(previous);

            m_selectedEvent = BtrfsEvent();

            m_acknowledgeBtn->setEnabled(false);
            m_resolveBtn->setEnabled(false);

            if (!current)
                return;

            const QVariant eventData = current->data(Qt::UserRole);

            if (!eventData.canConvert<BtrfsEvent>())
                return;

            m_selectedEvent = eventData.value<BtrfsEvent>();

        /*Acknowledge: solo evento ancora attivo e non già acknowledged.*/
        m_acknowledgeBtn->setEnabled(!m_acknowledgePending && m_selectedEvent.active && !m_selectedEvent.acknowledged);

        /*Resolve: per ora lo lasciamo disponibile solamente sugli eventi attivi.*/
            m_resolveBtn->setEnabled(m_selectedEvent.active);
        });

    connect(m_refreshBtn, &QPushButton::clicked, this, &BtrfsEventWidget::refreshEventList);
    connect(m_acknowledgeBtn, &QPushButton::clicked, this, &BtrfsEventWidget::onAcknowledgeClicked);
    connect(m_resolveBtn, &QPushButton::clicked, this, &BtrfsEventWidget::onResolveClicked);
    connect(m_clearBtn, &QPushButton::clicked, this, &BtrfsEventWidget::onClearHistoryClicked);
}

void BtrfsEventWidget::loadEvents()
{
    m_eventList->clear();
    m_eventItems.clear();

    m_selectedEvent = BtrfsEvent();

    m_acknowledgeBtn->setEnabled(false);
    m_resolveBtn->setEnabled(false);

    if (!m_eventManager || !m_eventManager->isOpen() || (!m_eventManager->hasEventsTable() && !m_eventManager->hasSmartEventsTable()))
    {
        QListWidgetItem *unavailableItem = new QListWidgetItem(QStringLiteral("⚠ Monitoring events unavailable"));

        unavailableItem->setTextAlignment(Qt::AlignCenter);

        unavailableItem->setFlags(unavailableItem->flags() & ~Qt::ItemIsSelectable);

        m_eventList->addItem(unavailableItem);

        m_badgeLabel->setText(QStringLiteral("⚠ Unavailable"));

        return;
    }

    const QList<BtrfsEvent> events = m_eventManager->recentEvents(100);

    if (events.isEmpty())
    {
        QListWidgetItem *emptyItem = new QListWidgetItem(QStringLiteral("✅ No events recorded"));

        emptyItem->setTextAlignment(Qt::AlignCenter);

        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsSelectable);

        m_eventList->addItem(emptyItem);

        updateBadge();
        return;
    }

    for (const BtrfsEvent &event : std::as_const(events))
    {
        QWidget *itemWidget = createEventItem(event);

        QListWidgetItem *item = new QListWidgetItem(m_eventList);

        item->setSizeHint(itemWidget->sizeHint());

        const QColor baseColor = m_eventList->palette().color(QPalette::Base);

        const QColor severityColor = QColor(getSeverityColor(event.severity));

        const int tintPercent = event.active ? 18 : 8;

        const QColor backgroundColor(
            (baseColor.red()   * (100 - tintPercent) + severityColor.red()   * tintPercent) / 100,
            (baseColor.green() * (100 - tintPercent) + severityColor.green() * tintPercent) / 100,
            (baseColor.blue()  * (100 - tintPercent) + severityColor.blue()  * tintPercent) / 100
            );

        item->setBackground(QBrush(backgroundColor));
        item->setData(Qt::UserRole, QVariant::fromValue(event));

        m_eventList->setItemWidget(item, itemWidget);

        const QString sourceKey = event.source == StorageEventSource::Smart
                ? QStringLiteral("smart") : QStringLiteral("btrfs");

        const QString itemKey = sourceKey + QChar(':') + QString::number(event.id);

        m_eventItems[itemKey] = item;
    }

    updateBadge();
}

QString BtrfsEventWidget::eventTypeText(const BtrfsEvent &event) const
{
    /*
     * Per gli eventi Btrfs già conosciuti
     * manteniamo il testo esistente.
     */
    if (event.source == StorageEventSource::Btrfs && event.type != BtrfsEventType::Unknown)
    {
        return BtrfsEventManager::eventTypeToString(event.type);
    }

    /*
     * Per SMART e per eventuali tipi Btrfs
     * non presenti nell'enum utilizziamo
     * direttamente la chiave persistita.
     */
    QStringList words = event.eventTypeKey.split(QChar('_'), Qt::SkipEmptyParts);

    for (QString &word : words)
    {
        if (!word.isEmpty())
        {
            word[0] = word[0].toUpper();
        }
    }

    QString text = words.join(QChar(' '));

    /*
     * Acronimi.
     */
    if (text.startsWith(QStringLiteral("Smart ")))
    {
        text.replace(0, 5, QStringLiteral("SMART "));
    }

    if (text.startsWith(QStringLiteral("Raid ")))
    {
        text.replace(0, 4, QStringLiteral("RAID "));
    }

    if (text.isEmpty())
    {
        return QStringLiteral("Unknown");
    }

    return text;
}

QString BtrfsEventWidget::eventIcon(const BtrfsEvent &event) const
{
    if (event.source == StorageEventSource::Smart)
    {
        return QStringLiteral("💽");
    }

    if (event.eventTypeKey == QStringLiteral("raid_imbalance"))
    {
        return QStringLiteral("⚖️");
    }

    return getEventIcon(event.type);
}

QWidget* BtrfsEventWidget::createEventItem(const BtrfsEvent &event)
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(10, 5, 10, 5);

    // ============================================================
    // ICONA DI SEVERITÀ
    // ============================================================
    QLabel *severityIcon = new QLabel(getSeverityIcon(event.severity), widget);
    severityIcon->setFixedSize(24, 24);
    severityIcon->setAlignment(Qt::AlignCenter);

    // ============================================================
    // TIPO EVENTO
    // ============================================================
    QLabel *typeLabel = new QLabel(eventIcon(event) + QStringLiteral(" ") + eventTypeText(event), widget);

    typeLabel->setFixedWidth(170);
    typeLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    typeLabel->setFixedWidth(150);
    typeLabel->setStyleSheet("font-weight: bold;");

    // ============================================================
    // MESSAGGIO
    // ============================================================
    QLabel *messageLabel = new QLabel(event.message, widget);
    messageLabel->setWordWrap(true);

    // ============================================================
    // TIMESTAMP
    // ============================================================
    QString timeStr = event.lastSeen.toString("dd/MM/yyyy HH:mm:ss");
    QLabel *timeLabel = new QLabel(timeStr, widget);
    timeLabel->setFixedWidth(160);
    timeLabel->setStyleSheet("color: gray; font-size: 11px;");

    // ============================================================
    // CONTEggio OCCORRENZE
    // ============================================================
    QLabel *countLabel = new QLabel(QString("×%1").arg(event.occurrenceCount), widget);
    countLabel->setFixedWidth(40);
    countLabel->setAlignment(Qt::AlignCenter);
    countLabel->setStyleSheet("color: gray; font-weight: bold;");

    // ============================================================
    // STATO
    // ============================================================
    QLabel *statusLabel = new QLabel(widget);
    if (!event.active)
    {
        statusLabel->setText(QStringLiteral("✓ Resolved"));
        statusLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    }
    else if (event.acknowledged)
    {
        statusLabel->setText(QStringLiteral("✓ Acknowledged"));
        statusLabel->setStyleSheet(QStringLiteral("color: green; font-size: 11px;"));
    }
    else
    {
        statusLabel->setText(QStringLiteral("⚠ Active"));
        statusLabel->setStyleSheet(QStringLiteral("color: orange; font-weight: bold; font-size: 11px;"));
    }
    statusLabel->setFixedWidth(100);

    // ============================================================
    // LAYOUT
    // ============================================================
    layout->addWidget(severityIcon);
    layout->addWidget(typeLabel);
    layout->addWidget(messageLabel, 1);
    layout->addWidget(timeLabel);
    layout->addWidget(countLabel);
    layout->addWidget(statusLabel);

    // Colore di sfondo in base alla severità
    //QString bgColor = getSeverityColor(event.severity);
    //widget->setStyleSheet(QString("background-color: %1; border-radius: 4px;").arg(bgColor));

    return widget;
}

QString BtrfsEventWidget::getSeverityColor(BtrfsEventSeverity severity)
{
    switch (severity) {
    case BtrfsEventSeverity::Critical: return "#FFCDD2"; // Rosso chiaro
    case BtrfsEventSeverity::Warning:  return "#FFF9C4"; // Giallo chiaro
    case BtrfsEventSeverity::Info:     return "#C8E6C9"; // Verde chiaro
    default: return "#FFFFFF";
    }
}

QString BtrfsEventWidget::getSeverityIcon(BtrfsEventSeverity severity)
{
    switch (severity) {
    case BtrfsEventSeverity::Critical: return "🔴";
    case BtrfsEventSeverity::Warning:  return "🟡";
    case BtrfsEventSeverity::Info:     return "🟢";
    default: return "⚪";
    }
}

QString BtrfsEventWidget::getEventIcon(BtrfsEventType type) const
{
    switch (type) {
    case BtrfsEventType::DeviceAdded:      return "➕";
    case BtrfsEventType::DeviceRemoved:    return "➖";
    case BtrfsEventType::DeviceMissing:    return "❌";
    case BtrfsEventType::ReadError:        return "📖";
    case BtrfsEventType::WriteError:       return "✏️";
    case BtrfsEventType::FlushError:       return "💾";
    case BtrfsEventType::CorruptionError:  return "💀";
    case BtrfsEventType::GenerationError:  return "⚠️";
    case BtrfsEventType::FilesystemRecovered: return "✅";
    case BtrfsEventType::ScrubStarted:     return "🧹";
    case BtrfsEventType::ScrubCompleted:   return "✨";
    case BtrfsEventType::ScrubError:       return "🧹❌";
    case BtrfsEventType::BalanceStarted:   return "⚖️";
    case BtrfsEventType::BalanceCompleted: return "✅";
    default: return "📋";
    }
}

void BtrfsEventWidget::updateBadge()
{
    const QList<BtrfsEvent> events = m_eventManager->activeEvents();
    int activeCount = events.count();
    int criticalCount = 0;

    for (const BtrfsEvent &event : std::as_const(events)) {
        if (event.severity == BtrfsEventSeverity::Critical) {
            criticalCount++;
        }
    }

    if (activeCount == 0) {
        m_badgeLabel->setText("✅ All Clear");
        m_badgeLabel->setStyleSheet("color: green; font-weight: bold;");
    } else {
        QString text = QString("⚠ %1 active events").arg(activeCount);
        if (criticalCount > 0) {
            text += QString(" (🔴 %1 critical)").arg(criticalCount);
        }
        m_badgeLabel->setText(text);
        m_badgeLabel->setStyleSheet("color: orange; font-weight: bold;");
    }
}


// ============================================================
// SLOTS
// ============================================================

void BtrfsEventWidget::refreshEventList()
{
    loadEvents();
}

void BtrfsEventWidget::onEventsChanged()
{
    loadEvents();
}

void BtrfsEventWidget::onAcknowledgeClicked()
{
    if (!m_agentDbusClient || m_acknowledgePending || m_selectedEvent.id == 0 || !m_selectedEvent.active || m_selectedEvent.acknowledged)
    {
        return;
    }

    const QString source = m_selectedEvent.source == StorageEventSource::Smart ? QStringLiteral("smart") : QStringLiteral("btrfs");

    m_acknowledgePending = true;

    m_acknowledgeBtn->setEnabled(false);

    m_agentDbusClient->acknowledgeEvent(source, static_cast<qulonglong>(m_selectedEvent.id));
}

void BtrfsEventWidget::onAcknowledgeFinished(const QString &source, qulonglong eventId, bool accepted, const QString &errorMessage)
{
    Q_UNUSED(source);
    Q_UNUSED(eventId);

    m_acknowledgePending = false;

    if (!accepted)
    {
        m_acknowledgeBtn->setEnabled(m_selectedEvent.id != 0 && m_selectedEvent.active && !m_selectedEvent.acknowledged);

        QMessageBox::warning(this, QStringLiteral("Acknowledge Event"), errorMessage.isEmpty() ? QStringLiteral("The event could not be acknowledged.") : errorMessage);

        return;
    }

    /*
     * Il daemon ha già modificato SQLite.
     *
     * Rileggiamo immediatamente il database:
     * nessun popup di successo necessario.
     */
    loadEvents();
}

void BtrfsEventWidget::onResolveClicked()
{
    if (!m_agentDbusClient || m_resolvePending || m_selectedEvent.id == 0 || !m_selectedEvent.active)
    {
        return;
    }

    const QMessageBox::StandardButton reply = QMessageBox::question(this, QStringLiteral("Resolve Event"), QStringLiteral("Are you sure you want to mark this event as resolved?"), QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes)
    {
        return;
    }

    const QString source = m_selectedEvent.source == StorageEventSource::Smart ? QStringLiteral("smart") : QStringLiteral("btrfs");

    m_resolvePending = true;
    m_resolveBtn->setEnabled(false);

    m_agentDbusClient->resolveEvent(source, static_cast<qulonglong>(m_selectedEvent.id));
}

void BtrfsEventWidget::onResolveFinished(const QString &source, qulonglong eventId, bool accepted, const QString &errorMessage)
{
    Q_UNUSED(source);
    Q_UNUSED(eventId);

    m_resolvePending = false;

    if (!accepted)
    {
        m_resolveBtn->setEnabled(m_selectedEvent.id != 0 && m_selectedEvent.active);

        QMessageBox::warning(this, QStringLiteral("Resolve Event"), errorMessage.isEmpty() ? QStringLiteral("The event could not be resolved.") : errorMessage);
        return;
    }

    /*
     * Il daemon ha già modificato SQLite.
     *
     * Rileggiamo immediatamente il database.
     * Nessun popup di successo necessario.
     */
    loadEvents();
}

void BtrfsEventWidget::onClearHistoryClicked()
{
    if (!m_agentDbusClient || m_clearHistoryPending)
    {
        return;
    }

    const QMessageBox::StandardButton reply = QMessageBox::question(this, QStringLiteral("Clear History"), QStringLiteral(
                "This will permanently delete all "
                "resolved event history.\n"
                "Active events will be preserved.\n"
                "⚠️ This action cannot be undone!"), QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes)
    {
        return;
    }

    m_clearHistoryPending = true;
    m_clearBtn->setEnabled(false);

    m_agentDbusClient->clearEventHistory();
}

void BtrfsEventWidget::onClearEventHistoryFinished(bool accepted, const QString &errorMessage)
{
    m_clearHistoryPending = false;
    m_clearBtn->setEnabled(true);

    if (!accepted)
    {
        QMessageBox::warning(this, QStringLiteral("Clear History"), errorMessage.isEmpty() ? QStringLiteral("The event history could not be cleared.")
                : errorMessage);
        return;
    }

    /*
     * Il daemon ha già modificato SQLite.
     * Rileggiamo immediatamente il database.
     */
    loadEvents();
}

void BtrfsEventWidget::onItemDoubleClicked(QListWidgetItem *item)
{
    QVariant eventData = item->data(Qt::UserRole);
    if (eventData.canConvert<BtrfsEvent>()) {
        BtrfsEvent event = eventData.value<BtrfsEvent>();
        showEventDetails(event);
    }
}

void BtrfsEventWidget::showEventDetails(const BtrfsEvent &event)
{
    QMessageBox details(this);
    details.setWindowTitle(QStringLiteral("Event Details"));
    details.setIcon(QMessageBox::Information);

    QString severityText;
    switch (event.severity)
    {
    case BtrfsEventSeverity::Critical: severityText = QStringLiteral("🔴 CRITICAL"); break;
    case BtrfsEventSeverity::Warning:  severityText = QStringLiteral("🟡 WARNING"); break;
    case BtrfsEventSeverity::Info:     severityText = QStringLiteral("🟢 INFO"); break;
    default:                           severityText = QStringLiteral("⚪ UNKNOWN"); break;
    }

    QString statusText;
    if (!event.active)                 statusText = QStringLiteral("✓ Resolved");
    else if (event.acknowledged)       statusText = QStringLiteral("✓ Acknowledged");
    else                               statusText = QStringLiteral("⚠ Active");

    const QString typeText = eventTypeText(event);
    const QString icon = eventIcon(event);

    QString detailsText;
    detailsText += QStringLiteral("═══════════════════════════════════════════════════════════\n📋 EVENT DETAILS\n═══════════════════════════════════════════════════════════\n\n");
    detailsText += QStringLiteral("🔹 Source:    %1\n").arg(event.source == StorageEventSource::Smart ? QStringLiteral("SMART") : QStringLiteral("Btrfs"));
    detailsText += QStringLiteral("🔹 ID:        %1\n").arg(event.id);
    detailsText += QStringLiteral("🔹 Type:      %1 %2\n").arg(icon, typeText);
    detailsText += QStringLiteral("🔹 Severity:  %1\n").arg(severityText);
    detailsText += QStringLiteral("🔹 Status:    %1\n\n").arg(statusText);
    detailsText += QStringLiteral("📌 Title:     %1\n").arg(event.title);
    detailsText += QStringLiteral("📝 Message:   %1\n\n").arg(event.message);

    if (event.source == StorageEventSource::Smart)
    {
        detailsText += QStringLiteral("───────────────────────────────────────────────────────────\n💽 SMART Device\n───────────────────────────────────────────────────────────\n");
        detailsText += QStringLiteral("  • Path:      %1\n").arg(event.devicePath);
        detailsText += QStringLiteral("  • Model:     %1\n").arg(event.modelName.isEmpty() ? QStringLiteral("-") : event.modelName);
        detailsText += QStringLiteral("  • Serial:    %1\n\n").arg(event.serialNumber.isEmpty() ? QStringLiteral("-") : event.serialNumber);
    }
    else
    {
        detailsText += QStringLiteral("───────────────────────────────────────────────────────────\n📍 Filesystem\n───────────────────────────────────────────────────────────\n");
        detailsText += QStringLiteral("  • UUID:      %1\n").arg(event.fsUuid);
        detailsText += QStringLiteral("  • Mount:     %1\n\n").arg(event.mountPoint);
        detailsText += QStringLiteral("───────────────────────────────────────────────────────────\n💾 Device\n───────────────────────────────────────────────────────────\n");
        detailsText += QStringLiteral("  • ID:        %1\n").arg(event.deviceId);
        detailsText += QStringLiteral("  • Path:      %1\n\n").arg(event.devicePath);
    }

    detailsText += QStringLiteral("───────────────────────────────────────────────────────────\n📊 Statistics\n───────────────────────────────────────────────────────────\n");
    detailsText += QStringLiteral("  • First Seen: %1\n").arg(event.firstSeen.toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")));
    detailsText += QStringLiteral("  • Last Seen:  %1\n").arg(event.lastSeen.toString(QStringLiteral("dd/MM/yyyy HH:mm:ss")));
    detailsText += QStringLiteral("  • Count:      %1\n").arg(event.occurrenceCount);

    if (event.source == StorageEventSource::Btrfs)
    {
        detailsText += QStringLiteral("  • Delta:      %1\n").arg(event.delta);
    }

    detailsText += QStringLiteral("  • Previous:   %1\n").arg(event.previousValue);
    detailsText += QStringLiteral("  • Current:    %1\n").arg(event.currentValue);
    detailsText += QStringLiteral("═══════════════════════════════════════════════════════════");

    details.setText(detailsText);
    details.exec();
}
