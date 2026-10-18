// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DISKINFO_H
#define DISKINFO_H

#include <QString>
#include <QtGlobal>
#include <QStringList>

    struct DiskInfo
{
    QString devicePath;      // /dev/sda1
    QString name;             // sda1
    QString filesystem;       // btrfs, ext4, ...
    QString size;             // 931.5G
    QString uuid;             // UUID completo
    QString mountPoint;       // /mnt/data
    QString status;            // healthy, error, missing
    QString btrfsDeviceId;     // Btrfs devid dal database

    // Nuovi campi per gestire le relazioni
    QString parentDisk;           // Nome del disco padre (es. "sda" per "sda1")
    QStringList childPartitions;  // Lista delle partizioni su questo disco
    bool isPartition = false;             // True se è una partizione, false se è un disco
    bool isWholeDiskBtrfsMember = false;
    quint64 totalDiskSize = 0;        // Dimensione totale del disco (per i dischi fisici)
    quint64 usedDiskSize = 0;         // Spazio usato sul disco

    // ============================================================
    // Btrfs device stats
    // ============================================================

    quint64 readErrors = 0;
    quint64 writeErrors = 0;
    quint64 flushErrors = 0;
    quint64 corruptionErrors = 0;
    quint64 generationErrors = 0;

    // ============================================================
    // SMART state - solo dati necessari alla lista device
    // ============================================================

    bool hasSmartData = false;
    bool smartPresent = false;
    bool smartReadOk = false;
    int smartSupported = -1;
    int smartHealthState = 0;

    // Indica se il disco può essere aggiunto al filesystem Btrfs
    bool canBeAddedToBtrfs = false;

};


#endif // DISKINFO_H
