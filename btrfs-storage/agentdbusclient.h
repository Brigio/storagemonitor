// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class AgentDbusClient final : public QObject
{
    Q_OBJECT

public:
    explicit AgentDbusClient(QObject *parent = nullptr);

    void startBalance(const QString &mountPoint);
    void cancelBalance(const QString &mountPoint);
    void requestBalanceStatus(const QString &mountPoint);
    void startScrub(const QString &mountPoint);
    void cancelScrub(const QString &mountPoint);
    void requestScrubStatus(const QString &mountPoint);
    void requestAutoScrubConfig(const QString &mountPoint);
    void requestAutoScrubSchedule(const QString &mountPoint);
    void setAutoScrubConfig(const QString &mountPoint, bool enabled, int intervalWeeks);
    void addDevice(const QString &mountPoint, const QString &devicePath);
    void removeDevice(const QString &mountPoint, qulonglong deviceId);
    void startReplace(const QString &mountPoint, qulonglong sourceDeviceId,
    const QString &targetDevicePath);
    void cancelReplace(const QString &mountPoint);
    void requestReplaceStatus(const QString &mountPoint);
    void acknowledgeEvent(const QString &source, qulonglong eventId);
    void resolveEvent(const QString &source, qulonglong eventId);
    void clearEventHistory();

signals:
    void startBalanceFinished(const QString &mountPoint, bool accepted);

    void cancelBalanceFinished(const QString &mountPoint, bool accepted);

    void balanceStatusReceived(const QString &mountPoint, const QString &state);

    void balanceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void balanceProgressChanged(const QString &mountPoint, int completedPercent, int leftPercent);

    void startScrubFinished(const QString &mountPoint, bool accepted);

    void cancelScrubFinished(const QString &mountPoint, bool accepted);

    void scrubStatusReceived(const QString &mountPoint, const QString &state);

    void autoScrubConfigReceived(const QString &mountPoint, bool enabled, int intervalWeeks);

    void autoScrubScheduleReceived(const QString &mountPoint, qint64 lastSuccessfulEpoch, qint64 nextDueEpoch);

    void setAutoScrubConfigFinished(const QString &mountPoint, bool accepted);

    void scrubStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void scrubProgressChanged(const QString &mountPoint, int completedPercent, qulonglong processedBytes, qulonglong totalBytes, qulonglong speedBytesPerSec, int errorCount);

    void addDeviceFinished(const QString &mountPoint, const QString &devicePath, bool accepted);

    void removeDeviceFinished(const QString &mountPoint, qulonglong deviceId, bool accepted);

    void startReplaceFinished(const QString &mountPoint, qulonglong sourceDeviceId, const QString &targetDevicePath, bool accepted);

    void cancelReplaceFinished(const QString &mountPoint, bool accepted);

    void replaceStatusReceived(const QString &mountPoint, const QString &state, int completedPercent, const QString &details);

    void replaceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void replaceProgressChanged(const QString &mountPoint, int completedPercent);

    void acknowledgeEventFinished(const QString &source, qulonglong eventId, bool accepted, const QString &errorMessage);

    void resolveEventFinished(const QString &source, qulonglong eventId, bool accepted, const QString &errorMessage);

    void clearEventHistoryFinished(bool accepted, const QString &errorMessage);

    void errorOccurred(const QString &operation, const QString &mountPoint, const QString &message);

    void agentServiceRegistered();

    void removeDeviceProgress(const QString &mountPoint, qulonglong deviceId, int completedPercent, qulonglong remainingBytes);


private slots:
    void onBalanceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void onBalanceProgressChanged(const QString &mountPoint, int completedPercent, int leftPercent);

    void onRemoveDeviceProgress(const QString &mountPoint, qulonglong deviceId, int completedPercent, qulonglong remainingBytes);

    void onReplaceStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void onReplaceProgressChanged(const QString &mountPoint, int completedPercent);

    void onScrubStateChanged(const QString &mountPoint, const QString &state, const QString &message);

    void onScrubProgressChanged(const QString &mountPoint, int completedPercent, qulonglong processedBytes, qulonglong totalBytes, qulonglong speedBytesPerSec, int errorCount);

};