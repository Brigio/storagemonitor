// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QDBusContext>
#include <functional>
#include <QStringList>

class QTimer;
class BtrfsMonitor;

class AgentDbusService final : public QObject, protected QDBusContext
{
    Q_OBJECT

    Q_CLASSINFO("D-Bus Interface", "org.storage_monitor.Agent")

public:
    explicit AgentDbusService(BtrfsMonitor *monitor, QObject *parent = nullptr);

public slots:

    bool StartBalance(const QString &mountPoint);

    bool CancelBalance(const QString &mountPoint);

    QString BalanceStatus(const QString &mountPoint);

    bool StartScrub(const QString &mountPoint);

    bool CancelScrub(const QString &mountPoint);

    QString ScrubStatus(const QString &mountPoint);

    QStringList ReplaceStatus(const QString &mountPoint);

    bool StartReplace(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath);

    bool CancelReplace(const QString &mountPoint);

    QStringList AutoScrubConfig(const QString &mountPoint);

    QStringList AutoScrubSchedule(const QString &mountPoint);

    bool SetAutoScrubConfig(const QString &mountPoint, bool enabled, int intervalWeeks);

    bool AddDevice(const QString &mountPoint, const QString &devicePath);

    bool RemoveDevice(const QString &mountPoint, qulonglong deviceId);

    bool AcknowledgeEvent(const QString &source, qulonglong eventId);

    bool ResolveEvent(const QString &source, qulonglong eventId);

    bool ClearEventHistory();

signals:

    void BalanceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void BalanceProgressChanged(const QString &mountPoint, int completedPercent, int leftPercent);

    void ScrubStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void ScrubProgressChanged(const QString &mountPoint, int completedPercent, qulonglong processedBytes, qulonglong totalBytes, qulonglong speedBytesPerSec, int errorCount);

    void RemoveDeviceProgress(const QString &mountPoint, qulonglong deviceId, int completedPercent, qulonglong remainingBytes);

    void ReplaceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void ReplaceProgressChanged(const QString &mountPoint, int completedPercent);

private slots:

    void onStartFinished(int exitCode, QProcess::ExitStatus exitStatus);

    void onCancelFinished(int exitCode, QProcess::ExitStatus exitStatus);

    void pollBalance();

    void pollRemoveProgress();

    void onRemoveFinished(int exitCode, QProcess::ExitStatus exitStatus);

    void onReplaceStartFinished(int exitCode, QProcess::ExitStatus exitStatus);

    void onReplaceCancelFinished(int exitCode, QProcess::ExitStatus exitStatus);

    void pollReplace();

    void onScrubStartFinished(int exitCode, QProcess::ExitStatus exitStatus);

    void onScrubCancelFinished(int exitCode, QProcess::ExitStatus exitStatus);

    void pollScrub();

    void checkAutoScrubSchedule();

private:
    using AuthorizedOperation = std::function<bool()>;

    struct ScrubRuntimeInfo
    {
        QString state;
        QString details;

        int completedPercent = -1;

        quint64 processedBytes = 0;
        quint64 totalBytes = 0;
        quint64 speedBytesPerSec = 0;

        int errorCount = 0;
    };

    struct ReplaceRuntimeInfo
    {
        QString state;
        QString details;

        int completedPercent = -1;
    };

    bool validateMountPoint(const QString &mountPoint) const;

    bool validateDevicePath(const QString &devicePath) const;

    bool validateBtrfsDeviceId(qulonglong deviceId) const;

    bool prepareDeviceForBtrfs(const QString &devicePath, QString &preparedDevicePath, QString &errorMessage) const;

    bool prepareReplacementTarget(const QString &devicePath, QString &preparedDevicePath, QString &errorMessage) const;

    bool blockDeviceSizeBytes(const QString &devicePath, quint64 &sizeBytes, QString &errorMessage) const;

    QString queryBalanceState(const QString &mountPoint, QString &details) const;

    ScrubRuntimeInfo queryScrubState(const QString &mountPoint) const;

    ReplaceRuntimeInfo queryReplaceState(const QString &mountPoint) const;

    void refreshMonitor();

    void updateBalanceProgressFromDetails(const QString &mountPoint, const QString &details);

    void authorizeAndRun(const QString &actionId, const QString &mountPoint, AuthorizedOperation operation);

    bool startBalanceAuthorized(const QString &mountPoint);

    bool cancelBalanceAuthorized(const QString &mountPoint);

    bool startScrubAuthorized(const QString &mountPoint);

    bool cancelScrubAuthorized(const QString &mountPoint);

    bool startReplaceAuthorized(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath);

    bool cancelReplaceAuthorized(const QString &mountPoint);

    bool setAutoScrubConfigAuthorized(const QString &mountPoint, bool enabled, int intervalWeeks);

    bool automaticScrubBlocked() const;

    bool reattachRunningReplace();

    bool reattachRunningScrub();

    bool addDeviceAuthorized(const QString &mountPoint, const QString &devicePath);

    bool removeDeviceAuthorized(const QString &mountPoint, qulonglong deviceId);

private:

    QString m_pkcheckPath;

    BtrfsMonitor *m_monitor = nullptr;

    QString m_btrfsPath;

    QProcess *m_startProcess = nullptr;
    QProcess *m_cancelProcess = nullptr;
    QProcess *m_removeProcess = nullptr;

    QTimer *m_balanceTimer = nullptr;
    QTimer *m_removeProgressTimer = nullptr;

    QString m_pendingStartMount;
    QString m_pendingCancelMount;
    QString m_activeBalanceMount;

    QString m_lastBalanceState;

    QString m_pendingRemoveMount;
    qulonglong m_pendingRemoveDeviceId = 0;

    quint64 m_pendingRemoveInitialAllocatedBytes = 0;

    int m_lastBalancePercent = -1;
    int m_scrubStatusErrorCount = 0;
    int m_lastRemovePercent = -1;

    bool m_cancelRequested = false;

    QProcess *m_scrubStartProcess = nullptr;
    QProcess *m_scrubCancelProcess = nullptr;

    QProcess *m_replaceStartProcess = nullptr;
    QProcess *m_replaceCancelProcess = nullptr;

    QString m_pendingReplaceStartMount;
    QString m_pendingReplaceCancelMount;
    QString m_activeReplaceMount;

    qulonglong m_pendingReplaceSourceDeviceId = 0;

    QString m_pendingReplaceTargetDevicePath;

    QString m_lastReplaceState;

    int m_lastReplacePercent = -1;
    int m_replaceStatusErrorCount = 0;

    bool m_replaceCancelRequested = false;

    QTimer *m_replaceTimer = nullptr;

    QTimer *m_scrubTimer = nullptr;
    QTimer *m_autoScrubTimer = nullptr;

    QString m_pendingScrubStartMount;
    QString m_pendingScrubCancelMount;
    QString m_activeScrubMount;

    QString m_lastScrubState;

    int m_lastScrubPercent = -1;

    bool m_scrubCancelRequested = false;

};