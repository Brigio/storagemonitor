// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef BTRFSMONITOR_H
#define BTRFSMONITOR_H

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QVariant>
#include <QMap>

// ============================================================
// ENUM PER I PROFILI RAID
// ============================================================
enum class BtrfsProfile {
    Single,
    Raid0,
    Raid1,
    Raid1c3,
    Raid1c4,
    Raid10,
    Raid5,
    Raid6,
    Unknown
};

// ============================================================
// STRUTTURA Bilanciamento
// ============================================================
struct RaidImbalanceInfo
{
    bool detected = false;

    QString fullestDevice;
    QString emptiestDevice;

    double fullestPercent = 0.0;
    double emptiestPercent = 0.0;
    double differencePercent = 0.0;
};

// ============================================================
// STRUTTURA SpaceInfo
// ============================================================
struct SpaceInfo
{
    quint64 total = 0;
    quint64 used = 0;
    quint64 unallocated = 0;
    quint64 metadataTotal = 0;
    quint64 metadataUsed = 0;
    quint64 dataTotal = 0;
    quint64 dataUsed = 0;
    quint64 systemTotal = 0;
    quint64 systemUsed = 0;

    BtrfsProfile dataProfile = BtrfsProfile::Unknown;
    BtrfsProfile metadataProfile = BtrfsProfile::Unknown;
    quint64 logicalUnallocated = 0;
    double logicalFactor = 1.0;
};

class EmailNotifier;

class BtrfsEventManager;
//Non serve includere smartmonitor.h nell'header
class SmartMonitor;

class SystemNotifier;

class BtrfsMonitor : public QObject
{
    Q_OBJECT

public:

    enum class HealthState
    {
        Unknown,
        Ok,
        Warning,
        Error
    };
    Q_ENUM(HealthState)

    explicit BtrfsMonitor(QObject *parent = nullptr);
    ~BtrfsMonitor();

    HealthState healthState(const QString &fsUuid) const;

    struct AutoScrubConfig
    {
        bool enabled = false;
        int intervalWeeks = 4;
    };

    struct AutoScrubSchedule
    {
        qint64 lastSuccessfulEpoch = 0;
        qint64 nextDueEpoch = 0;
    };

    bool autoScrubConfig(const QString &fsUuid, AutoScrubConfig &config) const;

    bool setAutoScrubConfig(const QString &fsUuid, bool enabled, int intervalWeeks);

    bool autoScrubConfigForMountPoint(const QString &mountPoint, AutoScrubConfig &config) const;

    bool setAutoScrubConfigForMountPoint(const QString &mountPoint, bool enabled, int intervalWeeks);

    bool setAutoScrubRetryAfterForMountPoint(const QString &mountPoint, qint64 retryAfterEpoch);

    bool autoScrubScheduleForMountPoint(const QString &mountPoint, AutoScrubSchedule &schedule) const;

    bool dueAutoScrubMountPoints(qint64 nowEpoch, QStringList &mountPoints) const;

    QStringList currentBtrfsMountPoints() const;

    bool btrfsDevicePresence(const QString &mountPoint, qulonglong deviceId, bool &present) const;

    bool btrfsDeviceAllocatedBytes(const QString &mountPoint, qulonglong deviceId, quint64 &allocatedBytes) const;

    bool btrfsDeviceTotalBytes(const QString &mountPoint, qulonglong deviceId, quint64 &totalBytes) const;

    bool beginDeviceReplacement(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath, const QString &details);

    bool updateDeviceReplacement(const QString &mountPoint, const QString &status, int progressPercent, const QString &details);

    bool finishDeviceReplacement(const QString &mountPoint, const QString &status, int progressPercent, const QString &details);

    bool acknowledgeEvent(const QString &source, qint64 eventId);

    bool resolveEvent(const QString &source, qint64 eventId);

    bool clearEventHistory();

signals:
    void healthChanged(const QString &fsUuid, BtrfsMonitor::HealthState state, const QString &reason);

public slots:
    void refreshNow();

private slots:

    void checkBtrfsSysfs();

    void onSmartDeviceUpdated(const QString &devicePath);
private:

    EmailNotifier *m_emailNotifier = nullptr;

    /*
    * Ultimo health SMART visto durante questa esecuzione.
    * key   = /dev/sda, /dev/nvme0, ...
    * value = SmartHealthState convertito in int
    */
    QHash<QString, int> m_lastSmartHealth;

    // ============================================================
    // METODI PER GESTIRE I PROFILI RAID
    // ============================================================
    bool readProfileInfo(const QString &fsUuid, BtrfsProfile &dataProfile, BtrfsProfile &metadataProfile) const;
    BtrfsProfile stringToProfile(const QString &profileStr) const;
    QString profileToString(BtrfsProfile profile) const;
    double getLogicalFactor(BtrfsProfile profile, int numDevices) const;
    void calculateLogicalUnallocated(SpaceInfo &space, int numDevices) const;

    QString getShortReason(const QString &reason) const;

    BtrfsEventManager *m_eventManager = nullptr;

    SmartMonitor *m_smartMonitor = nullptr;

    // ============================================================
    // METODI PER IL DEBUG DEL FORMATO
    // ============================================================
    void verifySysfsFormat(const QString &fsUuid) const;
    void logSpaceInfo(const QString &fsUuid, const SpaceInfo &space) const;

    struct DeviceStats
    {
        QString devicePath;

        quint64 readErrors = 0;
        quint64 writeErrors = 0;
        quint64 flushErrors = 0;
        quint64 corruptionErrors = 0;
        quint64 generationErrors = 0;

        quint64 deviceTotal = 0;
        quint64 deviceAllocated = 0;
        quint64 deviceUnallocated = 0;
    };

    struct DeviceState
    {
        bool present = true;
        DeviceStats stats;
    };

    struct FilesystemState
    {
        HealthState health = HealthState::Unknown;
        int deviceCount = 0;
        QHash<QString, DeviceState> devices;
        SpaceInfo space;
        bool initialized = false;
        QString mountPoint;
        QString scrubStatus;
        QString balanceStatus;
    };

    QHash<QString, FilesystemState> m_filesystems;
    //m_notifier è un puntatore
    SystemNotifier *m_notifier;

    mutable bool m_formatVerified = false;

    // ============================================================
    // DATABASE MEMBERS (SOLO PER SCRITTURA)
    // ============================================================
    QSqlDatabase m_db;
    bool m_dbInitialized = false;
    QString m_dbPath;

    // ============================================================
    // OPERATION STATE TRACKING (per evitare scritture duplicate)
    // ============================================================
    struct OperationState {
        QString status;
        QDateTime startTime;
        bool isRunning = false;
        quint64 processedBytes = 0;
        quint64 speedBytesPerSec = 0;
        int errorCount = 0;
    };

    QHash<QString, OperationState> m_scrubStates;
    QHash<QString, OperationState> m_balanceStates;

    // ============================================================
    // DATABASE METHODS (SOLO SCRITTURA)
    // ============================================================
    bool initializeDatabase();
    bool createTables();

    bool saveFilesystemState(const QString &fsUuid, const FilesystemState &state);

    bool saveBtrfsDevices(const QString &fsUuid, const FilesystemState &state);

    bool removeStaleBtrfsDevices(const QString &fsUuid, const QSet<QString> &memberDeviceIds);

    bool saveOperationState(const QString &fsUuid, const QString &operationType, const QString &status, quint64 processedBytes = 0, quint64 speed = 0, int errorCount = 0);

    bool updateOperationState(const QString &fsUuid, const QString &operationType, const QString &status, quint64 processedBytes = 0, quint64 speed = 0, int errorCount = 0);

    bool getLastOperationState(const QString &fsUuid, const QString &operationType, QString &status, QDateTime &startTime) const;

    // ============================================================
    // OPERATION STATE PROCESSING
    // ============================================================
    void processScrubState(const QString &fsUuid, const QString &fsPath);

    void processBalanceState(const QString &fsUuid, const QString &fsPath);

    void readScrubStatus(const QString &mountPoint, QString &status, quint64 &processedBytes, quint64 &speed, int &errorCount) const;

    void readBalanceStatus(const QString &mountPoint, QString &status) const;

    bool filesystemUuidForMountPoint(const QString &mountPoint, QString &fsUuid) const;

    // ============================================================
    // METODI ESISTENTI
    // ============================================================
    bool readDeviceStats(const QString &filePath, DeviceStats &stats) const;
    HealthState evaluateFilesystem(const FilesystemState &filesystem, const QSet<QString> &currentDevices, QString &reason) const;
    void processFilesystem(const QString &fsUuid);
    static QString healthStateToString(HealthState state);

    void sendSystemNotification(const QString &fsUuid, HealthState state, const QString &reason);
    void updateFilesystemState(const QString &fsUuid, HealthState newState, const QString &reason);
    void evaluateSpaceRules(const QString &fsUuid, const FilesystemState &filesystem, QStringList &warnings) const;
    void evaluateMetadataRules(const QString &fsUuid, const FilesystemState &filesystem, QStringList &warnings) const;
    RaidImbalanceInfo evaluateRaidBalanceRules(const QString &fsUuid, const FilesystemState &filesystem, QStringList &warnings) const;

    // ============================================================
    // METODI PER LETTURA SPAZIO
    // ============================================================
    bool readSpaceInfoFromBtrfsCommand(const QString &mountPoint, SpaceInfo &space) const;
    QString findMountPointForUUID(const QString &fsUuid) const;
    bool readSpaceInfo(const QString &fsUuid, SpaceInfo &space) const;
    bool readDeviceSpaceInfo(const QString &fsUuid, const QString &devId, DeviceState &devState, int deviceCount) const;
    bool readDeviceSpaceInfoFromBtrfsCommand(const QString &mountPoint, QHash<QString, DeviceStats> &deviceStats) const;
    quint64 convertToBytes(double value, const QString &unit) const;
    QString formatBytes(quint64 bytes) const;
    bool parseBtrfsUsageOutput(const QString &output, SpaceInfo &space) const;

    // ============================================================
    // COSTANTI
    // ============================================================
    static constexpr quint64 MIN_UNALLOCATED_BYTES = 2ULL * 1024 * 1024 * 1024;
    static constexpr double UNALLOCATED_PERCENT_THRESHOLD = 10.0;
    static constexpr double METADATA_USAGE_THRESHOLD = 30.0;
    static constexpr quint64 METADATA_SIZE_THRESHOLD = 10ULL * 1024 * 1024 * 1024;
    static constexpr double RAID_IMBALANCE_THRESHOLD = 15.0; //15.0;
};

#endif // BTRFSMONITOR_H
