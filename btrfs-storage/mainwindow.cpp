// SPDX-License-Identifier: GPL-3.0-or-later

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "diskrowwidget.h"
#include "diskinfo.h"
#include "agentdbusclient.h"

#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QSpacerItem>
#include <QMessageBox>
#include <QSet>
#include <QDebug>
#include <QPixmap>
#include <QtMath>
#include <utility>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDateTime>
#include <QTimeZone>

// ============================================================================
// FORMAT BYTES FOR GUI STATIC SOLO PER mainwindow.cpp
// ============================================================================

static QString formatBytes(quint64 bytes)
{
    if (bytes == 0)
        return "0 B";

    const double value = static_cast<double>(bytes);

    const double KiB = 1024.0;
    const double MiB = KiB * 1024.0;
    const double GiB = MiB * 1024.0;
    const double TiB = GiB * 1024.0;

    if (value >= TiB)
        return QString("%1 TiB").arg(value / TiB, 0, 'f', 2);

    if (value >= GiB)
        return QString("%1 GiB").arg(value / GiB, 0, 'f', 1);

    if (value >= MiB)
        return QString("%1 MiB").arg(value / MiB, 0, 'f', 1);

    if (value >= KiB)
        return QString("%1 KiB").arg(value / KiB, 0, 'f', 1);

    return QString("%1 B").arg(bytes);

}

bool MainWindow::isRemovalActive(const QString &mountPoint, qulonglong deviceId) const
{
    const auto it = m_activeRemovals.constFind(mountPoint.trimmed());

    return it != m_activeRemovals.cend()
           && it.value() == deviceId;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    ui->statusBar->showMessage(QStringLiteral("Ready"));

    // ============================================================
    // ICONS
    // ============================================================
    // 1. Definisci il font e la dimensione una sola volta
    QFont iconFont = ui->lblIcoBalance->font();
    iconFont.setPointSize(24);

    // 2. Crea una lista dei widget da aggiornare per evitare ripetizioni (DRY)
    const auto labels = { ui->lblIcoBalance, ui->lblIcoAutomation, ui->lblIcoScrub, ui->lblIcoRemove, ui->lblIcoReplace };

    for (QLabel* label : labels) {
        label->setFont(iconFont);
    }

    // 3. Assegna i testi (QString gestisce già internamente l'UTF-8 nei compilatori moderni)
    ui->lblIcoBalance->setText("⚖️");
    ui->lblIcoAutomation->setText("🗓️");
    ui->lblIcoScrub->setText("🔎");
    ui->lblIcoRemove->setText("💽❌");
    ui->lblIcoReplace->setText("💽🔄");
    // ============================================================

    // ============================================================
    // REMOVE DISK
    // ============================================================

    ui->grp_Removal->setEnabled(false);

    ui->progress_Removal->setRange(0, 100);
    ui->progress_Removal->setValue(0);

    ui->lbl_Info_Removal->clear();
    ui->lbl_Remainig_Removal->clear();

    m_removalHideTimer = new QTimer(this);

    m_removalHideTimer->setSingleShot(true);
    m_removalHideTimer->setInterval(5000);

    connect(m_removalHideTimer, &QTimer::timeout, this, [this]()
        {
            if (!m_activeRemovals.isEmpty())
            return;

            ui->grp_Removal->setEnabled(false);

            ui->progress_Removal->setValue(0);

            ui->lbl_Info_Removal->clear();
            ui->lbl_Remainig_Removal->clear();
        });

    // ============================================================
    // OPEN DATABASE
    // ============================================================
    openMonitorDatabase();

    // ============================================================
    // STORAGE MONITOR AGENT - D-BUS CLIENT
    // ============================================================

    m_agentDbusClient = new AgentDbusClient(this);

    connect(m_agentDbusClient, &AgentDbusClient::agentServiceRegistered, this, [this]() {
        // Breve intervallo per permettere al daemon di registrare l'oggetto D-Bus
        QTimer::singleShot(500, this, [this]() {
            if (!m_agentDbusClient) return;

            const QString mountPoint = ui->comboBoxMount->currentText().trimmed();
            if (mountPoint.isEmpty()) return;

            qInfo().noquote() << "Storage Monitor Agent returned; requesting scrub status for" << mountPoint;
            m_agentDbusClient->requestScrubStatus(mountPoint);
            m_agentDbusClient->requestAutoScrubConfig(mountPoint);
            m_agentDbusClient->requestAutoScrubSchedule(mountPoint);
            m_agentDbusClient->requestReplaceStatus(mountPoint);
        });
    });

    connect(m_agentDbusClient, &AgentDbusClient::balanceStateChanged, this, &MainWindow::onAgentBalanceStateChanged);

    connect(m_agentDbusClient, &AgentDbusClient::balanceProgressChanged, this, &MainWindow::onAgentBalanceProgressChanged);

    connect(m_agentDbusClient, &AgentDbusClient::balanceStatusReceived, this, &MainWindow::onBalanceStatusReceived);

    connect(m_agentDbusClient, &AgentDbusClient::startBalanceFinished, this, &MainWindow::onStartBalanceFinished);

    connect(m_agentDbusClient, &AgentDbusClient::cancelBalanceFinished, this, &MainWindow::onCancelBalanceFinished);

    connect(m_agentDbusClient, &AgentDbusClient::addDeviceFinished, this, &MainWindow::onAddDeviceFinished);

    connect(m_agentDbusClient, &AgentDbusClient::removeDeviceFinished, this, &MainWindow::onRemoveDeviceFinished);

    connect(m_agentDbusClient, &AgentDbusClient::removeDeviceProgress, this, &MainWindow::onRemoveDeviceProgress);

    connect(m_agentDbusClient, &AgentDbusClient::startReplaceFinished, this, &MainWindow::onStartReplaceFinished);

    connect(m_agentDbusClient, &AgentDbusClient::cancelReplaceFinished, this, &MainWindow::onCancelReplaceFinished);

    connect(m_agentDbusClient, &AgentDbusClient::replaceStatusReceived, this, &MainWindow::onReplaceStatusReceived);

    connect(m_agentDbusClient, &AgentDbusClient::replaceStateChanged, this, &MainWindow::onAgentReplaceStateChanged);

    connect(m_agentDbusClient, &AgentDbusClient::replaceProgressChanged, this, &MainWindow::onAgentReplaceProgressChanged);

    connect(m_agentDbusClient, &AgentDbusClient::errorOccurred, this, &MainWindow::onAgentDbusError);

    connect(m_agentDbusClient, &AgentDbusClient::scrubStateChanged, this, &MainWindow::onAgentScrubStateChanged);

    connect(m_agentDbusClient, &AgentDbusClient::scrubProgressChanged, this, &MainWindow::onAgentScrubProgressChanged);

    connect(m_agentDbusClient, &AgentDbusClient::scrubStatusReceived, this, &MainWindow::onScrubStatusReceived);

    connect(m_agentDbusClient, &AgentDbusClient::autoScrubConfigReceived, this, &MainWindow::onAutoScrubConfigReceived);

    connect(m_agentDbusClient, &AgentDbusClient::autoScrubScheduleReceived, this, &MainWindow::onAutoScrubScheduleReceived);

    connect(m_agentDbusClient, &AgentDbusClient::setAutoScrubConfigFinished, this, &MainWindow::onSetAutoScrubConfigFinished);

    connect(m_agentDbusClient, &AgentDbusClient::startScrubFinished, this, &MainWindow::onStartScrubFinished);

    connect(m_agentDbusClient, &AgentDbusClient::cancelScrubFinished, this, &MainWindow::onCancelScrubFinished);


    // ============================================================
    // INIZIALIZZAZIONE BTRFS MONITOR E EVENT MANAGER
    // ============================================================

    m_eventManager = new BtrfsEventManager(this);
    m_btrfsMonitor = new BtrfsMonitor(this);

    // Collega il monitor all'event manager
    // Il BtrfsMonitor pubblica snapshot che l'event manager processa
    // Questa connessione è già interna al BtrfsMonitor

    // Collega segnali di salute per notifiche immediate
    connect(m_btrfsMonitor, &BtrfsMonitor::healthChanged, this, &MainWindow::onBtrfsHealthChanged);

    //+++++++++++++++++++++++++++++++++++++++++++
    //+++++++++++++++++++++++++++++++++++++++++++
    connect(m_btrfsMonitor, &BtrfsMonitor::smartDevicesChanged, this, [this]()
        {
            const QString mountPoint = ui->comboBoxMount->currentText();

            if (mountPoint.isEmpty())
                return;

            qDebug().noquote() << "SMART state changed; refreshing" << mountPoint;

            onMountPointChanged(mountPoint);
        });
    //+++++++++++++++++++++++++++++++++++++++++++
    //+++++++++++++++++++++++++++++++++++++++++++

    connect(m_btrfsMonitor, &BtrfsMonitor::devicesChanged, this, [this]()
        {
            const QString mountPoint = ui->comboBoxMount->currentText();

            if (mountPoint.isEmpty())
                return;

            qDebug().noquote() << "Btrfs devices changed; refreshing" << mountPoint;

            onMountPointChanged(mountPoint);
        });

    // ============================================================
    // CREA TAB EVENTI
    // ============================================================

    // Crea il widget eventi
    m_eventWidget = new BtrfsEventWidget(m_eventManager, m_agentDbusClient, this);

    // Aggiungi il widget come nuova tab
    ui->tab_btrfs->addTab(m_eventWidget, "Events");

    // Se vuoi che sia l'ultima tab, mettila dopo management
    //ui->tab_btrfs->setCurrentIndex(ui->tab_btrfs->indexOf(ui->tab_management));
    ui->tab_btrfs->setCurrentIndex(0);

    // ============================================================
    // INIZIALIZZAZIONE BILANCIAMENTO - SEMPRE VISIBILE
    // ============================================================

    // La groupbox è sempre visibile perché è in una tab
    ui->grp_Bilanciamento->setVisible(true);

    //Pulsante Stop inizialmente disabilitato
    ui->btnStopBalance->setEnabled(false);
    ui->btnStopBalance->setVisible(true);

    // Inizializza la progress bar
    ui->progress_Bilaciamento->setRange(0, 100);
    ui->progress_Bilaciamento->setValue(0);
    ui->progress_Bilaciamento->setVisible(false);
    ui->lbl_Manu_Balance->setText("Ready");
    ui->grp_Bilanciamento->setTitle("Balance");
    ui->btnBalance->setEnabled(true);
    currentBalanceState = "ready";

    // ============================================================
    // INIZIALIZZAZIONE SCRUB
    // ============================================================

    ui->grp_Scrub->setVisible(true);

    ui->btnStartScrub->setEnabled(true);
    ui->btnStopScrub->setEnabled(false);

    ui->progress_Scrub->setRange(0, 100);
    ui->progress_Scrub->setValue(0);
    ui->progress_Scrub->setVisible(false);

    ui->lbl_Status_Scrub->setText(QStringLiteral("Ready"));

    currentScrubState = QStringLiteral("ready");

    ui->cmbIntervalScrub->clear();

    for (int weeks = 1; weeks <= 6; ++weeks)
        ui->cmbIntervalScrub->addItem(weeks == 1 ? QStringLiteral("1 week") : QStringLiteral("%1 weeks").arg(weeks), weeks);

    ui->chkAutoScrub->setChecked(false);

    int defaultIndex = ui->cmbIntervalScrub->findData(4);
    if (defaultIndex >= 0) ui->cmbIntervalScrub->setCurrentIndex(defaultIndex);

    ui->chkAutoScrub->setEnabled(false);
    ui->cmbIntervalScrub->setEnabled(false);
    ui->lblLastScrub->setText(QStringLiteral("Last scrub: -"));
    ui->lblNextScrub->setText(QStringLiteral("Next scrub: -"));

   /*
     * Usiamo clicked() e activated():
    * vengono emessi dalle azioni dell'utente,
    * non quando aggiorniamo programmaticamente
    * i widget dopo una risposta D-Bus.
    */
    connect(ui->chkAutoScrub, &QCheckBox::clicked, this, [this](bool)
        {
            saveAutoScrubConfig();
        });

    connect(ui->cmbIntervalScrub, qOverload<int>(&QComboBox::activated), this, [this](int)
        {
            saveAutoScrubConfig();
        });

    // ============================================================
    // 1. Layout delle etichette  e ScrollArea dei device
    // ============================================================

    // Recupera il colore nativo del bordo impostato sul sistema (KDE Breeze)
    QColor systemBorderColor = ui->label->palette().color(QPalette::Dark);

    // Converte il colore in una stringa esadecimale compatibile con il CSS (es. #31363b)
    QString borderStyle = QString("border: 1px solid %1;").arg(systemBorderColor.name());

    // Applica lo stile in un colpo solo a tutte le label coinvolte
    ui->label->setStyleSheet(borderStyle);
    ui->label_3->setStyleSheet(borderStyle);
    ui->label_4->setStyleSheet(borderStyle);
    ui->label_5->setStyleSheet(borderStyle);

    //Controlla se la zona interna della scroll
    //area (scrollAreaWidgetContents) non ha ancora un layout assegnato.
    if (!ui->scrollAreaDischi->layout()) {
        /*Se non esiste, crea un nuovo layout verticale (QVBoxLayout). Gli elementi
  * inseriti qui dentro verranno disposti dall'alto verso il basso.
  * Porgere ui->scrollAreaWidgetContents come argomento al costruttore assegna
  * automaticamente il widget come "padre" del layout. */
        QVBoxLayout *newLayout = new QVBoxLayout(ui->scrollAreaDischi);
        // Assegna ufficialmente il nuovo layout verticale al widget.
        ui->scrollAreaDischi->setLayout(newLayout);
    }

    scrollLayout = qobject_cast<QVBoxLayout*>(ui->scrollAreaDischi->layout());

    if (scrollLayout) {
        scrollLayout->setSpacing(2);
        scrollLayout->setContentsMargins(10, 10, 10, 10);
    }

    // ============================================================
    // Cambio mount point
    // ============================================================

    connect(ui->comboBoxMount, &QComboBox::currentTextChanged, this, &MainWindow::onMountPointChanged);

    // ============================================================
    // Mount Btrfs
    // ============================================================

    populateMountPoints();

}

MainWindow::~MainWindow()
{
    delete ui;
}

// ============================================================================
// PARSE BTRFS SIZE
//
// Accetta:
//
//     123
//     1.82TiB
//     700GiB
//     500GB
//
// Restituisce byte.
// ============================================================================

quint64 MainWindow::parseBtrfsSize(const QString &value) const
{
    const QString text = value.trimmed();

    static QRegularExpression regex(R"(^([0-9]+(?:\.[0-9]+)?)\s*([KMGTPE]i?B)?$)", QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = regex.match(text);

    if (!match.hasMatch())
        return 0;

    const double number = match.captured(1).toDouble();

    const QString unit = match.captured(2).toUpper();

    double multiplier = 1.0;

    if (unit == "KB")
        multiplier = 1000.0;
    else if (unit == "MB")
        multiplier = 1000.0 * 1000.0;
    else if (unit == "GB")
        multiplier = 1000.0 * 1000.0 * 1000.0;
    else if (unit == "TB")
        multiplier = 1000.0 * 1000.0 * 1000.0 * 1000.0;
    else if (unit == "PB")
        multiplier = 1000.0 * 1000.0 * 1000.0 * 1000.0 * 1000.0;
    else if (unit == "KIB")
        multiplier = 1024.0;
    else if (unit == "MIB")
        multiplier = 1024.0 * 1024.0;
    else if (unit == "GIB")
        multiplier = 1024.0 * 1024.0 * 1024.0;
    else if (unit == "TIB")
        multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else if (unit == "PIB")
        multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0;

    return static_cast<quint64>(number * multiplier);

}

// ============================================================================
// POPULATE MOUNT POINTS IN COMBOBOX da btrfs.db
// ============================================================================

void MainWindow::populateMountPoints()
{
    ui->comboBoxMount->clear();

    if (!m_monitorDb.isValid() || !m_monitorDb.isOpen())
    {
        qWarning() << "Monitor database is not open";
        return;
    }

    QSqlQuery query(m_monitorDb);

    if (!query.exec(R"(
        SELECT mount_point
        FROM filesystems
        WHERE mount_point IS NOT NULL
          AND TRIM(mount_point) <> ''
        ORDER BY mount_point COLLATE NOCASE
    )"))
    {
        qWarning().noquote() << "Cannot read mount points:" << query.lastError().text();
        return;
    }

    while (query.next())
    {
        const QString mountPoint = query.value(0).toString().trimmed();

        if (!mountPoint.isEmpty())
            ui->comboBoxMount->addItem(mountPoint);
    }

    if (ui->comboBoxMount->count() == 0)
    {
        qWarning() << "No Btrfs filesystems found in database";
    }
}

// ============================================================================
// BUILD COMPLETE BTRFS MODEL
// ============================================================================

MainWindow::BtrfsInfo
MainWindow::updateBtrfsInformation(const QString &mountPoint)
{
    BtrfsInfo info;

    info.mountPoint = mountPoint;

    if (!m_monitorDb.isValid() || !m_monitorDb.isOpen())
    {
        qWarning() << "Monitor database is not open";

        return info;
    }

    QSqlQuery query(m_monitorDb);

    query.prepare(R"(
        SELECT
            fs_uuid,
            mount_point,
            device_count,
            data_profile,
            metadata_profile,
            total_bytes,
            used_bytes,
            unallocated_bytes,
            scrub_status,
            balance_status,
            health_state,
            last_seen
        FROM filesystems
        WHERE mount_point = :mount_point
        ORDER BY last_seen DESC
        LIMIT 1
    )");

    query.bindValue(QStringLiteral(":mount_point"), mountPoint);

    if (!query.exec())
    {
        qWarning().noquote() << "Cannot read filesystem state:" << query.lastError().text();
        return info;
    }

    if (!query.next())
    {
        qWarning().noquote() << "Filesystem not found in database:" << mountPoint;
        return info;
    }


    // ============================================================
    // FILESYSTEM
    // ============================================================

    info.mountPoint = query.value(QStringLiteral("mount_point")).toString();

    info.deviceCount = query.value(QStringLiteral("device_count")).toInt();

    // ============================================================
    // PROFILES
    // ============================================================

    info.dataProfile = query.value(QStringLiteral("data_profile")).toString();

    info.metadataProfile = query.value(QStringLiteral("metadata_profile")).toString();

    const QString dataProfile = info.dataProfile.trimmed().toLower();
    const QString metadataProfile = info.metadataProfile.trimmed().toLower();

    const bool dataKnown = !dataProfile.isEmpty() && dataProfile != QStringLiteral("unknown");
    const bool metadataKnown = !metadataProfile.isEmpty() && metadataProfile != QStringLiteral("unknown");

    if (dataKnown && metadataKnown) info.profile = (dataProfile == metadataProfile) ? dataProfile : QStringLiteral("mixed");
    else if (dataKnown || metadataKnown) info.profile = QStringLiteral("incomplete");
    else info.profile = QStringLiteral("unknown");


    // ============================================================
    // STORAGE
    // ============================================================

    info.totalBytes = query.value(QStringLiteral("total_bytes")).toULongLong();

    info.usedBytes = query.value(QStringLiteral("used_bytes")).toULongLong();

    info.freeBytes = query.value(QStringLiteral("unallocated_bytes")).toULongLong();


    // ============================================================
    // MAINTENANCE
    // ============================================================

    info.scrubStatus = query.value(QStringLiteral("scrub_status")).toString();

    info.balanceStatus = query.value(QStringLiteral("balance_status")).toString();


    // ============================================================
    // HEALTH
    // ============================================================

    const int healthState = query.value(QStringLiteral("health_state")).toInt();

    switch (healthState)
    {
    case 0: // daemon: Unknown
        info.health = HealthState::Unknown;
        break;

    case 1: // daemon: Ok
        info.health = HealthState::Healthy;
        break;

    case 2: // daemon: Warning
        info.health = HealthState::Warning;
        break;

    case 3: // daemon: Error
        info.health =  HealthState::Failed;
        break;

    default:
        info.health = HealthState::Unknown;
        break;
    }


    return info;
}

void MainWindow::populateDeviceSummary(BtrfsInfo &info, const QString &mountPoint) const
{
    info.healthyDevices = 0;
    info.warningDevices = 0;
    info.failedDevices = 0;
    info.errorCount = 0;

    if (!m_btrfsMonitor)
        return;

    QString fsUuid;

    const QList<BtrfsMonitor::FilesystemState>
        filesystems = m_btrfsMonitor->filesystems();

    for (const BtrfsMonitor::FilesystemState &filesystem :
         filesystems)
    {
        if (filesystem.mountPoint == mountPoint)
        {
            fsUuid = filesystem.fsUuid;
            break;
        }
    }

    if (fsUuid.isEmpty())
        return;

    const QList<BtrfsMonitor::DeviceState>
        devices = m_btrfsMonitor->devicesForFilesystem(fsUuid);

    for (const BtrfsMonitor::DeviceState &device : devices)
    {
        const bool hasErrors = device.readErrors > 0 || device.writeErrors > 0 || device.flushErrors > 0 ||  device.corruptionErrors > 0 || device.generationErrors > 0;

        if (!device.present)
        {
            ++info.warningDevices;
        }
        else if (hasErrors)
        {
            ++info.failedDevices;
        }
        else
        {
            ++info.healthyDevices;
        }

        info.errorCount +=
            device.readErrors +
            device.writeErrors +
            device.flushErrors +
            device.corruptionErrors +
            device.generationErrors;
    }
}

// ============================================================================
// UPDATE OVERVIEW PAGE
// ============================================================================

void MainWindow::updateBtrfsOverview(const BtrfsInfo &info)
{
    // ============================================================
    // STORAGE
    // ============================================================
    ui->lbl_Btrfsused->setText(QString("%1 Used").arg(formatBytes(info.usedBytes)));
    ui->lbl_Btrfsfree->setText(QString("%1 Unallocated").arg(formatBytes(info.freeBytes)));
    ui->label_btrfstotal->setText(QString("Total: %1").arg(formatBytes(info.totalBytes)));

    int percentage = 0;

    if (info.totalBytes > 0) {

        percentage = static_cast<int>((static_cast<double>(info.usedBytes) / static_cast<double>(info.totalBytes)) * 100.0);

        percentage = qBound(0, percentage, 100);
    }

    ui->progressBar_btrfsdata->setRange(0, 100);
    ui->progressBar_btrfsdata->setValue(percentage);
    ui->progressBar_btrfsdata->setFormat(QString("%1%").arg(percentage));

    // ============================================================
    // FILESYSTEM
    // ============================================================

    ui->lbl_mountpoint->setText(QString("Mount point     %1").arg(info.mountPoint));
    ui->lbl_Device->setText(QString("Devices         %1").arg(info.deviceCount));
    ui->lbl_Profile->setText(QString("Profile         %1").arg(info.profile.isEmpty()? "-" : info.profile));
    ui->lbl_Data->setText(QString("Data            %1").arg(info.dataProfile.isEmpty()? "-" : info.dataProfile));
    ui->lbl_Metadata->setText(QString("Metadata        %1").arg(info.metadataProfile.isEmpty()? "-" : info.metadataProfile));

    //========================================================
    // DEVICES
    /*btrfs filesystem show /mountpoint
 * e
 * btrfs device stats /mountpoint
  ┌─ device presente?
  │
  ├── NO → Warning
  │
  └── SI
       │
       ├─ error counters > 0 → Failed
       │
       └─ error counters == 0 → Healthy
    */
    //========================================================

    ui->lbl_Healthy->setText(QString("Healthy    %1").arg(info.healthyDevices));
    ui->lbl_Warning->setText(QString("Warning    %1").arg(info.warningDevices));
    ui->lbl_Failed->setText(QString("Failed    %1").arg(info.failedDevices));

    //========================================================
    // MAINTENANCE
    //========================================================

    // Sincronizza lo stato dal DB/D-Bus e pulisce la stringa
    QString scrubState = info.scrubStatus.trimmed().toLower();

    // Se il mount point coincide e abbiamo uno stato D-Bus valido diverso da "ready", ha la precedenza
    if (info.mountPoint.trimmed() == ui->comboBoxMount->currentText().trimmed() &&
        !currentScrubState.isEmpty() && currentScrubState != QStringLiteral("ready")) {
        scrubState = currentScrubState;
    }

    // Fallback se vuoto e aggiornamento della label
    if (scrubState.isEmpty()) scrubState = QStringLiteral("Unknown");
    ui->lbl_Scrub->setText(QStringLiteral("Scrub: %1").arg(scrubState));
    ui->lbl_Balance->setText("Balance: " + (info.balanceStatus.isEmpty()? "Unknown" : info.balanceStatus));
    ui->lbl_Errors->setText(QString("Btrfs error counters    %1").arg(info.errorCount));

}

// ============================================================================
// HEALTH STATUS
// ============================================================================

void MainWindow::updateFilesystemHealth(const BtrfsInfo &info)
{
    QString icon;
    QString status;
    QString description;
    QString color;

    switch (info.health) {

    case HealthState::Unknown:
        icon = "ℹ️";
        status = "SYSTEM UNKNOWN";
        description = "Filesystem health information is not available.";
        color = "#888";
        break;

    case HealthState::Healthy:

        icon = "✅";
        status = "SYSTEM HEALTHY";
        description = "All devices are operating normally.";
        color = "#4CAF50";
        break;

    case HealthState::Warning:

        icon = "⚠️";
        status = "SYSTEM WARNING";
        description = "The filesystem is operational but requires attention.";
        color = "#FF9800";
        break;

    case HealthState::Failed:

        icon = "❌";
        status = "SYSTEM FAILED";
        description = "One or more devices have reported Btrfs errors.";
        color = "#F44336";
        break;
    }

    // Icona di fallback se l'immagine non viene trovata
    ui->labelHealthIcon->setText(icon);
    ui->labelHealthIcon->setStyleSheet("font-size: 48px;");

    ui->labelHealthStatus->setText(status);
    ui->labelHealthDescription->setText(description);

    ui->labelHealthStatus->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));

}

// ============================================================================
// DEVICE PATH -> NAME
// ============================================================================

QString MainWindow::deviceNameFromPath(const QString &path) const
{
    if (path.startsWith("/dev/")) {

        return path.mid(QString("/dev/").length());
    }

    return path;

}
// ============================================================================
// FIND DEVICE IN LSBLK
// ============================================================================

QJsonObject MainWindow::findLsblkDevice(const QJsonArray &devices, const QString &wantedName) const
{
    for (const QJsonValue &value : devices) {

        if (!value.isObject())
            continue;

        const QJsonObject dev = value.toObject();

        const QString name = dev["name"].toString();

        if (name == wantedName)
            return dev;

        if (dev.contains("children") && dev["children"].isArray()) {

            const QJsonObject result = findLsblkDevice(dev["children"].toArray(), wantedName);

            if (!result.isEmpty())
                return result;
        }
    }

    return QJsonObject();

}

// ============================================================================
// CREATE DISK ROW
// ============================================================================

void MainWindow::addDiskRow(const DiskInfo &info, bool isBtrfs)
{
    //Controlla se il puntatore scrollLayout (il contenitore vertical
    //in cui vengono inseriti i widget esiste ed è stato inizializzato.
    if (!scrollLayout)
        return;

    /*Alloca dinamicamente in memoria un nuovo oggetto di tipo diskrowwidget.
Dettaglio: Passare this (la MainWindow) come argomento al costruttore
imposta la finestra principale come "genitore" (parent) del widget.
Questo garantisce che quando la finestra verrà chiusa e distrutta,
anche la riga del disco verrà eliminata automaticamente dalla memoria,
evitando memory leak.*/
    diskrowwidget *row = new diskrowwidget(this);

    /*Chiama una funzione interna del widget appena creato per impostare
 i testi visibili e icona*/
    row->setDiskData(info, isBtrfs);

    /*Collega un evento del widget appena creato a un'azione della finestra
 * principale.Collega un evento del widget appena creato a un'azione della finestra principale.*/
    connect(row, &diskrowwidget::diskClicked, this, &MainWindow::onDiskSelected);

    /*Aggiunge materialmente il widget row all'interfaccia grafica, posizionandolo dentro il layout.*/
    scrollLayout->addWidget(row);

    qDebug() << "Aggiunto row per:" << info.name << "isBtrfs:" << isBtrfs;

}

// ============================================================================
// MOUNT POINT CHANGED
// ============================================================================

void MainWindow::onMountPointChanged(const QString &mountPoint)
{

    // ============================================================
    // CAMBIO PAGINA
    // ===========================================================
    ui->stackedWidget->setCurrentWidget(ui->pageInfo);

    // ============================================================
    // RESET DELLA GROUPBOX DI BILANCIAMENTO (SEMPRE VISIBILE)
    // ===========================================================
    // Reset dello stato ma la groupbox rimane visibile
    ui->progress_Bilaciamento->setVisible(false);
    ui->progress_Bilaciamento->setValue(0);
    ui->lbl_Manu_Balance->setText("Ready");
    ui->grp_Bilanciamento->setTitle("Balance");
    ui->btnBalance->setEnabled(true);
    currentBalanceState = "ready";


    if (mountPoint.isEmpty())
        return;

    // ============================================================
    // CLEAN DEVICE LIST DISK in scrollAreaDischi
    // ============================================================
    if (ui->scrollAreaDischi && ui->scrollAreaDischi->layout()) {

        QLayout *diskLayout = ui->scrollAreaDischi->layout();
        QLayoutItem *item = nullptr;

        // Svuota in sicurezza solo il layout interno a questa specifica scrollArea
        while ((item = diskLayout->takeAt(0)) != nullptr) {

            if (item->widget()) {
                delete item->widget(); // Distrugge il DiskRowWidget e libera la memoria
            }

            delete item; // Elimina il contenitore del layout
        }
    }

    // ============================================================
    // LSBLK
    // ============================================================

    QProcess lsblkProcess;

    //formato JSON (una struttura dati standard e pulita)
    lsblkProcess.start("lsblk", QStringList() << "-o" << "NAME,FSTYPE,SIZE,MOUNTPOINT,UUID,TYPE" << "--json");

    //attesa 5" poi errore
    if (!lsblkProcess.waitForFinished(5000)) {
        qWarning() << "Timeout esecuzione lsblk";
        if (scrollLayout)
            scrollLayout->addStretch();
        return;
    }

    const QByteArray output = lsblkProcess.readAllStandardOutput();

    QJsonParseError jsonError;

    const QJsonDocument jsonDoc = QJsonDocument::fromJson(output, &jsonError);

    if (jsonDoc.isNull() || !jsonDoc.isObject()) {

        qWarning() << "Errore JSON lsblk:" << jsonError.errorString();

        if (scrollLayout)
            scrollLayout->addStretch();

        return;
    }

    /*lsblk --json viene eseguito correttamente, racchiude tutti i dischi dentro una categoria principale chiamata "blockdevices"
 *Questa riga estrae esattamente quella categoria e la trasforma in un QJsonArray. Questo array contiene, uno dopo l'altro,
 * tutti i dischi fisici trovati (come sda, sdb, sdc, ecc.) con tutte le loro proprietà e le relative sotto-partizioni*/
    const QJsonObject jsonObj = jsonDoc.object();
    const QJsonArray blockdevices = jsonObj["blockdevices"].toArray();

    // ============================================================
    // COSTRUZIONE MODELLO BTRFS
    // ============================================================

    BtrfsInfo info = updateBtrfsInformation(mountPoint);

    // ============================================================
    // STATO BALANCE DA DATABASE
    // ============================================================

    const QString databaseBalanceState = info.balanceStatus.trimmed().toLower();

    if (databaseBalanceState == QStringLiteral("running"))
    {
        isBalanceRunning = true;

        currentBalanceMount = mountPoint;

        setBalanceState(QStringLiteral("running"), info.balanceStatus);
    }
    else
    {
        isBalanceRunning = false;

        if (currentBalanceMount == mountPoint)
            currentBalanceMount.clear();

        setBalanceState(QStringLiteral("ready"), QStringLiteral("Ready"));
    }

    ui->lblLastScrub->setText(QStringLiteral("Last scrub: -"));
    ui->lblNextScrub->setText(QStringLiteral("Next scrub: -"));
    ui->chkAutoScrub->setEnabled(false);
    ui->cmbIntervalScrub->setEnabled(false);

    /*
    * Conferma lo stato live attraverso il daemon.
    * Nessun polling: una sola richiesta quando cambia mount.
    */
    if (m_agentDbusClient)
    {
        m_agentDbusClient->requestScrubStatus(mountPoint);
        m_agentDbusClient->requestAutoScrubConfig(mountPoint);
        m_agentDbusClient->requestAutoScrubSchedule(mountPoint);
        m_agentDbusClient->requestReplaceStatus(mountPoint);
    }

    // ============================================================
    // STATO SCRUB DA DATABASE
    // ============================================================

    const QString databaseScrubState = info.scrubStatus.trimmed().toLower();

    if (databaseScrubState == QStringLiteral("running")) { currentScrubMount = mountPoint; setScrubState(QStringLiteral("running"), QStringLiteral("Scrub is running")); }
    else if (databaseScrubState == QStringLiteral("finished")) { currentScrubMount.clear(); setScrubState(QStringLiteral("finished"), QStringLiteral("Last scrub completed")); }
    else if (databaseScrubState == QStringLiteral("cancelled")) { currentScrubMount.clear(); setScrubState(QStringLiteral("cancelled"), QStringLiteral("Last scrub was cancelled")); }
    else if (databaseScrubState == QStringLiteral("interrupted")) { currentScrubMount.clear(); setScrubState(QStringLiteral("interrupted"), QStringLiteral("Last scrub was interrupted")); }
    else { currentScrubMount.clear(); setScrubState(QStringLiteral("ready"), QStringLiteral("Ready")); }


    if (m_agentDbusClient) m_agentDbusClient->requestScrubStatus(mountPoint);

    // ============================================================
    // DEVICE BTRFS
    // ============================================================

    QSet<QString> btrfsDevices;

    QHash<QString, BtrfsDeviceStats>
        btrfsDeviceStats;

    QSet<QString> btrfsErrorDevices;

    QHash<QString, QString> btrfsDeviceIdsByPath;

    QList<BtrfsMonitor::DeviceState>
        unrepresentedMissingDevices;

    // ============================================================
    // DEVICE BTRFS DA DATABASE
    // ============================================================

    QString fsUuid;

    if (m_btrfsMonitor)
    {
        const QList<BtrfsMonitor::FilesystemState>
            filesystems = m_btrfsMonitor->filesystems();

        for (const BtrfsMonitor::FilesystemState &filesystem : filesystems)
        {
            if (filesystem.mountPoint == mountPoint)
            {
                fsUuid = filesystem.fsUuid;
                break;
            }
        }
    }


    if (!fsUuid.isEmpty() && m_btrfsMonitor)
    {
        const QList<BtrfsMonitor::DeviceState>
            devices = m_btrfsMonitor->devicesForFilesystem(fsUuid);

        for (const BtrfsMonitor::DeviceState &device : devices)
        {
            /*
             * Il path può essere conservato nel DB anche quando
             * il device diventa missing.
            */
            const QString devicePath = device.devicePath.trimmed();

            const QString deviceId = device.deviceId.trimmed();

            if (!devicePath.isEmpty() && !deviceId.isEmpty())
            {
                btrfsDeviceIdsByPath.insert(devicePath, deviceId);
            }

            const bool hasErrors = device.readErrors > 0 || device.writeErrors > 0 || device.flushErrors > 0 || device.corruptionErrors > 0 || device.generationErrors > 0;

            /*
         * Un membro Btrfs missing non deve essere considerato
         * tra i device presenti.
         *
         * Manteniamo per ora anche il token "MISSING" perché
         * il codice GUI sottostante lo gestisce già.
         */
            if (!device.present)
            {
                bool representedByLsblk = false;

                if (!devicePath.isEmpty())
                {
                    const QString deviceName = deviceNameFromPath(devicePath);

                    const QJsonObject dev = findLsblkDevice(blockdevices, deviceName);

                    representedByLsblk = isPhysicalDisk(dev);
                }

                if (!representedByLsblk)
                {
                    unrepresentedMissingDevices.append(device);
                }
            }
            else if (!devicePath.isEmpty())
            {
                btrfsDevices.insert(devicePath);
            }

            /*
         * Error counters indicizzati per device_path,
         * come nel vecchio modello MainWindow.
         */
            if (!devicePath.isEmpty())
            {
                BtrfsDeviceStats stats;

                stats.readErrors = device.readErrors;
                stats.writeErrors = device.writeErrors;
                stats.flushErrors = device.flushErrors;
                stats.corruptionErrors = device.corruptionErrors;
                stats.generationErrors = device.generationErrors;

                btrfsDeviceStats.insert(devicePath, stats);


                if (hasErrors)
                {
                    btrfsErrorDevices.insert(devicePath);
                }
            }
        }

    }
    else
    {
        qWarning().noquote() << "Cannot find Btrfs filesystem in monitor cache for mount point:" << mountPoint;
    }

    // ============================================================
    // DEVICE REPLACE - SOURCE DEVICES
    // ============================================================

    ui->cmbReplaceSource->clear();

    for (const QString &devicePath : std::as_const(btrfsDevices))
    {
        const QString deviceId = btrfsDeviceIdsByPath.value(devicePath).trimmed();
        bool idOk = false;
        const qulonglong deviceIdValue = deviceId.toULongLong(&idOk);

        if (!idOk || deviceIdValue == 0) continue;

        const QString text = QStringLiteral("devid %1 - %2").arg(deviceId, devicePath);

        /* Conserviamo nel QVariant il devid numerico. Il path rimane nel testo visualizzato. */
        ui->cmbReplaceSource->addItem(text, QVariant::fromValue(deviceIdValue));
    }

    // ============================================================
    // SMART DEVICES DA DATABASE
    // ============================================================

    QHash<QString, BtrfsMonitor::SmartDeviceState>
        smartDevicesByPath;

    auto populateSmartData = [&smartDevicesByPath](DiskInfo &diskInfo)
        {
            const auto it = smartDevicesByPath.constFind(diskInfo.devicePath);
            if (it == smartDevicesByPath.constEnd()) return;

            const BtrfsMonitor::SmartDeviceState &smart = it.value();
            diskInfo.hasSmartData = true;
            diskInfo.smartPresent = smart.present;
            diskInfo.smartReadOk = smart.readOk;
            diskInfo.smartSupported = smart.smartSupported;
            diskInfo.smartHealthState = static_cast<int>(smart.health);
        };



    if (m_btrfsMonitor)
    {
        const QList<BtrfsMonitor::SmartDeviceState>
            smartDevices = m_btrfsMonitor->smartDevices();

        for (const BtrfsMonitor::SmartDeviceState &device : smartDevices)
        {
            const QString devicePath = device.devicePath.trimmed();

            if (devicePath.isEmpty())
                continue;

            smartDevicesByPath.insert(devicePath, device);
        }
    }

    // ============================================================
    // AGGIORNA OVERVIEW E HEALTH
    // ORA I CONTATORI DEVICE SONO COMPLETI
    // ============================================================

    populateDeviceSummary(info, mountPoint);

    updateBtrfsOverview(info);
    updateFilesystemHealth(info);

    qDebug() << "========================================";
    qDebug() << "Btrfs mountpoint:" << mountPoint;
    qDebug() << "Btrfs devices:" << btrfsDevices;
    qDebug() << "Btrfs error devices:" << btrfsErrorDevices;
    qDebug() << "========================================";

    QSet<QString> displayedDevices;

    // ============================================================
    // COSTRUISCI GERARCHIA DISCHI/PARTIZIONI
    // ============================================================

    QMap<QString, DiskHierarchy> diskHierarchy = buildDiskHierarchy(blockdevices);

    // ============================================================
    // INTEGRA LA GERARCHIA LSBLK CON I MEMBRI BTRFS DAL DATABASE
    // ============================================================
    for (auto it = diskHierarchy.begin(); it != diskHierarchy.end(); ++it) {
        DiskHierarchy &hierarchy = it.value();

        for (const QJsonObject &part : std::as_const(hierarchy.partitions)) {
            const QString partName = part["name"].toString().trimmed();
            if (partName.isEmpty()) continue;

            const QString partDevicePath = QStringLiteral("/dev/") + partName;

            if (btrfsDevices.contains(partDevicePath) && !hierarchy.btrfsPartitions.contains(partName)) {
                hierarchy.btrfsPartitions.append(partName);
            }
        }
    }

    // ============================================================
    // DEVICE REPLACE - TARGET DEVICES
    // ============================================================

    ui->cmbReplaceTarget->clear();

    for (auto it = diskHierarchy.cbegin(); it != diskHierarchy.cend(); ++it)
    {
        const QString diskName = it.key();
        const DiskHierarchy &hierarchy = it.value();

        if (!isPhysicalDisk(hierarchy.disk)) continue;

        const QString diskPath = QStringLiteral("/dev/") + diskName;

        // --------------------------------------------------------
        // DISCO INTERO VUOTO
        // --------------------------------------------------------
        const QString diskFsType = hierarchy.disk["fstype"].toString().trimmed();
        const QString diskMount = hierarchy.disk["mountpoint"].toString().trimmed();
        const QString diskUuid = hierarchy.disk["uuid"].toString().trimmed();
        const QString diskSize = hierarchy.disk["size"].toString().trimmed();

        const bool wholeDiskCandidate = hierarchy.partitions.isEmpty() && !btrfsDevices.contains(diskPath) && diskFsType.isEmpty() && diskMount.isEmpty() && diskUuid.isEmpty();

        if (wholeDiskCandidate)
        {
            QString text = QStringLiteral("%1 - %2 (whole disk)").arg(diskPath, diskSize.isEmpty() ? QStringLiteral("unknown size") : diskSize);
            ui->cmbReplaceTarget->addItem(text, diskPath);
        }

        // --------------------------------------------------------
        // PARTIZIONI ESISTENTI VUOTE E NON MONTATE
        // --------------------------------------------------------
        for (const QJsonObject &part : std::as_const(hierarchy.partitions))
        {
            if (!isPhysicalDisk(part)) continue;

            const QString partName = part["name"].toString().trimmed();
            if (partName.isEmpty()) continue;

            const QString partPath = QStringLiteral("/dev/") + partName;
            if (btrfsDevices.contains(partPath)) continue;

            const QString partFsType = part["fstype"].toString().trimmed();
            const QString partMount = part["mountpoint"].toString().trimmed();
            const QString partUuid = part["uuid"].toString().trimmed();
            const QString partSize = part["size"].toString().trimmed();

            if (!partFsType.isEmpty() || !partMount.isEmpty() || !partUuid.isEmpty()) continue;

            QString text = QStringLiteral("%1 - %2 (partition)").arg(partPath, partSize.isEmpty() ? QStringLiteral("unknown size") : partSize);
            ui->cmbReplaceTarget->addItem(text, partPath);
        }
    }

    // ============================================================
    // VISUALIZZA DISCHI FISICI CON PARTIZIONI BTRFS
    // ============================================================

    // Prima passata: mostra i dischi fisici che contengono partizioni Btrfs
    for (auto it = diskHierarchy.begin(); it != diskHierarchy.end(); ++it) {
        const QString &diskName = it.key();
        const DiskHierarchy &hierarchy = it.value();

        // Filtro: Controlla se il disco fisico è valido
        if (!isPhysicalDisk(hierarchy.disk)) {
            qDebug() << "Saltato disco non valido:" << diskName;
            continue;
        }

        // Se il disco non ha partizioni Btrfs, salta (verrà gestito dopo)
        if (hierarchy.btrfsPartitions.isEmpty()) {
            continue;
        }

        // Determina lo stato del disco basato sulle partizioni
        QString diskStatus = getDiskStatusFromPartitions(hierarchy, btrfsDevices, btrfsErrorDevices);

        // Crea informazioni per il disco fisico
        DiskInfo diskInfo;
        diskInfo.devicePath = "/dev/" + diskName;
        diskInfo.name = diskName;
        diskInfo.filesystem = "disk";
        diskInfo.size = hierarchy.disk["size"].toString();
        diskInfo.uuid = "";
        diskInfo.mountPoint = "";
        diskInfo.status = diskStatus;
        diskInfo.isPartition = false;
        diskInfo.parentDisk = "";
        diskInfo.childPartitions = hierarchy.btrfsPartitions;

        // Se la dimensione del disco non è disponibile, calcolala dalla somma delle partizioni
        if (diskInfo.size.isEmpty() || diskInfo.size == "0") {
            quint64 totalSize = 0;
            for (const QJsonObject &part : hierarchy.partitions) {
                if (isPhysicalDisk(part)) {
                    QString partSize = part["size"].toString();
                    quint64 bytes = parseBtrfsSize(partSize);
                    totalSize += bytes;
                }
            }
            diskInfo.size = formatBytes(totalSize);
            diskInfo.totalDiskSize = totalSize;
        }

        populateSmartData(diskInfo);

        // Aggiungi la riga del disco fisico
        addDiskRow(diskInfo, false);

        // ============================================================
        // VISUALIZZA PARTIZIONI BTRFS DEL DISCO
        // ============================================================

        for (const QString &partName : hierarchy.btrfsPartitions) {
            QString devicePath = "/dev/" + partName;

            // Trova i dettagli della partizione
            QJsonObject partDev = findLsblkDevice(blockdevices, partName);

            if (!isPhysicalDisk(partDev)) {
                qDebug() << "Saltata partizione Btrfs non valida:" << partName;
                continue;
            }

            DiskInfo partInfo;
            partInfo.devicePath = devicePath;
            partInfo.btrfsDeviceId = btrfsDeviceIdsByPath.value(devicePath);
            partInfo.name = partName;
            partInfo.filesystem = "btrfs";
            partInfo.size = partDev["size"].toString();
            partInfo.uuid = !fsUuid.isEmpty() ? fsUuid : partDev["uuid"].toString();
            partInfo.mountPoint = mountPoint;
            partInfo.parentDisk = diskName;
            partInfo.isPartition = true;
            partInfo.status = "healthy";

            if (btrfsDevices.contains(devicePath)) {
                const BtrfsDeviceStats deviceStats = btrfsDeviceStats.value(devicePath);
                partInfo.readErrors = deviceStats.readErrors;
                partInfo.writeErrors = deviceStats.writeErrors;
                partInfo.flushErrors = deviceStats.flushErrors;
                partInfo.corruptionErrors = deviceStats.corruptionErrors;
                partInfo.generationErrors = deviceStats.generationErrors;

                if (btrfsErrorDevices.contains(devicePath)) {
                    partInfo.status = "error";
                } else {
                    partInfo.status = "healthy";
                }

                displayedDevices.insert(devicePath);
            } else {
                partInfo.status = "missing";
            }

            addDiskRow(partInfo, true);
        }
    }

    // ============================================================
    // VISUALIZZA DISCHI FISICI CON PARTIZIONI NON BTRFS
    // ============================================================

    // Seconda passata: mostra i dischi con partizioni NON Btrfs
    // Scorre l'elenco principale di tutti i dischi fisici disponibili nel sistema (es. sda, sdb, nvme0n1).
    for (auto it = diskHierarchy.begin(); it != diskHierarchy.end(); ++it) {
        const QString &diskName = it.key();
        const DiskHierarchy &hierarchy = it.value();

        // Filtro: Controlla se il disco fisico è valido
        if (!isPhysicalDisk(hierarchy.disk)) {
            qDebug() << "Saltato disco non valido:" << diskName;
            continue;
        }

        // Se il disco ha partizioni Btrfs, è già stato visualizzato
        if (!hierarchy.btrfsPartitions.isEmpty()) {
            continue;
        }

        // Verifica se il disco ha partizioni non Btrfs
        bool hasNonBtrfsPartitions = false;
        for (const QJsonObject &part : hierarchy.partitions) {
            if (isPhysicalDisk(part)) {
                QString partFstype = part["fstype"].toString();
                if (!partFstype.isEmpty() && partFstype != "btrfs") {
                    hasNonBtrfsPartitions = true;
                    break;
                }
            }
        }

        // Salta i dischi senza partizioni (verranno gestiti nella terza passata)
        if (!hasNonBtrfsPartitions) {
            continue;
        }

        DiskInfo diskInfo;
        diskInfo.devicePath = "/dev/" + diskName;
        diskInfo.name = diskName;
        diskInfo.filesystem = "disk";
        diskInfo.size = hierarchy.disk["size"].toString();
        diskInfo.uuid = "";
        diskInfo.mountPoint = "";
        diskInfo.status = "healthy";
        diskInfo.isPartition = false;
        diskInfo.parentDisk = "";
        diskInfo.canBeAddedToBtrfs = false;  // Ha già partizioni, non può essere aggiunto direttamente


        // Se la dimensione del disco non è disponibile, calcolala
        if (diskInfo.size.isEmpty() || diskInfo.size == "0") {
            quint64 totalSize = 0;
            for (const QJsonObject &part : hierarchy.partitions) {
                if (isPhysicalDisk(part)) {
                    QString partSize = part["size"].toString();
                    quint64 bytes = parseBtrfsSize(partSize);
                    totalSize += bytes;
                }
            }
            diskInfo.size = formatBytes(totalSize);
            diskInfo.totalDiskSize = totalSize;
        }

        populateSmartData(diskInfo);

        // Aggiungi la riga del disco
        addDiskRow(diskInfo, false);

        // Visualizza le partizioni non Btrfs
        // Per il singolo disco selezionato
        //dal ciclo esterno, scorre l'elenco di tutte le partizioni
        //associate a quel blocco di memoria (es. sda1, sda2).
        for (const QJsonObject &part : hierarchy.partitions) {
            QString partName = part["name"].toString();
            QString partFstype = part["fstype"].toString();
            QString devicePath = "/dev/" + partName;

            if (!isPhysicalDisk(part)) {
                qDebug() << "Saltata partizione non valida:" << partName;
                continue;
            }

            // Salta le partizioni Btrfs (già visualizzate) e quelle vuote
            if (partFstype == "btrfs" || partFstype.isEmpty()) {
                continue;
            }

            DiskInfo partInfo;
            partInfo.devicePath = devicePath;
            partInfo.name = partName;
            partInfo.filesystem = partFstype;
            partInfo.size = part["size"].toString();
            partInfo.uuid = part["uuid"].toString();
            partInfo.mountPoint = part["mountpoint"].toString();
            partInfo.parentDisk = diskName;
            partInfo.isPartition = true;
            partInfo.status = "healthy";
            partInfo.canBeAddedToBtrfs = false;  // È già una partizione esistente

            addDiskRow(partInfo, false);
        }
    }

    // ============================================================
    //  VISUALIZZA DISCHI VUOTI (SENZA PARTIZIONI)
    // ============================================================

    // Terza passata: mostra i dischi vuoti che possono essere aggiunti a Btrfs
    for (auto it = diskHierarchy.begin(); it != diskHierarchy.end(); ++it) {
        const QString &diskName = it.key();
        const DiskHierarchy &hierarchy = it.value();

        // Filtro: Controlla se il disco fisico è valido
        if (!isPhysicalDisk(hierarchy.disk)) {
            qDebug() << "Saltato disco non valido:" << diskName;
            continue;
        }

        // Se il disco ha partizioni (Btrfs o non Btrfs), è già stato visualizzato
        if (!hierarchy.partitions.isEmpty()) {
            continue;
        }

        const QString diskDevicePath = QStringLiteral("/dev/") + diskName;

        /*
        * Un disco intero può essere direttamente
        * membro Btrfs senza avere partizioni.
        * In quel caso non è un disco "vuoto"
        * aggiungibile.
        */
        if (btrfsDevices.contains(diskDevicePath))
        {
            continue;
        }

        // Questo è un disco vuoto - può essere aggiunto a Btrfs
        DiskInfo diskInfo;
        diskInfo.devicePath = diskDevicePath;
        diskInfo.name = diskName;
        diskInfo.filesystem = "disk";
        diskInfo.size = hierarchy.disk["size"].toString();
        diskInfo.uuid = "";
        diskInfo.mountPoint = "";
        diskInfo.status = "empty";  // Stato speciale per dischi vuoti
        diskInfo.isPartition = false;
        diskInfo.parentDisk = "";
        diskInfo.canBeAddedToBtrfs = true;  //Flag per indicare che può essere aggiunto

        // Se la dimensione non è disponibile, usa "Unknown"
        if (diskInfo.size.isEmpty() || diskInfo.size == "0") {
            diskInfo.size = "Unknown";
            diskInfo.totalDiskSize = 0;
        }

        populateSmartData(diskInfo);

        // Aggiungi la riga del disco vuoto
        addDiskRow(diskInfo, false);
    }

    // ============================================================
    // DEVICE BTRFS MISSING NON RAPPRESENTABILI DA LSBLK
    // ============================================================
    for (const BtrfsMonitor::DeviceState &missingDevice : std::as_const(unrepresentedMissingDevices)) {
        DiskInfo missingInfo;

        const QString missingPath = missingDevice.devicePath.trimmed();
        missingInfo.devicePath = missingPath.isEmpty() ? QStringLiteral("MISSING") : missingPath;
        missingInfo.btrfsDeviceId = missingDevice.deviceId.trimmed();
        missingInfo.name = missingInfo.btrfsDeviceId.isEmpty() ? QStringLiteral("MISSING DEVICE") : QStringLiteral("MISSING DEVICE (devid %1)").arg(missingInfo.btrfsDeviceId);
        missingInfo.filesystem = QStringLiteral("btrfs");
        missingInfo.size = QStringLiteral("-");
        missingInfo.uuid = fsUuid;
        missingInfo.mountPoint = mountPoint;
        missingInfo.status = QStringLiteral("missing");
        missingInfo.isPartition = false;
        missingInfo.isWholeDiskBtrfsMember = false;
        missingInfo.canBeAddedToBtrfs = false;
        missingInfo.readErrors = missingDevice.readErrors;
        missingInfo.writeErrors = missingDevice.writeErrors;
        missingInfo.flushErrors = missingDevice.flushErrors;
        missingInfo.corruptionErrors = missingDevice.corruptionErrors;
        missingInfo.generationErrors = missingDevice.generationErrors;

        addDiskRow(missingInfo, true);
    }

    // ============================================================
    // VISUALIZZA DEVICE BTRFS NON ASSOCIATI A DISCHI FISICI
    // ============================================================

    // Quarta passata: mostra eventuali device Btrfs non associati a dischi fisici
    for (const QString &devicePath : std::as_const(btrfsDevices)) {

        // Se il device è già stato visualizzato, salta
        if (displayedDevices.contains(devicePath)) {
            continue;
        }

        const QString deviceName = deviceNameFromPath(devicePath);
        const QJsonObject dev = findLsblkDevice(blockdevices, deviceName);

        if (!isPhysicalDisk(dev)) {
            qDebug() << "Saltato device Btrfs non valido:" << deviceName;
            continue;
        }

        DiskInfo info;
        info.devicePath = devicePath;
        info.btrfsDeviceId = btrfsDeviceIdsByPath.value(devicePath);
        info.name = deviceName;
        info.filesystem = dev["fstype"].toString();
        info.size = dev["size"].toString();
        info.uuid = dev["uuid"].toString();
        info.mountPoint = mountPoint;

        const bool isWholeDiskBtrfsMember = dev["type"].toString() == QStringLiteral("disk");
        info.isWholeDiskBtrfsMember =  isWholeDiskBtrfsMember;
        info.isPartition = !isWholeDiskBtrfsMember;

        info.parentDisk = "";
        info.canBeAddedToBtrfs = false;

        const BtrfsDeviceStats deviceStats = btrfsDeviceStats.value(devicePath);
        info.readErrors = deviceStats.readErrors;
        info.writeErrors = deviceStats.writeErrors;
        info.flushErrors = deviceStats.flushErrors;
        info.corruptionErrors = deviceStats.corruptionErrors;
        info.generationErrors = deviceStats.generationErrors;

        info.status = "healthy";
        if (btrfsErrorDevices.contains(devicePath)) {
            info.status = "error";
        }

        addDiskRow(info, true);
        displayedDevices.insert(devicePath);
    }

    // ============================================================
    // NESSUN DEVICE BTRFS
    // ============================================================

    if (btrfsDevices.isEmpty()) {
        qWarning() << "Nessun device Btrfs trovato per:" << mountPoint;

        DiskInfo info;
        info.devicePath = "";
        info.name = QString("Btrfs (%1)").arg(mountPoint);
        info.filesystem = "btrfs";
        info.size = "-";
        info.uuid = "-";
        info.mountPoint = mountPoint;
        info.status = "error";
        info.isPartition = false;
        info.canBeAddedToBtrfs = false;

        addDiskRow(info, true);
    }

    // ============================================================
    // FINAL SPACER
    // ============================================================

    if (scrollLayout)
        scrollLayout->addStretch();

}

// ============================================================================
// DISK SELECTION
// ============================================================================

void MainWindow::onDiskSelected(diskrowwidget *clickedWidget)
{
    if (!scrollLayout || !clickedWidget) return;

    // ============================================================
    // DESELEZIONA LE ALTRE ROW
    // ============================================================
    for (int i = 0; i < scrollLayout->count(); ++i) {
        QLayoutItem *item = scrollLayout->itemAt(i);
        if (!item || !item->widget()) continue;
        diskrowwidget *row = qobject_cast<diskrowwidget*>(item->widget());
        if (row) row->setSelected(row == clickedWidget);
    }

    // ============================================================
    // RECUPERA DISK INFO
    // ============================================================
    const DiskInfo &info = clickedWidget->getDiskInfo();

    // Aggiorna il disco selezionato

    // const DiskInfo &info = clickedWidget->getDiskInfo();
    currentSelectedDisk = info;  // Copia i dati
    isDiskSelected = true;

    // ============================================================
    // AGGIORNA PAGE DISK - INFORMAZIONI GENERALI
    // ============================================================

    ui->lblDiskDeviceValue->setText(info.devicePath.isEmpty() ? "-" : info.devicePath);
    ui->lblDiskNameValue->setText(info.name.isEmpty() ? "-" : info.name);
    ui->lblDiskSizeValue->setText(info.size.isEmpty() ? "-" : info.size);
    ui->lblDiskUuidValue->setText(info.uuid.isEmpty() ? "-" : info.uuid);
    ui->lblDiskMountPointValue->setText(info.mountPoint.isEmpty() ? "-" : info.mountPoint);

    // ============================================================
    // PULSANTE ADD / REMOVE BTRFS
    // ============================================================

    const bool canAddToBtrfs = info.canBeAddedToBtrfs && info.status == QStringLiteral("empty");
    const bool canRemoveMissingBtrfs = info.status == QStringLiteral("missing") && info.filesystem == QStringLiteral("btrfs") && !info.btrfsDeviceId.trimmed().isEmpty() && !info.mountPoint.trimmed().isEmpty();

    bool removeIdOk = false;
    const qulonglong removeDeviceId = info.btrfsDeviceId.trimmed().toULongLong(&removeIdOk);
    const bool removalInProgress = canRemoveMissingBtrfs && removeIdOk && removeDeviceId > 0 && isRemovalActive(info.mountPoint, removeDeviceId);

    ui->btnAddToBtrfs->setVisible(canAddToBtrfs || canRemoveMissingBtrfs);

    if (canRemoveMissingBtrfs) {
        ui->btnAddToBtrfs->setEnabled(!removalInProgress);
        ui->btnAddToBtrfs->setText(removalInProgress ? QStringLiteral("Removing device %1...").arg(info.btrfsDeviceId) : QStringLiteral("Remove device %1").arg(info.btrfsDeviceId));
        ui->btnAddToBtrfs->setToolTip(removalInProgress ? QStringLiteral("Device removal is already in progress") : QStringLiteral("Remove this missing device from the Btrfs filesystem"));
    } else if (canAddToBtrfs) {
        ui->btnAddToBtrfs->setEnabled(!m_addDevicePending);
        ui->btnAddToBtrfs->setText(QStringLiteral("Add %1 to Btrfs").arg(info.name));
        ui->btnAddToBtrfs->setToolTip(QStringLiteral("Add this device to the Btrfs filesystem"));
    } else {
        ui->btnAddToBtrfs->setEnabled(false);
        isDiskSelected = false;
    }

    // ============================================================
    // GESTIONE DISCO FISICO VS PARTIZIONE
    // ============================================================

    QString iconPath;
    QString statusText;
    QString description;
    QString color;

    if (!info.isPartition) {

        const bool isWholeDiskBtrfs = info.isWholeDiskBtrfsMember;

        // ============================================================
        // DISCO FISICO
        // ============================================================

        ui->lblDiskFilesystemValue->setText(isWholeDiskBtrfs ? QStringLiteral("Btrfs whole-disk member") : QStringLiteral("Physical Disk"));

        ui->lblDiskStatusValue->setText(info.status);

        if (isWholeDiskBtrfs)
        {
            ui->lblDiskBtrfsPartitionsValue->setText(QStringLiteral("Whole-disk member"));
        }
        else if (!info.childPartitions.isEmpty())
        {
            ui->lblDiskBtrfsPartitionsValue->setText(QString("%1 partition(s)").arg(info.childPartitions.size()));
        }
        else
        {
            ui->lblDiskBtrfsPartitionsValue->setText(QStringLiteral("No Btrfs partitions"));
        }

        ui->groupBtrfsErrors->setVisible(isWholeDiskBtrfs);

        if (isWholeDiskBtrfs)
        {
            ui->lblReadErrors->setText(QString("Read errors = %1").arg(info.readErrors));
            ui->lblWriteErrors->setText(QString("Write errors = %1").arg(info.writeErrors));
            ui->lblFlushErrors->setText(QString("Flush errors = %1").arg(info.flushErrors));
            ui->lblCorruptionErrors->setText(QString("Corruption errors = %1").arg(info.corruptionErrors));
            ui->lblGenerationErrors->setText(QString("Generation errors = %1").arg(info.generationErrors));
        }

        // ============================================================
        // SMART - LETTURA DELLO STATO CORRENTE DALLA CACHE
        // ============================================================

        BtrfsMonitor::SmartDeviceState smart;

        const bool hasSmartState = m_btrfsMonitor && m_btrfsMonitor->smartDeviceState(info.devicePath, smart);

        ui->groupSmartInfo->setVisible(hasSmartState);

        if (hasSmartState)
        {
            QString smartStatus;

            if (!smart.present) { smartStatus = QStringLiteral("Missing"); }
            else if (!smart.readOk) { smartStatus = QStringLiteral("Read error"); }
            else if (smart.smartSupported == 0) { smartStatus = QStringLiteral("N/A"); }
            else if (smart.smartSupported < 0) { smartStatus = QStringLiteral("Unknown"); }
            else
            {
                switch (smart.health)
                {
                case BtrfsMonitor::HealthState::Ok: smartStatus = QStringLiteral("OK"); break;
                case BtrfsMonitor::HealthState::Warning: smartStatus = QStringLiteral("Warning"); break;
                case BtrfsMonitor::HealthState::Error: smartStatus = QStringLiteral("Error"); break;
                case BtrfsMonitor::HealthState::Unknown:
                default: smartStatus = QStringLiteral("Unknown"); break;
                }
            }

            ui->lblSMART_status->setText(QStringLiteral("Status: %1").arg(smartStatus));
            ui->lblSMART_temp->setText(smart.temperatureCelsius >= 0 ? QStringLiteral("Temperature: %1 °C").arg(smart.temperatureCelsius) : QStringLiteral("Temperature: N/A"));
            ui->lblSMART_power->setText(QStringLiteral("Power-on hours: %1").arg(smart.powerOnHours));
            ui->lblSMART_reallocated->setText(QStringLiteral("Reallocated sectors: %1").arg(smart.reallocatedSectors));
            ui->lblSMART_pending->setText(QStringLiteral("Pending sectors: %1").arg(smart.pendingSectors));
            ui->lblSMART_crc->setText(QStringLiteral("CRC errors: %1").arg(smart.crcErrors));
            ui->lblSMART_reason->setText(QStringLiteral("Health reason: %1").arg(smart.healthReason.trimmed().isEmpty() ? QStringLiteral("-") : smart.healthReason));


            QString smartColor = QStringLiteral("#888");
            bool smartEmphasis = false;

            if (smart.smartSupported > 0 && smart.present && smart.readOk)
            {
                switch (smart.health)
                {
                case BtrfsMonitor::HealthState::Ok:      smartColor = QStringLiteral("#4CAF50"); break;
                case BtrfsMonitor::HealthState::Warning: smartColor = QStringLiteral("#FF9800"); smartEmphasis = true; break;
                case BtrfsMonitor::HealthState::Error:   smartColor = QStringLiteral("#F44336"); smartEmphasis = true; break;
                case BtrfsMonitor::HealthState::Unknown:
                default:                                 smartColor = QStringLiteral("#888"); break;
                }
            }

            ui->lblSMART_status->setStyleSheet(QStringLiteral("color: %1; font-weight: %2;").arg(smartColor, smartEmphasis ? QStringLiteral("bold") : QStringLiteral("normal")));
            ui->lblSMART_reason->setStyleSheet(QStringLiteral("color: %1; font-weight: %2;").arg(smartColor, smartEmphasis ? QStringLiteral("bold") : QStringLiteral("normal")));
            ui->lblSMART_temp->setStyleSheet(smartEmphasis ? QStringLiteral("color: %1; font-weight: bold;").arg(smartColor) : QString());


        }

        const bool smartError = hasSmartState && smart.present && smart.readOk && smart.smartSupported > 0 && smart.health == BtrfsMonitor::HealthState::Error;

        const bool smartWarning = hasSmartState && (!smart.present || !smart.readOk || (smart.smartSupported > 0 &&  smart.health == BtrfsMonitor::HealthState::Warning));

        // ============================================================
        // STATO VISIVO DEL DISCO FISICO
        //
        // Priorità:
        // 1. Btrfs / stato disco se non healthy
        // 2. SMART Error
        // 3. SMART Warning
        // 4. Healthy
        //
        // SMART unsupported (smartSupported == 0) resta neutro.
        // ============================================================

        if (info.status == "error")
        {
            iconPath = ":/icons/hhd_alarm.png";
            statusText = isWholeDiskBtrfs ? QStringLiteral("BTRFS DEVICE ERROR") : QStringLiteral("DEVICE ERROR");
            description = isWholeDiskBtrfs ? QStringLiteral("Whole-disk Btrfs member has reported errors.") : QStringLiteral("Physical disk has reported Btrfs errors.");
            color = "#F44336";
        }
        else if (info.status == "warning" || info.status == "missing")
        {
            iconPath = ":/icons/hhd_alarm.png";
            statusText = isWholeDiskBtrfs ? QStringLiteral("BTRFS DEVICE WARNING") : QStringLiteral("DEVICE WARNING");
            description = isWholeDiskBtrfs ? QStringLiteral("Whole-disk Btrfs member requires attention.") : QStringLiteral("Physical disk requires attention.");
            color = "#FF9800";
        }
        else if (smartError)
        {
            iconPath = ":/icons/hhd_alarm.png";
            statusText = "DEVICE ERROR";
            description = "SMART reports an error condition.";
            color = "#F44336";
        }
        else if (smartWarning)
        {
            iconPath = ":/icons/hhd_alarm.png";
            statusText = "DEVICE WARNING";

            if (hasSmartState &&
                !smart.healthReason.trimmed().isEmpty())
            {
                description = smart.healthReason;
            }
            else
            {
                description = "SMART reports a warning condition.";
            }

            color = "#FF9800";
        }
        else if (info.status == "healthy")
        {
            iconPath = ":/icons/hhd_ok.png";

            if (isWholeDiskBtrfs)
            {
                statusText = QStringLiteral("BTRFS DEVICE HEALTHY");
                description = QStringLiteral("Whole-disk Btrfs member is operating normally.");
            }
            else
            {
                statusText = QStringLiteral("DEVICE HEALTHY");
                description = QStringLiteral("Physical disk is operating normally.");
            }
            color = "#4CAF50";
        }
        else
        {
            iconPath = ":/icons/hhd_ko.png";
            statusText = "DEVICE UNKNOWN";
            description = "Physical disk status is unknown.";
            color = "#888";
        }

    } else {
        // ============================================================
        // PARTIZIONE
        // ============================================================

        ui->groupSmartInfo->setVisible(false);

        ui->lblDiskFilesystemValue->setText(info.filesystem.isEmpty() ? "-" : info.filesystem);
        ui->lblDiskStatusValue->setText(info.status);

        if (!info.parentDisk.isEmpty()) {
            ui->lblDiskBtrfsPartitionsValue->setText(QString("Parent: %1").arg(info.parentDisk));
        } else {
            ui->lblDiskBtrfsPartitionsValue->setText("-");
        }

        ui->groupBtrfsErrors->setVisible(true);
        ui->lblReadErrors->setText(QString("Read errors = %1").arg(info.readErrors));
        ui->lblWriteErrors->setText(QString("Write errors = %1").arg(info.writeErrors));
        ui->lblFlushErrors->setText(QString("Flush errors = %1").arg(info.flushErrors));
        ui->lblCorruptionErrors->setText(QString("Corruption errors = %1").arg(info.corruptionErrors));
        ui->lblGenerationErrors->setText(QString("Generation errors = %1").arg(info.generationErrors));

        if (info.filesystem == "btrfs") {
            if (info.status == "healthy") {
                iconPath = ":/icons/btrfs_ok.png";
                statusText = "BTRFS HEALTHY";
                description = "Btrfs partition is operating normally.";
                color = "#4CAF50";
            } else if (info.status == "error") {
                iconPath = ":/icons/btrfs_error.png";
                statusText = "BTRFS ERROR";
                description = "Btrfs partition has reported errors.";
                color = "#F44336";
            } else if (info.status == "missing") {
                iconPath = ":/icons/btrfs_missing.png";
                statusText = "BTRFS MISSING";
                description = "Btrfs partition is missing.";
                color = "#FF9800";
            } else {
                iconPath = ":/icons/btrfs_ok.png";
                statusText = "BTRFS UNKNOWN";
                description = "Btrfs partition status is unknown.";
                color = "#888";
            }
        } else {
            if (info.status == "healthy") {
                iconPath = ":/icons/hhd_ok.png";
                statusText = "PARTITION HEALTHY";
                description = "Partition is operating normally.";
                color = "#4CAF50";
            } else if (info.status == "error") {
                iconPath = ":/icons/hhd_alarm.png";
                statusText = "PARTITION ERROR";
                description = "Partition has reported errors.";
                color = "#F44336";
            } else {
                iconPath = ":/icons/hhd_ko.png";
                statusText = "PARTITION UNKNOWN";
                description = "Partition status is unknown.";
                color = "#888";
            }
        }
    }

    // ============================================================
    // DEBUG OUTPUT
    // ============================================================

    qDebug() << "========================================";
    qDebug() << "DEVICE:" << info.devicePath;
    qDebug() << "NAME:" << info.name;
    qDebug() << "FILESYSTEM:" << info.filesystem;
    qDebug() << "SIZE:" << info.size;
    qDebug() << "UUID:" << info.uuid;
    qDebug() << "MOUNT:" << info.mountPoint;
    qDebug() << "STATUS:" << info.status;
    qDebug() << "IS PARTITION:" << info.isPartition;
    qDebug() << "PARENT DISK:" << info.parentDisk;
    qDebug() << "CHILD PARTITIONS:" << info.childPartitions;
    qDebug() << "========================================";

    // ============================================================
    // IMPOSTAZIONE ICON E LABEL
    // ============================================================

    QPixmap pixmap(iconPath);
    if (!pixmap.isNull()) {
        ui->lbl_icodisk->setPixmap(pixmap.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        ui->lbl_icodisk->setFixedSize(48, 48);
    } else {
        // Icona di fallback se l'immagine non viene trovata
        ui->lbl_icodisk->setText("💾");
        ui->lbl_icodisk->setStyleSheet("font-size: 48px;");
    }

    // Imposta il testo dello stato
    ui->lblDiskStatus->setText(statusText);
    ui->lblDiskStatus->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color));

    // Imposta la descrizione
    ui->lblDiskDescription->setText(description);

    // ============================================================
    // CAMBIA PAGINA
    // ============================================================
    ui->stackedWidget->setCurrentWidget(ui->pageDisk);

}

//=======================================================================
// FILTRO DISPOSITIVI PER LA LISTA
//=======================================================================
bool MainWindow::isPhysicalDisk(const QJsonObject &device) const
{
    QString type = device["type"].toString();
    QString fstype = device["fstype"].toString();
    QString name = device["name"].toString();

    // Esclude i lettori CD/DVD
    if (type == "rom" || type == "cdrom") {
        qDebug() << "Escluso CD/DVD:" << name;
        return false;
    }

    // Esclude le partizioni di swap
    if (fstype == "swap") {
        qDebug() << "Escluso swap:" << name;
        return false;
    }

    // Esclude dispositivi loop (mount di file immagine)
    if (name.startsWith("loop")) {
        qDebug() << "Escluso loop device:" << name;
        return false;
    }

    // Esclude dispositivi mapper (LUKS, LVM)
    if (name.contains("dm-")) {
        qDebug() << "Escluso device mapper:" << name;
        return false;
    }

    // Accetta solo "disk" e "part"
    bool valid = (type == "disk" || type == "part");
    if (!valid) {
        qDebug() << "Escluso tipo non supportato:" << name << "(" << type << ")";
    }
    return valid;

}

// ============================================================================
// BUILD DISK HIERARCHY
// ============================================================================

QMap<QString, MainWindow::DiskHierarchy> MainWindow::buildDiskHierarchy(const QJsonArray &blockdevices)
{
    QMap<QString, DiskHierarchy> hierarchy;

    // Prima passata: trova tutti i dischi fisici
    for (const QJsonValue &value : std::as_const(blockdevices)) {
        if (!value.isObject()) continue;

        const QJsonObject dev = value.toObject();
        const QString name = dev["name"].toString();
        const QString type = dev["type"].toString();

        if (type == "disk") {
            DiskHierarchy hier;
            hier.disk = dev;

            // Cerca le partizioni (children)
            if (dev.contains("children") && dev["children"].isArray()) {
                // uso std::as_const o un loop con indice
                const QJsonArray children = dev["children"].toArray();

                // Opzione A: std::as_const
                for (const QJsonValue &childVal : std::as_const(children)) {
                    if (!childVal.isObject()) continue;

                    QJsonObject child = childVal.toObject();
                    QString childFstype = child["fstype"].toString();

                    hier.partitions.append(child);

                    if (childFstype == "btrfs") {
                        hier.btrfsPartitions.append(child["name"].toString());
                    }
                }
            }

            hierarchy.insert(name, hier);
        }
    }

    // Seconda passata: trova le partizioni che potrebbero non avere un disco padre
    for (const QJsonValue &value : std::as_const(blockdevices)) {
        if (!value.isObject()) continue;

        const QJsonObject dev = value.toObject();
        const QString name = dev["name"].toString();
        const QString type = dev["type"].toString();

        if (type == "part") {
            // Estrai il nome del disco padre (es. "sda" da "sda1")
            QString diskName = name;
            // Sostituisci la tua riga originale con questo blocco:
            static const QRegularExpression trailingDigitsRegex(QStringLiteral("\\d+$")); diskName.remove(trailingDigitsRegex);

            // Se il disco padre non è ancora nella gerarchia, crea un entry base
            if (!hierarchy.contains(diskName)) {
                DiskHierarchy hier;
                QJsonObject baseDisk;
                baseDisk["name"] = diskName;
                baseDisk["type"] = "disk";
                baseDisk["size"] = "0";
                hier.disk = baseDisk;
                hierarchy.insert(diskName, hier);
            }

            // Aggiungi la partizione al disco padre se non è già presente
            bool alreadyExists = false;
            for (const QJsonObject &existing : std::as_const(hierarchy[diskName].partitions)) {
                if (existing["name"].toString() == name) {
                    alreadyExists = true;
                    break;
                }
            }

            if (!alreadyExists) {
                hierarchy[diskName].partitions.append(dev);

                if (dev["fstype"].toString() == "btrfs") {
                    hierarchy[diskName].btrfsPartitions.append(name);
                }
            }
        }
    }

    return hierarchy;

}

// ============================================================================
// GET DISK STATUS FROM PARTITIONS
// ============================================================================

QString MainWindow::getDiskStatusFromPartitions(const DiskHierarchy &hierarchy, const QSet<QString> &btrfsDevices, const QSet<QString> &btrfsErrorDevices)
{
    // Se il disco non ha partizioni Btrfs, considera healthy
    if (hierarchy.btrfsPartitions.isEmpty()) {
        return "healthy";
    }

    bool hasErrors = false;
    bool hasMissing = false;

    for (const QString &partName : hierarchy.btrfsPartitions) {
        QString devicePath = "/dev/" + partName;

        // Controlla se la partizione è presente
        if (!btrfsDevices.contains(devicePath)) {
            hasMissing = true;
            continue;
        }

        // Controlla se la partizione ha errori
        if (btrfsErrorDevices.contains(devicePath)) {
            hasErrors = true;
        }
    }

    if (hasErrors) return "error";
    if (hasMissing) return "warning";
    return "healthy";

}

// ============================================================================
// ADD DISK TO BTRFS
// ============================================================================

void MainWindow::addDiskToBtrfs(const DiskInfo &diskInfo)
{
    // ============================================================
    // PREVIENI CHIAMATE MULTIPLE
    // ============================================================
    if (m_addDevicePending) {
        qDebug() << "AddDevice already in progress, skipping duplicate call";
        return;
    }

    // ============================================================
    // VERIFICA PRELIMINARE
    // ============================================================
    if (diskInfo.status != QStringLiteral("empty")) {
        QMessageBox::warning(this, "Cannot Add Disk", QString("Disk %1 is not empty. Status: %2").arg(diskInfo.name, diskInfo.status));
        return;
    }

    const QString mountPoint = ui->comboBoxMount->currentText().trimmed();
    if (mountPoint.isEmpty()) {
        QMessageBox::warning(this, "Error", "No mount point selected!");
        return;
    }

    if (diskInfo.filesystem != QStringLiteral("empty") && diskInfo.filesystem != QStringLiteral("disk") && diskInfo.filesystem != QStringLiteral("btrfs")) {
        QMessageBox::warning(this, "Cannot Add Device", QString("Device %1 has an existing filesystem (%2).\n\nTo add this device to Btrfs:\n1. The device must be EMPTY (no filesystem)\n2. Or already formatted as Btrfs").arg(diskInfo.name, diskInfo.filesystem));
        return;
    }

    if (!m_agentDbusClient) {
        QMessageBox::critical(this, "Service Error", "Storage Monitor Agent D-Bus client is not available.");
        return;
    }

    // ============================================================
    // CONFERMA UTENTE
    // ============================================================
    const QMessageBox::StandardButton reply = QMessageBox::question(this, "Add Disk to Btrfs", QString("Are you sure you want to add %1 (%2) to the Btrfs filesystem?\nMount point: %3\nAdministrative authorization will be requested.\nMake sure the disk is empty and not in use.").arg(diskInfo.name, diskInfo.size, mountPoint), QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    // ============================================================
    // D-BUS REQUEST
    // ============================================================
    m_addDevicePending = true;
    m_pendingAddDeviceName = diskInfo.name;

    ui->btnAddToBtrfs->setEnabled(false);
    ui->btnAddToBtrfs->setText(QStringLiteral("Adding device..."));

    m_agentDbusClient->addDevice(mountPoint, diskInfo.devicePath);
}


// ============================================================
// BILANCIAMENTO MANUALE
// ============================================================

void MainWindow::balanceBtrfs(const QString &mountPoint)
{
    const QString path = mountPoint.trimmed();

    if (path.isEmpty())
    {
        QMessageBox::warning(
            this,
            "Error",
            "No mount point selected!");

        return;
    }

    if (isBalanceRunning)
    {
        QMessageBox::warning(
            this,
            "Balance Running",
            "A balance operation is already running.");

        return;
    }

    if (!m_agentDbusClient)
    {
        QMessageBox::critical(
            this,
            "Service Error",
            "Storage Monitor Agent D-Bus client is not available.");

        return;
    }

    const QMessageBox::StandardButton reply =
        QMessageBox::question(
            this,
            "Balance Filesystem",
            QString(
                "This will balance the Btrfs filesystem at:\n"
                "%1\n\n"
                "This operation may take several minutes and "
                "can impact system performance.\n\n"
                "The balance is managed by storage-monitor-agent "
                "and will continue even if this application is closed.\n\n"
                "Administrative authorization will be requested.\n\n"
                "Continue?")
                .arg(path),
            QMessageBox::Yes |
                QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    currentBalanceMount =
        path;

    setBalanceState(QStringLiteral("starting"), QStringLiteral("Waiting for authorization..."));

    m_agentDbusClient->startBalance(path);
}

void MainWindow::setBalanceState(const QString &state, const QString &status)
{
    currentBalanceState = state;

    ui->grp_Bilanciamento->setVisible(true);

    if (state == QStringLiteral("ready"))
    {
        ui->progress_Bilaciamento->setRange(0, 100);

        ui->progress_Bilaciamento->setVisible(false);

        ui->progress_Bilaciamento->setValue(0);

        ui->lbl_Manu_Balance->setText(status);

        ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance"));

        ui->btnBalance->setEnabled(true);

        ui->btnStopBalance->setEnabled(false);
    }
    else if (state == QStringLiteral("starting"))
    {
        ui->progress_Bilaciamento->setVisible(true);

        ui->progress_Bilaciamento->setRange(0, 0);

        ui->lbl_Manu_Balance->setText(status.isEmpty()
                    ? QStringLiteral(
                          "Starting balance...")
                    : status);

        ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance Starting"));

        ui->btnBalance->setEnabled(false);

        ui->btnStopBalance->setEnabled(false);
    }
    else if (state == QStringLiteral("running"))
    {
        ui->progress_Bilaciamento->setVisible(true);

        ui->progress_Bilaciamento->setRange(0, 0);

        ui->lbl_Manu_Balance->setText(status.isEmpty()
                    ? QStringLiteral("Balance in progress...")
                    : status);

        ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance Running"));

        ui->btnBalance->setEnabled(false);

        ui->btnStopBalance->setEnabled(true);
    }
    else if (state == QStringLiteral("paused"))
    {
        ui->progress_Bilaciamento->setVisible(true);

        ui->progress_Bilaciamento->setRange(0, 0);

        ui->lbl_Manu_Balance->setText(status.isEmpty()
                    ? QStringLiteral("Balance paused")
                    : status);

        ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance Paused"));

        ui->btnBalance->setEnabled(false);

        ui->btnStopBalance->setEnabled(true);
    }
    else if (state == QStringLiteral("stopping"))
    {
        ui->progress_Bilaciamento->setVisible(true);

        ui->progress_Bilaciamento->setRange(0, 0);

        ui->lbl_Manu_Balance->setText(status.isEmpty()
                    ? QStringLiteral("Stopping balance...")
                    : status);

        ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance Stopping"));

        ui->btnBalance->setEnabled(false);

        ui->btnStopBalance->setEnabled(false);
    }
    else if (state == QStringLiteral("completed"))
    {
        ui->progress_Bilaciamento->setVisible(true);

        ui->progress_Bilaciamento->setRange(0, 100);

        ui->progress_Bilaciamento->setValue(100);

        ui->lbl_Manu_Balance->setText(status.isEmpty()
                    ? QStringLiteral("Balance completed!")
                    : status);

        ui->grp_Bilanciamento->setTitle(
                QStringLiteral("Balance Completed"));

        ui->btnBalance->setEnabled(true);

        ui->btnStopBalance->setEnabled(false);
    }
    else if (state == QStringLiteral("stopped"))
    {
        ui->progress_Bilaciamento->setRange(0, 100);

        ui->progress_Bilaciamento->setVisible(false);

        ui->progress_Bilaciamento->setValue(0);

        ui->lbl_Manu_Balance->setText(status.isEmpty()
                    ? QStringLiteral("Balance stopped")
                    : status);

        ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance Stopped"));

        ui->btnBalance->setEnabled(true);

        ui->btnStopBalance->setEnabled(false);
    }
    else if (state == QStringLiteral("error"))
    {
        ui->progress_Bilaciamento->setRange(0, 100);

        ui->progress_Bilaciamento->setVisible(false);

        ui->progress_Bilaciamento->setValue(0);

        ui->lbl_Manu_Balance->setText(status.isEmpty()
                    ? QStringLiteral("Balance error")
                    : status);

        ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance Error"));

        ui->btnBalance->setEnabled(true);

        ui->btnStopBalance->setEnabled(false);
    }
}

void MainWindow::setScrubState(const QString &state, const QString &status)
{
    currentScrubState = state;
    QString overviewState = state;

    if (overviewState == QStringLiteral("ready"))
    {
        overviewState = QStringLiteral("not running");
    }

    ui->lbl_Scrub->setText(QStringLiteral("Scrub: %1").arg(overviewState));
    ui->grp_Scrub->setVisible(true);

    if (state == QStringLiteral("ready"))
    {
        isScrubRunning = false;
        ui->progress_Scrub->setRange(0, 100); ui->progress_Scrub->setValue(0); ui->progress_Scrub->setVisible(false);
        ui->lbl_Status_Scrub->setText(status.isEmpty() ? QStringLiteral("Ready") : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub"));
        ui->btnStartScrub->setEnabled(true); ui->btnStopScrub->setEnabled(false);
    }
    else if (state == QStringLiteral("starting"))
    {
        isScrubRunning = true;
        ui->progress_Scrub->setVisible(true); ui->progress_Scrub->setRange(0, 0);
        ui->lbl_Status_Scrub->setText(status.isEmpty() ? QStringLiteral("Starting scrub...") : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub Starting"));
        ui->btnStartScrub->setEnabled(false); ui->btnStopScrub->setEnabled(false);
    }
    else if (state == QStringLiteral("running"))
    {
        isScrubRunning = true;
        if (ui->progress_Scrub->maximum() == 0) ui->progress_Scrub->setVisible(true);
        ui->lbl_Status_Scrub->setText(status.isEmpty() ? QStringLiteral("Scrub in progress...") : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub Running"));
        ui->btnStartScrub->setEnabled(false); ui->btnStopScrub->setEnabled(true);
    }
    else if (state == QStringLiteral("stopping"))
    {
        isScrubRunning = true;
        ui->lbl_Status_Scrub->setText(status.isEmpty() ? QStringLiteral("Stopping scrub...") : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub Stopping"));
        ui->btnStartScrub->setEnabled(false); ui->btnStopScrub->setEnabled(false);
    }
    else if (state == QStringLiteral("finished"))
    {
        isScrubRunning = false;
        ui->progress_Scrub->setVisible(true); ui->progress_Scrub->setRange(0, 100); ui->progress_Scrub->setValue(100); ui->progress_Scrub->setFormat(QStringLiteral("100%"));
        ui->lbl_Status_Scrub->setText(status.isEmpty() ? QStringLiteral("Scrub completed") : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub Completed"));
        ui->btnStartScrub->setEnabled(true); ui->btnStopScrub->setEnabled(false);
    }
    else if (state == QStringLiteral("cancelled"))
    {
        isScrubRunning = false;
        ui->progress_Scrub->setVisible(true);
        ui->lbl_Status_Scrub->setText(status.isEmpty() ? QStringLiteral("Scrub cancelled") : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub Cancelled"));
        ui->btnStartScrub->setEnabled(true); ui->btnStopScrub->setEnabled(false);
    }
    else if (state == QStringLiteral("interrupted"))
    {
        isScrubRunning = false;

        /*
     * Se conosciamo già una percentuale raggiunta,
     * la lasciamo visibile. Se non abbiamo dati,
     * non mostriamo una barra a 0% che potrebbe
     * essere fuorviante.
     */
        ui->progress_Scrub->setRange(0, 100);

        if (ui->progress_Scrub->value() > 0)
        {
            ui->progress_Scrub->setVisible(true);
        }
        else
        {
            ui->progress_Scrub->setVisible(false);
        }

        ui->lbl_Status_Scrub->setText(status.isEmpty()
                ? QStringLiteral("Scrub interrupted")
                : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub Interrupted"));
        ui->btnStartScrub->setEnabled(true);
        ui->btnStopScrub->setEnabled(false);
    }
    else if (state == QStringLiteral("error"))
    {
        isScrubRunning = false;
        ui->progress_Scrub->setVisible(false);
        ui->lbl_Status_Scrub->setText(status.isEmpty() ? QStringLiteral("Scrub error") : status);
        ui->grp_Scrub->setTitle(QStringLiteral("Scrub Error"));
        ui->btnStartScrub->setEnabled(true); ui->btnStopScrub->setEnabled(false);
    }
}

void MainWindow::saveAutoScrubConfig()
{
    if (!m_agentDbusClient) return;

    const QString mountPoint = ui->comboBoxMount->currentText().trimmed();
    if (mountPoint.isEmpty()) return;

    bool intervalOk = false;
    const int intervalWeeks = ui->cmbIntervalScrub->currentData().toInt(&intervalOk);

    if (!intervalOk || intervalWeeks < 1 || intervalWeeks > 6) { qWarning() << "Invalid automatic scrub interval in GUI:" << intervalWeeks; return; }

    const bool enabled = ui->chkAutoScrub->isChecked();

    /* Evitiamo modifiche multiple mentre Polkit e il daemon stanno elaborando la richiesta. */
    ui->chkAutoScrub->setEnabled(false);
    ui->cmbIntervalScrub->setEnabled(false);

    qInfo().noquote() << "Saving automatic scrub configuration:" << mountPoint << "enabled:" << enabled << "interval:" << intervalWeeks << "week(s)";

    m_agentDbusClient->setAutoScrubConfig(mountPoint, enabled, intervalWeeks);
}

void MainWindow::refreshBalanceDataFromDatabase(const QString &mountPoint)
{
    if (mountPoint.isEmpty())
        return;

    if (ui->comboBoxMount->currentText() != mountPoint)
        return;

    BtrfsInfo info = updateBtrfsInformation(mountPoint);

    populateDeviceSummary(info, mountPoint);

    updateBtrfsOverview(info);
    updateFilesystemHealth(info);
}

void MainWindow::on_btn_Status_clicked()
{
    // ============================================================
    // CAMBIO PAGINA
    // ===========================================================
    ui->stackedWidget->setCurrentWidget(ui->pageInfo);

}

// ============================================================
// PULSANTE AGGIUNGI DISCO E TOGLI DISCO X MISSING
// ===========================================================

void MainWindow::on_btnAddToBtrfs_clicked()
{
    if (!isDiskSelected)
    {
        qDebug() << "Nessun disco selezionato";
        return;
    }

    const bool isMissingBtrfsDevice = currentSelectedDisk.status == QStringLiteral("missing") && currentSelectedDisk.filesystem == QStringLiteral("btrfs") &&
        !currentSelectedDisk.btrfsDeviceId.trimmed().isEmpty() && !currentSelectedDisk.mountPoint.trimmed().isEmpty();

    if (isMissingBtrfsDevice)
    {
        bool ok = false;

        const qulonglong deviceId = currentSelectedDisk.btrfsDeviceId.trimmed().toULongLong(&ok);

        if (!ok || deviceId == 0)
        {
            QMessageBox::warning(this, QStringLiteral("Remove Device"), QStringLiteral("Invalid Btrfs device ID."));
            return;
        }

        const QString mountPoint = currentSelectedDisk.mountPoint.trimmed();

        // ============================================================
        // EVITA UNA SECONDA RIMOZIONE DELLO STESSO DEVICE
        // ============================================================

        if (isRemovalActive(mountPoint, deviceId))
        {
            QMessageBox::information(this, QStringLiteral("Remove Device"), QStringLiteral("Removal of Btrfs device %1 is already in progress.").arg(deviceId));
            return;
        }

        const QMessageBox::StandardButton reply =
            QMessageBox::question(this, QStringLiteral("Remove Device"), QStringLiteral("Remove missing Btrfs device %1 from filesystem %2?")
                    .arg(deviceId)
                    .arg(mountPoint), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (reply != QMessageBox::Yes)
            return;

        ui->btnAddToBtrfs->setEnabled(false);

        ui->btnAddToBtrfs->setText(QStringLiteral("Starting removal..."));

        m_activeRemovals.insert(mountPoint, deviceId);

        m_agentDbusClient->removeDevice(mountPoint, deviceId);

        return;
    }

    addDiskToBtrfs(currentSelectedDisk);
}

void MainWindow::on_btnStartReplace_clicked()
{
    if (!m_agentDbusClient) { QMessageBox::critical(this, QStringLiteral("Device Replace"), QStringLiteral("Storage Monitor Agent is not available.")); return; }

    const QString mountPoint = ui->comboBoxMount->currentText().trimmed();
    if (mountPoint.isEmpty()) { QMessageBox::warning(this, QStringLiteral("Device Replace"), QStringLiteral("No mount point selected.")); return; }

    bool sourceOk = false;
    const qulonglong sourceDeviceId = ui->cmbReplaceSource->currentData().toULongLong(&sourceOk);
    const QString targetDevicePath = ui->cmbReplaceTarget->currentData().toString().trimmed();

    if (!sourceOk || sourceDeviceId == 0 || targetDevicePath.isEmpty()) { QMessageBox::warning(this, QStringLiteral("Device Replace"), QStringLiteral("Select a valid source and target device.")); return; }
    if (isReplaceRunning) { QMessageBox::information(this, QStringLiteral("Device Replace"), QStringLiteral("A device replacement is already running.")); return; }

    const QString sourceText = ui->cmbReplaceSource->currentText();
    const QString targetText = ui->cmbReplaceTarget->currentText();

    const QMessageBox::StandardButton reply = QMessageBox::question(this, QStringLiteral("Start Device Replace"), QStringLiteral("Replace the following Btrfs device?\n\nSource: %1\nTarget: %2\nFilesystem: %3\n\nThe target device will be prepared by storage-monitor-agent.\nAdministrative authorization will be requested.\n\nContinue?").arg(sourceText, targetText, mountPoint), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    currentReplaceMount = mountPoint;
    currentReplaceState = QStringLiteral("starting");
    currentReplacePercent = -1;
    currentReplaceDetails = QStringLiteral("Waiting for authorization...");

    ui->lbl_Status_Replace->setText(
        QStringLiteral("Status: Starting..."));
    ui->lbl_Details_Replace->setText(currentReplaceDetails);
    ui->progress_Replace->setRange(0, 100);
    ui->progress_Replace->setValue(0);

    ui->btnStartReplace->setEnabled(false);
    ui->cmbReplaceSource->setEnabled(false);
    ui->cmbReplaceTarget->setEnabled(false);

    m_agentDbusClient->startReplace(mountPoint, sourceDeviceId, targetDevicePath);
}

void MainWindow::on_btnCancelReplace_clicked() {
    if (!m_agentDbusClient) { QMessageBox::critical(this, QStringLiteral("Device Replace"), QStringLiteral("Storage Monitor Agent is not available.")); return; }

    const QString mountPoint = ui->comboBoxMount->currentText().trimmed();
    if (mountPoint.isEmpty() || !isReplaceRunning || currentReplaceMount.trimmed() != mountPoint) { QMessageBox::information(this, QStringLiteral("Device Replace"), QStringLiteral("No device replacement is currently running.")); return; }

    const QMessageBox::StandardButton reply = QMessageBox::question(this, QStringLiteral("Cancel Device Replace"), QStringLiteral("Cancel the device replacement currently running on %1?\n\nAdministrative authorization will be requested.").arg(mountPoint), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    currentReplaceState = QStringLiteral("stopping");
    ui->lbl_Status_Replace->setText(QStringLiteral("Status: Cancelling..."));
    ui->lbl_Details_Replace->setText(QStringLiteral("Waiting for device replacement cancellation..."));
    ui->btnCancelReplace->setEnabled(false);

    m_agentDbusClient->cancelReplace(mountPoint);
}

// ============================================================
// PULSANTI BILANCIAMENTO DISCHI
// ===========================================================

void MainWindow::on_btnBalance_clicked()
{
    const QString mountPoint = ui->comboBoxMount->currentText().trimmed();

    if (mountPoint.isEmpty())
    {
        QMessageBox::warning(this, "Error", "No mount point selected!");
        return;
    }

    if (isBalanceRunning)
    {
        QMessageBox::information(this, "Balance Running", "A balance operation is already running.");
        return;
    }

    balanceBtrfs(mountPoint);
}

// ============================================================
// PULSANTI START SCRUB
// ===========================================================

void MainWindow::on_btnStartScrub_clicked()
{
    const QString mountPoint = ui->comboBoxMount->currentText().trimmed();

    if (mountPoint.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("Scrub"), QStringLiteral("No mount point selected."));
        return;
    }

    if (!m_agentDbusClient)
    {
        QMessageBox::critical(this, QStringLiteral("Scrub"), QStringLiteral("Storage Monitor Agent is not available."));
        return;
    }

    if (isScrubRunning)
    {
        QMessageBox::information(this, QStringLiteral("Scrub"), QStringLiteral("A scrub is already running."));
        return;
    }

    const QMessageBox::StandardButton reply = QMessageBox::question(this, QStringLiteral("Start Scrub"), QString("Start a Btrfs scrub on:\n%1\n\nThe operation can take several minutes and may affect storage performance.\n\nIt is managed by storage-monitor-agent and continues if this application is closed.\n\nAdministrative authorization will be requested.\n\nContinue?").arg(mountPoint), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    currentScrubMount = mountPoint;
    setScrubState(QStringLiteral("starting"), QStringLiteral("Waiting for authorization..."));
    m_agentDbusClient->startScrub(mountPoint);
}

//Risposte Start/Cancel
void MainWindow::onStartScrubFinished(const QString &mountPoint, bool accepted)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    if (accepted) { if (currentScrubState != QStringLiteral("running")) setScrubState(QStringLiteral("starting"), QStringLiteral("Scrub request accepted. Waiting for start...")); return; }

    if (m_agentDbusClient) m_agentDbusClient->requestScrubStatus(mountPoint);
}
//Risposte Start/Cancel
void MainWindow::onCancelScrubFinished(const QString &mountPoint, bool accepted)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    if (accepted) return;

    if (m_agentDbusClient) m_agentDbusClient->requestScrubStatus(mountPoint);
}

// ============================================================
// Stato D-Bus dello scrub
// ===========================================================
void MainWindow::onAgentScrubStateChanged(const QString &mountPoint, const QString &state, const QString &message)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    qInfo().noquote() << "Scrub D-Bus state:" << mountPoint << state;

    if (state == QStringLiteral("starting"))
    {
        currentScrubMount = mountPoint;
        setScrubState(QStringLiteral("starting"), message);
    }
    else if (state == QStringLiteral("running"))
    {
        currentScrubMount = mountPoint;
        setScrubState(QStringLiteral("running"), QStringLiteral("Scrub in progress..."));
    }
    else if (state == QStringLiteral("stopping"))
    {
        currentScrubMount = mountPoint;
        setScrubState(QStringLiteral("stopping"), message);
    }
    else if (state == QStringLiteral("finished"))
    {
        if (currentScrubMount == mountPoint) currentScrubMount.clear();
        setScrubState(QStringLiteral("finished"), message);
        ui->lbl_Scrub->setText(QStringLiteral("Scrub: finished"));
    }
    else if (state == QStringLiteral("cancelled"))
    {
        if (currentScrubMount == mountPoint) currentScrubMount.clear();
        setScrubState(QStringLiteral("cancelled"), message);
        ui->lbl_Scrub->setText(QStringLiteral("Scrub: cancelled"));
    }
    else if (state == QStringLiteral("interrupted"))
    {
        if (currentScrubMount == mountPoint)
            currentScrubMount.clear();

        setScrubState(QStringLiteral("interrupted"), message.isEmpty() ? QStringLiteral("Scrub interrupted") : message);

    }
    else if (state == QStringLiteral("error"))
    {
        if (currentScrubMount == mountPoint) currentScrubMount.clear();
        setScrubState(QStringLiteral("error"), message);
    }

    /*
    * Uno stato terminale può modificare lo schedule:
    *
    * finished:
    *   nuovo Last scrub e nuova scadenza normale.
     *
    * cancelled / interrupted / error:
    *   uno scrub automatico può avere impostato
    *   il retry/backoff persistente di 24 ore.
    *
    * Rileggiamo sempre il daemon come source of truth.
    */
    if ((state == QStringLiteral("finished") || state == QStringLiteral("cancelled") || state == QStringLiteral("interrupted") || state == QStringLiteral("error")) && m_agentDbusClient)
    {
        m_agentDbusClient->requestAutoScrubSchedule(mountPoint);
    }
}

// ============================================================
// Progresso D-Bus
// ===========================================================
void MainWindow::onAgentScrubProgressChanged(const QString &mountPoint, int completedPercent, qulonglong processedBytes, qulonglong totalBytes, qulonglong speedBytesPerSec, int errorCount)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    completedPercent = qBound(0, completedPercent, 100);

    isScrubRunning = true;
    currentScrubMount = mountPoint;
    currentScrubState = QStringLiteral("running");
    ui->lbl_Scrub->setText(QStringLiteral("Scrub: running"));

    ui->progress_Scrub->setVisible(true);
    ui->progress_Scrub->setRange(0, 100);
    ui->progress_Scrub->setValue(completedPercent);
    ui->progress_Scrub->setFormat(QStringLiteral("%1%").arg(completedPercent));

    QString status = QStringLiteral("Scrub: %1 / %2 — %3/s").arg(formatBytes(processedBytes), formatBytes(totalBytes), formatBytes(speedBytesPerSec));

    if (errorCount > 0) status += QStringLiteral(" — errors: %1").arg(errorCount);

    ui->lbl_Status_Scrub->setText(status);
    ui->grp_Scrub->setTitle(QStringLiteral("Scrub Running"));
    ui->btnStartScrub->setEnabled(false);
    ui->btnStopScrub->setEnabled(true);

    qInfo().noquote() << "Scrub GUI progress:" << mountPoint << completedPercent << "%" << "processed:" << processedBytes << "total:" << totalBytes << "speed:" << speedBytesPerSec << "errors:" << errorCount;
}

// ============================================================
// Risposta ScrubStatus
// ===========================================================
void MainWindow::onScrubStatusReceived(const QString &mountPoint, const QString &state)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    if (state == QStringLiteral("running")) { currentScrubMount = mountPoint; if (currentScrubState != QStringLiteral("running")) setScrubState(QStringLiteral("running"), QStringLiteral("Scrub is running")); }
    else if (state == QStringLiteral("finished")) { currentScrubMount.clear(); setScrubState(QStringLiteral("finished"), QStringLiteral("Last scrub completed")); }
    else if (state == QStringLiteral("cancelled")) { currentScrubMount.clear(); setScrubState(QStringLiteral("cancelled"), QStringLiteral("Last scrub was cancelled")); }
    else if (state == QStringLiteral("interrupted")) { currentScrubMount.clear(); setScrubState(QStringLiteral("interrupted"), QStringLiteral("Last scrub was interrupted")); }
    else if (state == QStringLiteral("not running")) { currentScrubMount.clear(); setScrubState(QStringLiteral("ready"), QStringLiteral("Ready")); }
    else if (state == QStringLiteral("error")) { currentScrubMount.clear(); setScrubState(QStringLiteral("error"), QStringLiteral("Unable to determine scrub status")); }
}

void MainWindow::onAutoScrubConfigReceived(const QString &mountPoint, bool enabled, int intervalWeeks)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    ui->chkAutoScrub->setChecked(enabled);

    const int index = ui->cmbIntervalScrub->findData(intervalWeeks);
    if (index >= 0) ui->cmbIntervalScrub->setCurrentIndex(index);

    ui->chkAutoScrub->setEnabled(true);
    ui->cmbIntervalScrub->setEnabled(true);

    qInfo().noquote() << "Automatic scrub configuration received:" << mountPoint << "enabled:" << enabled << "interval:" << intervalWeeks << "week(s)";
}

void MainWindow::onAutoScrubScheduleReceived(const QString &mountPoint, qint64 lastSuccessfulEpoch, qint64 nextDueEpoch)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    // LAST SUCCESSFUL SCRUB
    //ui->lblLastScrub->setText(lastSuccessfulEpoch > 0 ? QStringLiteral("Last scrub: %1").arg(QDateTime::fromSecsSinceEpoch(lastSuccessfulEpoch, Qt::UTC).toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss"))) : QStringLiteral("Last scrub: -"));
    ui->lblLastScrub->setText(lastSuccessfulEpoch > 0 ? QStringLiteral("Last scrub: %1").arg(QDateTime::fromSecsSinceEpoch(lastSuccessfulEpoch, QTimeZone::UTC).toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss"))) : QStringLiteral("Last scrub: -"));

    // NEXT AUTOMATIC SCRUB
    //ui->lblNextScrub->setText(nextDueEpoch > 0 ? QStringLiteral("Next scrub: %1").arg(QDateTime::fromSecsSinceEpoch(nextDueEpoch, Qt::UTC).toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss"))) : QStringLiteral("Next scrub: -"));
    ui->lblNextScrub->setText(nextDueEpoch > 0 ? QStringLiteral("Next scrub: %1").arg(QDateTime::fromSecsSinceEpoch(nextDueEpoch, QTimeZone::UTC).toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm:ss"))) : QStringLiteral("Next scrub: -"));

    qInfo().noquote() << "Automatic scrub schedule received:" << mountPoint << "last:" << lastSuccessfulEpoch << "next:" << nextDueEpoch;
}

void MainWindow::onSetAutoScrubConfigFinished(const QString &mountPoint, bool accepted)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    if (!m_agentDbusClient) return;

    if (!accepted) QMessageBox::warning(this, QStringLiteral("Automatic Scrub"), QStringLiteral("The automatic scrub configuration was not accepted."));

    /* Non ci fidiamo semplicemente dei valori presenti nella GUI: rileggiamo ciò che il daemon ha realmente salvato nel database. */
    m_agentDbusClient->requestAutoScrubConfig(mountPoint);
    m_agentDbusClient->requestAutoScrubSchedule(mountPoint);
}

void MainWindow::on_btnStopScrub_clicked()
{
    const QString mountPoint = ui->comboBoxMount->currentText().trimmed();

    if (mountPoint.isEmpty()) return;
    if (!m_agentDbusClient) return;

    const QMessageBox::StandardButton reply = QMessageBox::question(this, QStringLiteral("Stop Scrub"), QStringLiteral("Are you sure you want to cancel the running scrub?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    setScrubState(QStringLiteral("stopping"), QStringLiteral("Waiting for authorization..."));
    m_agentDbusClient->cancelScrub(mountPoint);
}

void MainWindow::on_btnCheckBalanceStatus_clicked()
{
    const QString mountPoint =
        ui->comboBoxMount
            ->currentText()
            .trimmed();

    if (mountPoint.isEmpty())
    {
        QMessageBox::warning(
            this,
            "Error",
            "No mount point selected!");

        return;
    }

    if (!m_agentDbusClient)
    {
        QMessageBox::critical(
            this,
            "Service Error",
            "Storage Monitor Agent D-Bus client is not available.");

        return;
    }

    m_balanceStatusDialogPending =
        true;

    m_agentDbusClient
        ->requestBalanceStatus(
            mountPoint);
}

void MainWindow::on_btnStopBalance_clicked()
{
    const QString mountPoint =
        ui->comboBoxMount
            ->currentText()
            .trimmed();

    if (mountPoint.isEmpty())
    {
        QMessageBox::warning(
            this,
            "Error",
            "No mount point selected!");

        return;
    }

    if (!m_agentDbusClient)
    {
        QMessageBox::critical(
            this,
            "Service Error",
            "Storage Monitor Agent D-Bus client is not available.");

        return;
    }

    const QMessageBox::StandardButton reply =
        QMessageBox::question(
            this,
            "Stop Balance",
            "Are you sure you want to stop "
            "the balance operation?",
            QMessageBox::Yes |
                QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    setBalanceState(QStringLiteral("stopping"), QStringLiteral("Waiting for authorization..."));

    m_agentDbusClient->cancelBalance(mountPoint);
}

void MainWindow::onAgentBalanceStateChanged(const QString &mountPoint, const QString &state, const QString &message)
{
    /*
     * Un segnale relativo a un altro filesystem
     * non deve cambiare i controlli del mount
     * attualmente visualizzato.
     */
    if (ui->comboBoxMount->currentText() != mountPoint)
    {
        return;
    }

    qInfo().noquote() << "Balance D-Bus state:" << mountPoint << state << message;

    if (state == QStringLiteral("starting"))
    {
        isBalanceRunning = true;

        currentBalanceMount = mountPoint;

        setBalanceState(QStringLiteral("starting"), message);
    }
    else if (state == QStringLiteral("running"))
    {
        isBalanceRunning = true;

        currentBalanceMount = mountPoint;

        /*
         * Il daemon aggiorna il DB prima di
         * emettere "running".
         */
        refreshBalanceDataFromDatabase(mountPoint);

        setBalanceState(QStringLiteral("running"), message);
    }
    else if (state == QStringLiteral("paused"))
    {
        isBalanceRunning = true;

        currentBalanceMount = mountPoint;

        setBalanceState(QStringLiteral("paused"), message);
    }
    else if (state == QStringLiteral("stopping"))
    {
        isBalanceRunning = true;

        currentBalanceMount = mountPoint;

        setBalanceState(
            QStringLiteral("stopping"), message);
    }
    else if (state == QStringLiteral("completed"))
    {
        isBalanceRunning = false;

        if (currentBalanceMount ==
            mountPoint)
        {
            currentBalanceMount.clear();
        }

        /*
         * Testato nel daemon:
         * SQLite è già aggiornato prima
         * del segnale completed.
         */
        refreshBalanceDataFromDatabase(
            mountPoint);

        setBalanceState(
            QStringLiteral("completed"),
            message);
    }
    else if (state == QStringLiteral("stopped"))
    {
        isBalanceRunning = false;

        if (currentBalanceMount ==
            mountPoint)
        {
            currentBalanceMount.clear();
        }

        refreshBalanceDataFromDatabase(
            mountPoint);

        setBalanceState(
            QStringLiteral("stopped"),
            message);
    }
    else if (state ==
             QStringLiteral("error"))
    {
        isBalanceRunning = false;

        if (currentBalanceMount ==
            mountPoint)
        {
            currentBalanceMount.clear();
        }

        setBalanceState(
            QStringLiteral("error"),
            message);
    }
}

void MainWindow::onAgentBalanceProgressChanged(const QString &mountPoint, int completedPercent, int leftPercent)
{
    /*
     * Ignoriamo segnali appartenenti a un altro
     * filesystem rispetto a quello visualizzato.
     */
    if (ui->comboBoxMount->currentText() != mountPoint)
        return;

    completedPercent = qBound(0, completedPercent, 100);

    leftPercent = qBound(0, leftPercent, 100);

    isBalanceRunning = true;
    currentBalanceMount = mountPoint;
    currentBalanceState =
        QStringLiteral("running");

    ui->progress_Bilaciamento->setVisible(true);
    ui->progress_Bilaciamento->setRange(0, 100);
    ui->progress_Bilaciamento->setValue(
        completedPercent);

    ui->progress_Bilaciamento->setFormat(QStringLiteral("%1%").arg(completedPercent));

    ui->lbl_Manu_Balance->setText(QStringLiteral("Balance in progress: %1% — %2% left").arg(completedPercent).arg(leftPercent));

    ui->grp_Bilanciamento->setTitle(QStringLiteral("Balance Running"));

    ui->btnBalance->setEnabled(false);
    ui->btnStopBalance->setEnabled(true);

    qInfo().noquote() << "Balance D-Bus progress:" << mountPoint << completedPercent << "% completed," << leftPercent << "% left";
}

void MainWindow::onBalanceStatusReceived(const QString &mountPoint, const QString &state)
{
    if (ui->comboBoxMount->currentText() == mountPoint)
    {
        if (state == QStringLiteral("running"))
        {
            isBalanceRunning = true;

            currentBalanceMount = mountPoint;

     /*Se BalanceProgressChanged è già arrivato, currentBalanceState è già "running" e
     * non dobbiamo riportare la progress bar in modalità indeterminata. */
            if (currentBalanceState != QStringLiteral("running"))
            {
                setBalanceState(QStringLiteral("running"), QStringLiteral("Balance is running"));
            }
        }
        else if (state == QStringLiteral("paused"))
        {
            isBalanceRunning = true;

            currentBalanceMount = mountPoint;

            setBalanceState(QStringLiteral("paused"), QStringLiteral("Balance is paused"));
        }
        else if (state ==
                 QStringLiteral("idle"))
        {
            isBalanceRunning = false;

            if (currentBalanceMount == mountPoint)
            {
                currentBalanceMount.clear();
            }

            /*
             * Non cancelliamo immediatamente
             * "completed" o "stopped", così
             * l'utente può vedere l'esito.
             */
            if (currentBalanceState != QStringLiteral("completed") && currentBalanceState != QStringLiteral("stopped") && currentBalanceState != QStringLiteral("ready"))
            {
                setBalanceState(QStringLiteral("ready"), QStringLiteral("Ready"));
            }
        }
        else if (state ==QStringLiteral("error"))

        {
            setBalanceState(QStringLiteral("error"), QStringLiteral("Unable to determine balance status"));
        }
    }

    if (m_balanceStatusDialogPending)
    {
        m_balanceStatusDialogPending =
            false;

        QMessageBox::information(
            this,
            "Balance Status",
            QString(
                "Balance status for %1:\n\n%2")
                .arg(
                    mountPoint,
                    state));
    }
}


void MainWindow::onStartBalanceFinished(const QString &mountPoint, bool accepted)
{
    if (ui->comboBoxMount->currentText() !=
        mountPoint)
    {
        return;
    }

    if (accepted)
    {
        if (currentBalanceState !=
            QStringLiteral("running"))
        {
            setBalanceState(
                QStringLiteral("starting"),
                QStringLiteral(
                    "Balance request accepted. "
                    "Waiting for start..."));
        }

        return;
    }

    /*
     * false può significare anche che il daemon
     * ha rilevato un balance già in corso.
     * Richiediamo lo stato reale anziché
     * inventare un errore.
     */
    if (m_agentDbusClient)
    {
        m_agentDbusClient
            ->requestBalanceStatus(
                mountPoint);
    }
}

void MainWindow::onCancelBalanceFinished(const QString &mountPoint, bool accepted)
{
    if (ui->comboBoxMount->currentText() !=
        mountPoint)
    {
        return;
    }

    if (accepted)
    {
        /*
         * Il segnale "stopped" arriverà
         * quando il daemon avrà terminato
         * realmente la cancellazione.
         */
        return;
    }

    if (m_agentDbusClient)
    {
        m_agentDbusClient
            ->requestBalanceStatus(
                mountPoint);
    }
}

void MainWindow::onAddDeviceFinished(const QString &mountPoint, const QString &devicePath, bool accepted)
{
    Q_UNUSED(devicePath);

    const QString deviceName = m_pendingAddDeviceName;
    m_addDevicePending = false;
    m_pendingAddDeviceName.clear();

    ui->btnAddToBtrfs->setEnabled(true);

    if (!accepted) {
        QMessageBox::critical(this, "Add Device", QString("The Storage Monitor Agent rejected the request to add %1.").arg(deviceName.isEmpty() ? QStringLiteral("the device") : deviceName));
        return;
    }

    QMessageBox::information(this, "Success", QString("Device %1 successfully added to the Btrfs filesystem!\n\nMount point: %2").arg(deviceName, mountPoint));

    /* Il daemon ha già eseguito refreshMonitor() prima di restituire true. */
    onMountPointChanged(mountPoint);
    ui->btnAddToBtrfs->setVisible(false);
    isDiskSelected = false;

    const QMessageBox::StandardButton balanceReply = QMessageBox::question(this, "Balance Filesystem", QString("Device %1 successfully added!\n\nDo you want to run a balance operation now?\nThis will redistribute data across all devices.\n\nThis may take several minutes.").arg(deviceName), QMessageBox::Yes | QMessageBox::No);

    if (balanceReply == QMessageBox::Yes) {
        ui->tab_btrfs->setCurrentIndex(ui->tab_btrfs->indexOf(ui->tab_management));
        balanceBtrfs(mountPoint);
    }
}

void MainWindow::onRemoveDeviceFinished(const QString &mountPoint, qulonglong deviceId, bool accepted)
{
    qDebug().noquote() << "RemoveDevice finished:" << "mount:" << mountPoint << "devid:" << deviceId << "accepted:" << accepted;

    if (!accepted)
    {
        if (isRemovalActive(mountPoint, deviceId))
            m_activeRemovals.remove(mountPoint.trimmed());

        if (m_removalHideTimer)
            m_removalHideTimer->stop();

        ui->grp_Removal->setEnabled(false);
        ui->progress_Removal->setValue(0);
        ui->lbl_Info_Removal->clear();
        ui->lbl_Remainig_Removal->clear();

        ui->btnAddToBtrfs->setEnabled(true);

        ui->btnAddToBtrfs->setText(QStringLiteral("Remove device %1").arg(deviceId));

        QMessageBox::critical(this, QStringLiteral("Remove Device"), QStringLiteral("The Storage Monitor Agent rejected the request to remove Btrfs device %1.").arg(deviceId));

        return;
    }

    ui->btnAddToBtrfs->setEnabled(false);

    ui->btnAddToBtrfs->setText(QStringLiteral("Removing device %1...").arg(deviceId));

    if (m_removalHideTimer)
        m_removalHideTimer->stop();

    ui->grp_Removal->setEnabled(true);

    ui->progress_Removal->setValue(0);

    ui->lbl_Info_Removal->setText(QStringLiteral("Removing device %1 from %2").arg(deviceId).arg(mountPoint));

    ui->lbl_Remainig_Removal->setText(QStringLiteral("Waiting for progress..."));

    ui->stackedWidget->setCurrentWidget(ui->pageInfo);

    ui->tab_btrfs->setCurrentIndex(ui->tab_btrfs->indexOf(ui->tab_management));

    m_activeRemovals.insert(mountPoint.trimmed(), deviceId);

}

void MainWindow::onAgentDbusError(const QString &operation, const QString &mountPoint, const QString &message)
{
    qWarning().noquote() << "Agent D-Bus error:" << operation << mountPoint << message;

    if (operation == QStringLiteral("StartReplace")) {
        if (ui->comboBoxMount->currentText().trimmed() == mountPoint.trimmed()) {
            isReplaceRunning = false;
            currentReplaceMount.clear();
            currentReplaceState = QStringLiteral("error");
            currentReplacePercent = -1;
            currentReplaceDetails = message;
            ui->btnStartReplace->setEnabled(true);
            ui->cmbReplaceSource->setEnabled(true);
            ui->cmbReplaceTarget->setEnabled(true);
            ui->lbl_Status_Replace->setText(QStringLiteral("Status: Error"));
            ui->lbl_Details_Replace->setText(message);
        }
        QMessageBox::warning(this, QStringLiteral("Device Replace"), QStringLiteral("Cannot start device replacement:\n\n%1").arg(message));
        return;
    }

    if (operation == QStringLiteral("CancelReplace"))
    {
        if (ui->comboBoxMount->currentText().trimmed() == mountPoint.trimmed())
        {
            ui->lbl_Status_Replace->setText(QStringLiteral("Status: Running"));

            ui->lbl_Details_Replace->setText(QStringLiteral("Device replace cancellation was not accepted."));

            if (m_agentDbusClient)
            {
                m_agentDbusClient->requestReplaceStatus(mountPoint);
            }
        }

        QMessageBox::warning(this, QStringLiteral("Device Replace"), QStringLiteral("Cannot cancel device replacement:\n\n%1").arg(message));
        return;
    }

    if (operation == QStringLiteral("ReplaceStatus"))
    {
        if (ui->comboBoxMount->currentText().trimmed() == mountPoint.trimmed())
        {
            isReplaceRunning = false;
            currentReplaceMount.clear();
            currentReplaceState = QStringLiteral("error");
            currentReplacePercent = -1;
            currentReplaceDetails = message;
        }

        return;
    }

    if (operation == QStringLiteral("AddDevice")) {
        m_addDevicePending = false;
        m_pendingAddDeviceName.clear();
        ui->btnAddToBtrfs->setEnabled(true);
        QMessageBox::warning(this, "Add Device", QString("Cannot add the device:\n\n%1").arg(message));
        return;
    }

    if (operation == QStringLiteral("RemoveDevice"))
    {
        const QString normalizedMount = mountPoint.trimmed();

        // La richiesta non è partita correttamente:
        // non deve rimanere registrata come attiva.
        m_activeRemovals.remove(normalizedMount);

        if (m_removalHideTimer)
            m_removalHideTimer->stop();

        // Eventuale pannello di rimozione lasciato aperto
        // non rappresenta più un'operazione attiva.
        ui->grp_Removal->setEnabled(false);
        ui->progress_Removal->setValue(0);
        ui->lbl_Info_Removal->clear();
        ui->lbl_Remainig_Removal->clear();

        ui->btnAddToBtrfs->setEnabled(true);

        const QString deviceId = currentSelectedDisk.btrfsDeviceId.trimmed();

        if (currentSelectedDisk.status == QStringLiteral("missing") &&
            currentSelectedDisk.filesystem == QStringLiteral("btrfs") &&
            currentSelectedDisk.mountPoint.trimmed() == normalizedMount &&
            !deviceId.isEmpty())
        {
            ui->btnAddToBtrfs->setText(QStringLiteral("Remove device %1").arg(deviceId));
        }
        else
        {
            ui->btnAddToBtrfs->setText(QStringLiteral("Remove device"));
        }

        QMessageBox::warning(this, QStringLiteral("Remove Device"), QStringLiteral("Cannot start the device removal:\n\n%1").arg(message));

        return;
    }

    if (operation == QStringLiteral("AutoScrubConfig") || operation == QStringLiteral("AutoScrubSchedule"))
    {
        return;
    }

    if (operation == QStringLiteral("SetAutoScrubConfig"))
    {
        if (ui->comboBoxMount->currentText().trimmed() == mountPoint.trimmed())
        {
            /*
         * Ripristiniamo i valori realmente salvati,
         * ad esempio dopo un rifiuto Polkit.
         */
            if (m_agentDbusClient)
            {
                m_agentDbusClient->requestAutoScrubConfig(mountPoint);
            }

            QMessageBox::warning(this, QStringLiteral("Automatic Scrub"), QStringLiteral("Cannot save the automatic scrub configuration:\n\n%1").arg(message));
        }

        return;
    }

    if (operation == QStringLiteral("ScrubStatus")) { if (ui->comboBoxMount->currentText().trimmed() == mountPoint.trimmed()) setScrubState(QStringLiteral("error"), QStringLiteral("Unable to read scrub status")); return; }

    if (ui->comboBoxMount->currentText().trimmed() == mountPoint.trimmed()) {
        if (operation == QStringLiteral("StartScrub")) { isScrubRunning = false; currentScrubMount.clear(); setScrubState(QStringLiteral("error"), message); }
        else if (operation == QStringLiteral("CancelScrub")) { setScrubState(QStringLiteral("running"), QStringLiteral("Scrub cancellation was not accepted")); if (m_agentDbusClient) m_agentDbusClient->requestScrubStatus(mountPoint); }
    }

    /* BalanceStatus viene chiamato anche automaticamente al cambio mount. In quel caso non vogliamo popup continui. */
    if (operation == QStringLiteral("BalanceStatus")) {
        if (m_balanceStatusDialogPending) {
            m_balanceStatusDialogPending = false;
            QMessageBox::warning(this, "Balance Status", QString("Cannot read balance status:\n\n%1").arg(message));
        }
        return;
    }

    if (ui->comboBoxMount->currentText() == mountPoint) {
        if (operation == QStringLiteral("StartBalance")) {
            isBalanceRunning = false;
            currentBalanceMount.clear();
            setBalanceState(QStringLiteral("error"), message);
        } else if (operation == QStringLiteral("CancelBalance")) {
            /* Il cancel non è stato autorizzato o non ha raggiunto il daemon: il balance potrebbe essere ancora in esecuzione. */
            setBalanceState(QStringLiteral("running"), QStringLiteral("Balance cancellation was not accepted"));
            if (m_agentDbusClient) m_agentDbusClient->requestBalanceStatus(mountPoint);
        }
    }

    QMessageBox::warning(this, "Storage Monitor Agent", QString("%1 failed:\n\n%2").arg(operation, message));
}

void MainWindow::onStartReplaceFinished(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath, bool accepted)
{
    qInfo().noquote() << "StartReplace finished:" << "mount:" << mountPoint << "source devid:" << sourceDeviceId << "target:" << targetDevicePath << "accepted:" << accepted;

    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    if (accepted) {
        isReplaceRunning = true;
        currentReplaceMount = mountPoint;
        currentReplaceState = QStringLiteral("starting");
        currentReplacePercent = -1;
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Starting..."));
        ui->lbl_Details_Replace->setText(QStringLiteral("Device replace request accepted. Waiting for progress..."));
        ui->btnCancelReplace->setEnabled(false);
        return;
    }
    else
    {
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Ready"));
        ui->lbl_Details_Replace->setText(QStringLiteral("Device replace request was rejected."));
    }


    if (m_agentDbusClient) m_agentDbusClient->requestReplaceStatus(mountPoint);
}

void MainWindow::onCancelReplaceFinished(const QString &mountPoint, bool accepted)
{
    qInfo().noquote() << "CancelReplace finished:" << "mount:" << mountPoint << "accepted:" << accepted;

    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    /* Non dichiariamo ancora cancelled: aspettiamo ReplaceStateChanged dal daemon. */
    if (accepted) return;

    if (m_agentDbusClient) m_agentDbusClient->requestReplaceStatus(mountPoint);
}

void MainWindow::onReplaceStatusReceived(const QString &mountPoint, const QString &state, int completedPercent, const QString &details) {

    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) return;

    const QString normalizedState = state.trimmed().toLower();

    currentReplaceMount = mountPoint;
    currentReplaceState = normalizedState;
    currentReplacePercent = completedPercent;
    currentReplaceDetails = details;

    isReplaceRunning = normalizedState == QStringLiteral("starting") || normalizedState == QStringLiteral("running") || normalizedState == QStringLiteral("stopping");

    ui->progress_Replace->setRange(0, 100);

    if (completedPercent >= 0) { ui->progress_Replace->setValue(qBound(0, completedPercent, 100)); }
    else { ui->progress_Replace->setValue(0); }

    if (normalizedState == QStringLiteral("running"))
    {
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Running"));
        ui->lbl_Details_Replace->setText(details);
        ui->btnStartReplace->setEnabled(false);
        ui->cmbReplaceSource->setEnabled(false);
        ui->cmbReplaceTarget->setEnabled(false);
        ui->btnCancelReplace->setEnabled(true);
    }
    else if (normalizedState == QStringLiteral("finished"))
    {
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Finished"));
        ui->lbl_Details_Replace->setText(details);
        currentReplacePercent = 100;
        ui->progress_Replace->setValue(100);
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->btnCancelReplace->setEnabled(false);
    }
    else if (normalizedState == QStringLiteral("cancelled"))
    {
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Cancelled"));
        ui->lbl_Details_Replace->setText(details);
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->btnCancelReplace->setEnabled(false);
    }
    else if (normalizedState == QStringLiteral("idle"))
    {
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Ready"));
        ui->lbl_Details_Replace->clear();
        ui->progress_Replace->setValue(0);
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->btnCancelReplace->setEnabled(false);
    }
    else if (normalizedState == QStringLiteral("error"))
    {
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Error"));
        ui->lbl_Details_Replace->setText(details);
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->btnCancelReplace->setEnabled(false);
    }

    if (!isReplaceRunning && (normalizedState == QStringLiteral("idle") || normalizedState == QStringLiteral("finished") || normalizedState == QStringLiteral("cancelled") || normalizedState == QStringLiteral("error"))) currentReplaceMount.clear();

    qInfo().noquote() << "ReplaceStatus GUI:" << mountPoint << "state:" << normalizedState << "progress:" << completedPercent << "details:" << details;
}

void MainWindow::onAgentReplaceStateChanged(const QString &mountPoint, const QString &state, const QString &message)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) { return; }

    const QString normalizedState = state.trimmed().toLower();
    currentReplaceState = normalizedState;
    currentReplaceDetails = message;

    qInfo().noquote() << "Replace D-Bus state:" << mountPoint << normalizedState << message;

    if (normalizedState == QStringLiteral("starting"))
    {
        isReplaceRunning = true;
        currentReplaceMount = mountPoint;
        ui->btnStartReplace->setEnabled(false);
        ui->cmbReplaceSource->setEnabled(false);
        ui->cmbReplaceTarget->setEnabled(false);
        ui->btnCancelReplace->setEnabled(false);
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Starting..."));
        ui->lbl_Details_Replace->setText(message);
        return;
    }

    if (normalizedState == QStringLiteral("running"))
    {
        isReplaceRunning = true;
        currentReplaceMount = mountPoint;
        ui->btnStartReplace->setEnabled(false);
        ui->cmbReplaceSource->setEnabled(false);
        ui->cmbReplaceTarget->setEnabled(false);
        ui->btnCancelReplace->setEnabled(true);
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Running"));
        ui->lbl_Details_Replace->setText(message);
        return;
    }

    if (normalizedState == QStringLiteral("stopping"))
    {
        isReplaceRunning = true;
        currentReplaceMount = mountPoint;
        ui->btnStartReplace->setEnabled(false);
        ui->cmbReplaceSource->setEnabled(false);
        ui->cmbReplaceTarget->setEnabled(false);
        ui->btnCancelReplace->setEnabled(false);
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Cancelling..."));
        ui->lbl_Details_Replace->setText(message);
        return;
    }

    if (normalizedState == QStringLiteral("finished"))
    {
        isReplaceRunning = false;
        currentReplaceMount.clear();
        currentReplacePercent = 100;
        ui->progress_Replace->setRange(0, 100);
        ui->progress_Replace->setValue(100);
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->btnCancelReplace->setEnabled(false);
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Finished"));
        ui->lbl_Details_Replace->setText(message);

        // Il replace ha modificato i membri Btrfs: ricostruiamo Source e Target.
        QTimer::singleShot(500, this, [this, mountPoint]() { if (ui->comboBoxMount->currentText().trimmed() == mountPoint.trimmed()) { onMountPointChanged(mountPoint); } });
        return;
    }

    if (normalizedState == QStringLiteral("cancelled"))
    {
        isReplaceRunning = false;
        currentReplaceMount.clear();
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->btnCancelReplace->setEnabled(false);
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Cancelled"));
        ui->lbl_Details_Replace->setText(message);
        return;
    }

    if (normalizedState == QStringLiteral("error"))
    {
        isReplaceRunning = false;
        currentReplaceMount.clear();
        ui->btnStartReplace->setEnabled(true);
        ui->cmbReplaceSource->setEnabled(true);
        ui->cmbReplaceTarget->setEnabled(true);
        ui->btnCancelReplace->setEnabled(false);
        ui->lbl_Status_Replace->setText(QStringLiteral("Status: Error"));
        ui->lbl_Details_Replace->setText(message);
    }
}

void MainWindow::onAgentReplaceProgressChanged(const QString &mountPoint, int completedPercent)
{
    if (ui->comboBoxMount->currentText().trimmed() != mountPoint.trimmed()) { return; }

    currentReplaceMount = mountPoint;
    currentReplaceState = QStringLiteral("running");
    currentReplacePercent = qBound(0, completedPercent, 100);
    isReplaceRunning = true;

    ui->progress_Replace->setRange(0, 100);
    ui->progress_Replace->setValue(currentReplacePercent);
    ui->lbl_Status_Replace->setText(QStringLiteral("Status: Running"));
    ui->lbl_Details_Replace->setText(QStringLiteral("%1% completed").arg(currentReplacePercent));

    ui->btnStartReplace->setEnabled(false);
    ui->cmbReplaceSource->setEnabled(false);
    ui->cmbReplaceTarget->setEnabled(false);
    ui->btnCancelReplace->setEnabled(true);

    qInfo().noquote() << "Replace GUI progress:" << mountPoint << currentReplacePercent << "%";
}

void MainWindow::onBtrfsHealthChanged(const QString &fsUuid, BtrfsMonitor::HealthState state, const QString &reason)
{
    // ============================================================
    // AGGIORNA IL BANNER DEL FILESYSTEM VISUALIZZATO
    // ============================================================

    if (m_btrfsMonitor)
    {
        const QString currentMountPoint = ui->comboBoxMount->currentText();

        const QList<BtrfsMonitor::FilesystemState>
            filesystems = m_btrfsMonitor->filesystems();

        for (const BtrfsMonitor::FilesystemState &filesystem : filesystems)
        {
            if (filesystem.fsUuid == fsUuid && filesystem.mountPoint == currentMountPoint)
            {
                BtrfsInfo info = updateBtrfsInformation(currentMountPoint);

                populateDeviceSummary(info, currentMountPoint);

                updateBtrfsOverview(info);
                updateFilesystemHealth(info);

                break;
            }
        }
    }

    // ============================================================
    // STATUS BAR
    // ============================================================
    QString statusText;
    QString style;

    switch (state) {
    case BtrfsMonitor::HealthState::Ok:
        statusText = "✅ Btrfs: Healthy";
        style = "color: green;";
        break;
    case BtrfsMonitor::HealthState::Warning:
        statusText = "⚠️ Btrfs: Warning";
        style = "color: orange;";
        break;
    case BtrfsMonitor::HealthState::Error:
        statusText = "❌ Btrfs: Error";
        style = "color: red; font-weight: bold;";
        break;
    default:
        statusText = "ℹ️ Btrfs: Unknown";
        style = "color: gray;";
    }

    ui->statusBar->setStyleSheet(style);
    ui->statusBar->showMessage(statusText + " - " + reason, 3000);

    QTimer::singleShot(3100, this, [this]()
                       {
                           if (ui->statusBar->currentMessage().isEmpty())
                               ui->statusBar->showMessage(QStringLiteral("Ready"));
                       });

    // Notifica popup per errori critici
    if (state == BtrfsMonitor::HealthState::Error) {
        QMessageBox::critical(this, "⚠️ Btrfs Error",
                              "Filesystem health error detected:\n\n" + reason,
                              QMessageBox::Ok);
    }

}

bool MainWindow::openMonitorDatabase()
{
    static const QString connectionName = QStringLiteral("btrfs_storage_readonly");

    if (QSqlDatabase::contains(connectionName))
    {
        m_monitorDb = QSqlDatabase::database(connectionName);
    }
    else
    {
        m_monitorDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);

        m_monitorDb.setDatabaseName(QStringLiteral("/var/lib/storage-monitor-agent/btrfs.db"));

        m_monitorDb.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    }

    if (!m_monitorDb.open())
    {
        qWarning().noquote() << "Cannot open storage-monitor-agent database:" << m_monitorDb.lastError().text();
        return false;
    }

    /*
     * Seconda protezione oltre all'apertura READ ONLY.
     */
    QSqlQuery query(m_monitorDb);

    if (!query.exec(
            QStringLiteral("PRAGMA query_only = ON")))
    {
        qWarning().noquote() << "Cannot enable SQLite query_only:"  << query.lastError().text();
        return false;
    }

    qInfo().noquote() << "Opened monitor database read-only:" << m_monitorDb.databaseName();
    return true;
}

void MainWindow::onRemoveDeviceProgress(const QString &mountPoint, qulonglong deviceId, int completedPercent, qulonglong remainingBytes)
{
    const int percent = qBound(0, completedPercent, 100);
    const bool selectedSameDevice = isDiskSelected && currentSelectedDisk.status == QStringLiteral("missing") && currentSelectedDisk.filesystem == QStringLiteral("btrfs") && currentSelectedDisk.mountPoint.trimmed() == mountPoint.trimmed() && currentSelectedDisk.btrfsDeviceId.trimmed() == QString::number(deviceId);

    if (percent < 100) {

        if (m_removalHideTimer)
            m_removalHideTimer->stop();

        m_activeRemovals.insert(mountPoint.trimmed(), deviceId); // Fondamentale per ricostruire lo stato dopo riavvio GUI
        if (selectedSameDevice) { ui->btnAddToBtrfs->setEnabled(false); ui->btnAddToBtrfs->setText(QStringLiteral("Removing device %1...").arg(deviceId)); }
    } else {
        if (isRemovalActive(mountPoint, deviceId)) m_activeRemovals.remove(mountPoint.trimmed());
    }

    ui->grp_Removal->setVisible(true);
    ui->progress_Removal->setValue(percent);
    ui->lbl_Remainig_Removal->setText(QStringLiteral("Remaining: %1").arg(formatBytes(remainingBytes)));

    if (percent >= 100) {
        ui->lbl_Info_Removal->setText(QStringLiteral("Device %1 removed from %2").arg(deviceId).arg(mountPoint));

        ui->btnAddToBtrfs->setEnabled(true);

        if (m_removalHideTimer)
            m_removalHideTimer->start();
    } else {
        ui->lbl_Info_Removal->setText(QStringLiteral("Removing device %1 from %2").arg(deviceId).arg(mountPoint));
    }

    qDebug().noquote() << "RemoveDevice GUI progress:" << "mount:" << mountPoint << "devid:" << deviceId << "completed:" << completedPercent << "%" << "remaining:" << formatBytes(remainingBytes);
}
