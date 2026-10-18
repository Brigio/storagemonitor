// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QComboBox>
#include <QVBoxLayout>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QHash>
#include <QMap>
#include <QList>
#include <QProgressBar>
#include <QLabel>
#include <QSqlDatabase>
#include <QTimer>

#include "btrfs_event_widget.h"
#include "btrfsmonitor.h"
#include "btrfseventmanager.h"
#include "diskrowwidget.h"
#include "diskinfo.h"

class AgentDbusClient;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    QString getCurrentMountPoint();

    enum class HealthState
    {
        Unknown,
        Healthy,
        Warning,
        Failed
    };

    struct BtrfsDeviceStats
    {
        quint64 readErrors = 0;
        quint64 writeErrors = 0;
        quint64 flushErrors = 0;
        quint64 corruptionErrors = 0;
        quint64 generationErrors = 0;
    };

    struct BtrfsInfo
    {
        QString mountPoint;

        int deviceCount = 0;
        int healthyDevices = 0;
        int warningDevices = 0;
        int failedDevices = 0;

        QString profile;
        QString dataProfile;
        QString metadataProfile;

        quint64 totalBytes = 0;
        quint64 usedBytes = 0;
        quint64 freeBytes = 0;

        QString scrubStatus;
        QString balanceStatus;

        quint64 errorCount = 0;

        HealthState health = HealthState::Warning;
    };

    struct DiskHierarchy
    {
        QJsonObject disk;
        QList<QJsonObject> partitions;
        QStringList btrfsPartitions;
    };

private slots:
    void onMountPointChanged(const QString &mountPoint);
    void onDiskSelected(diskrowwidget *clickedWidget);

    void on_btn_Status_clicked();
    void on_btnAddToBtrfs_clicked();
    void on_btnBalance_clicked();
    void on_btnCheckBalanceStatus_clicked();
    void on_btnStopBalance_clicked();
    void on_btnStartReplace_clicked();
    void on_btnCancelReplace_clicked();

    void onBtrfsHealthChanged(const QString &fsUuid, BtrfsMonitor::HealthState state, const QString &reason);

    // ============================================================
    // D-BUS BALANCE
    // ============================================================

    void onAgentBalanceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void onAgentBalanceProgressChanged(const QString &mountPoint, int completedPercent,int leftPercent);

    void onBalanceStatusReceived(const QString &mountPoint, const QString &state);

    void onStartBalanceFinished(const QString &mountPoint, bool accepted);

    void onCancelBalanceFinished(const QString &mountPoint, bool accepted);

    void onAddDeviceFinished(const QString &mountPoint, const QString &devicePath, bool accepted);

    void onRemoveDeviceFinished(const QString &mountPoint, qulonglong deviceId, bool accepted);

    void onRemoveDeviceProgress(const QString &mountPoint, qulonglong deviceId, int completedPercent, qulonglong remainingBytes);

    // ============================================================
    // D-BUS DEVICE REPLACE
    // ============================================================

    void onStartReplaceFinished(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath, bool accepted);

    void onCancelReplaceFinished(const QString &mountPoint, bool accepted);

    void onReplaceStatusReceived(const QString &mountPoint, const QString &state, int completedPercent, const QString &details);

    void onAgentReplaceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void onAgentReplaceProgressChanged(const QString &mountPoint, int completedPercent);

    void onAgentDbusError(const QString &operation, const QString &mountPoint, const QString &message);

    // ============================================================
    // D-BUS SCRUB
    // ============================================================

    void on_btnStartScrub_clicked();
    void on_btnStopScrub_clicked();

    void onAgentScrubStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void onAgentScrubProgressChanged(const QString &mountPoint, int completedPercent, qulonglong processedBytes, qulonglong totalBytes, qulonglong speedBytesPerSec, int errorCount);

    void onScrubStatusReceived(const QString &mountPoint, const QString &state);

    void onAutoScrubConfigReceived(const QString &mountPoint, bool enabled, int intervalWeeks);

    void onAutoScrubScheduleReceived(const QString &mountPoint, qint64 lastSuccessfulEpoch, qint64 nextDueEpoch);

    void onSetAutoScrubConfigFinished(const QString &mountPoint, bool accepted);

    void onStartScrubFinished(const QString &mountPoint, bool accepted);

    void onCancelScrubFinished(const QString &mountPoint, bool accepted);


private:
    bool openMonitorDatabase();

    QSqlDatabase m_monitorDb;

    // ============================================================
    // DAEMON D-BUS CLIENT
    // ============================================================

    AgentDbusClient *m_agentDbusClient = nullptr;

    bool m_balanceStatusDialogPending = false;

    bool m_addDevicePending = false;

    QString m_pendingAddDeviceName;

    // ============================================================
    // REMOVE DEVICE
    // ============================================================

    QHash<QString, qulonglong> m_activeRemovals;

    bool isRemovalActive(const QString &mountPoint, qulonglong deviceId) const;

    QTimer *m_removalHideTimer = nullptr;

    // ============================================================
    // DEVICE REPLACE
    // ============================================================

    bool isReplaceRunning = false;

    QString currentReplaceMount;
    QString currentReplaceState = QStringLiteral("idle");

    int currentReplacePercent = -1;

    QString currentReplaceDetails;

    // ============================================================
    // VARIABILI EVENT
    // ============================================================

    BtrfsMonitor *m_btrfsMonitor = nullptr;
    BtrfsEventManager *m_eventManager = nullptr;
    BtrfsEventWidget *m_eventWidget = nullptr;

    // ============================================================
    // VARIABILI MEMBRO
    // ============================================================

    Ui::MainWindow *ui;

    QVBoxLayout *scrollLayout = nullptr;

    DiskInfo currentSelectedDisk;
    bool isDiskSelected = false;

    // ============================================================
    // VARIABILI PER IL BILANCIAMENTO
    // ============================================================

    bool isBalanceRunning = false;

    QString currentBalanceMount;
    QString currentBalanceState;

    // ============================================================
    // METODI PER IL BILANCIAMENTO
    // ============================================================

    void balanceBtrfs(const QString &mountPoint);

    void setBalanceState(const QString &state, const QString &status = QString());

    void refreshBalanceDataFromDatabase(const QString &mountPoint);

    // ============================================================
    // VARIABILI SCRUB
    // ============================================================

    bool isScrubRunning = false;

    QString currentScrubMount;
    QString currentScrubState;

    // ============================================================
    // METODI SCRUB
    // ============================================================

    void setScrubState(const QString &state, const QString &status = QString());

    void saveAutoScrubConfig();

    // ------------------------------------------------------------
    // METODO PER AGGIUNGERE UN DISCO A BTRFS
    // ------------------------------------------------------------

    void addDiskToBtrfs(const DiskInfo &diskInfo);

    // ------------------------------------------------------------
    // Filtro device nella lista
    // ------------------------------------------------------------

    bool isPhysicalDisk(const QJsonObject &device) const;

    // ------------------------------------------------------------
    // Mount point
    // ------------------------------------------------------------

    void populateMountPoints();

    // ------------------------------------------------------------
    // Btrfs information/model
    // ------------------------------------------------------------

    BtrfsInfo updateBtrfsInformation(const QString &mountPoint);

    void populateDeviceSummary(BtrfsInfo &info, const QString &mountPoint) const;

    quint64 parseBtrfsSize(const QString &value) const;

    void updateBtrfsOverview(const BtrfsInfo &info);

    void updateFilesystemHealth(const BtrfsInfo &info);

    // ------------------------------------------------------------
    // Utility device
    // ------------------------------------------------------------

    QString deviceNameFromPath(const QString &path) const;

    QJsonObject findLsblkDevice(const QJsonArray &devices, const QString &wantedName) const;

    // ------------------------------------------------------------
    // Disk hierarchy
    // ------------------------------------------------------------

    QMap<QString, DiskHierarchy>
    buildDiskHierarchy(const QJsonArray &blockdevices);

    QString getDiskStatusFromPartitions(const DiskHierarchy &hierarchy, const QSet<QString> &btrfsDevices, const QSet<QString> &btrfsErrorDevices);

    // ------------------------------------------------------------
    // GUI disk rows
    // ------------------------------------------------------------

    void addDiskRow(const DiskInfo &info, bool isBtrfs);
};

#endif // MAINWINDOW_H