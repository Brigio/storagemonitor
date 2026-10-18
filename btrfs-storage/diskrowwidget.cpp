// SPDX-License-Identifier: GPL-3.0-or-later

#include "diskrowwidget.h"
#include "ui_diskrowwidget.h"
#include "diskinfo.h"

#include <QIcon>
#include <QPixmap>
#include <QRegularExpression>

diskrowwidget::diskrowwidget(QWidget *parent) : QWidget(parent), ui(new Ui::diskrowwidget)
{
    ui->setupUi(this);

    // Imposta lo stile di default per il frame
    m_baseStyle =
        "QFrame#frame {"
        "   background-color: palette(base);"
        "   border: 1px solid palette(mid);"
        "   border-radius: 4px;"
        "}"
        "QFrame#frame:hover {"
        "   border: 1px solid palette(highlight);"
        "   background-color: palette(alternate-base);"
        "}";

    if (ui->frame) {
        ui->frame->setStyleSheet(m_baseStyle);
    }

    // Nascondi l'icona di gerarchia di default
    if (ui->labelHierarchyIcon) {
        ui->labelHierarchyIcon->setVisible(false);
    }
}

diskrowwidget::~diskrowwidget()
{
    delete ui;
}

void diskrowwidget::setDiskData(const DiskInfo &info, bool isBtrfs)
{
    // Salva il modello completo del disco
    diskInfo = info;
    m_isBtrfs = isBtrfs;

    // ============================================================
    // COSTRUISCI LO STILE BASE COMPLETO PER IL FRAME
    // ============================================================

    QString baseStyle;

    if (info.isPartition) {
        // Stile per partizioni (con rientro)
        baseStyle =
            "QFrame#frame {"
            "   padding-left: 5px;"
            "   background-color: palette(base);"
            "   border: 1px solid palette(mid);"
            "   border-radius: 4px;"
            "}"
            "QFrame#frame:hover {"
            "   border: 1px solid palette(highlight);"
            "   background-color: palette(alternate-base);"
            "}";

        // Mostra l'indicatore di gerarchia
        if (ui->labelHierarchyIcon) {
            ui->labelHierarchyIcon->setVisible(true);
            ui->labelHierarchyIcon->setText("└─");
            ui->labelHierarchyIcon->setStyleSheet("color: #888; font-size: 14px; font-weight: bold;");
        }
    } else {
        // Stile per dischi fisici (senza rientro)
        baseStyle =
            "QFrame#frame {"
            "   padding-left: 5px;"
            "   background-color: palette(base);"
            "   border: 1px solid palette(mid);"
            "   border-radius: 4px;"
            "}"
            "QFrame#frame:hover {"
            "   border: 1px solid palette(highlight);"
            "   background-color: palette(alternate-base);"
            "}";

        // Nascondi l'indicatore di gerarchia
        if (ui->labelHierarchyIcon) {
            ui->labelHierarchyIcon->setVisible(false);
        }
    }

    // Aggiungi il bordo colorato per lo stato
    QString borderColor;
    if (info.status == "healthy") {
        borderColor = "#4CAF50";
    } else if (info.status == "warning" || info.status == "missing") {
        borderColor = "#FF9800";
    } else if (info.status == "error") {
        borderColor = "#F44336";
    }

    if (info.isPartition) {
        // Per le partizioni, usa il bordo sinistro
        baseStyle.replace("border-left: 3px solid #888;",
                          QString("border-left: 3px solid %1;").arg(borderColor));
    } else {
        // Per i dischi, usa il bordo inferiore
        baseStyle += QString("QFrame#frame { border-bottom: 3px solid %1; }").arg(borderColor);
    }

    // Salva lo stile base
    m_baseStyle = baseStyle;

    // Applica lo stile base al FRAME
    if (ui->frame) {
        if (!m_selected) {
            ui->frame->setStyleSheet(m_baseStyle);
        } else {
            ui->frame->setStyleSheet(m_baseStyle + "\n" +
                "QFrame#frame {"
                "   background-color: palette(highlight);"
                "   border: 2px solid palette(highlight);"
                "   border-radius: 4px;"
                "}"
                "QFrame#frame QLabel#labelTitolto,"
                "QFrame#frame QLabel#labelSottotitolo {"
                "   color: palette(highlighted-text);"
                "}");
        }
    }

    // ============================================================
    // TITOLO
    // ============================================================

    QString title;

    if (info.isWholeDiskBtrfsMember)
    {
        title = QStringLiteral("%1 [Btrfs whole disk]").arg(info.name);
    }
    else if (info.isPartition)
    {
        title = info.name;

        if (!info.parentDisk.isEmpty())
        {
            title = QStringLiteral("%1 (on %2)").arg(info.name, info.parentDisk);
        }
    }
    else
    {
        title = info.name;
    }

    if (!info.mountPoint.isEmpty()) {
        title = QString("%1 (%2)").arg(title, info.mountPoint);
    }

    if (!info.isPartition && !info.childPartitions.isEmpty()) {
        title = QString("%1 [%2 Btrfs]")
        .arg(title)
            .arg(info.childPartitions.size());
    }

    if (!info.isPartition && info.filesystem == "disk" && !info.childPartitions.isEmpty()) {
        title = QString("📀 %1").arg(title);
    }

    // ============================================================
    // SOTTOTITOLO
    // ============================================================

    QString subtitle;
    if (info.isPartition) {
        subtitle = QString("Size: %1 | UUID: %2")
        .arg(info.size.isEmpty() ? "-" : info.size, info.uuid.isEmpty() ? "-" : info.uuid.left(8));

        if (!info.parentDisk.isEmpty()) {
            subtitle = QString("%1 | Parent: %2").arg(subtitle, info.parentDisk);
        }
    } else {
        if (!info.childPartitions.isEmpty()) {
            subtitle = QString("Size: %1 | Btrfs partitions: %2")
            .arg(info.size.isEmpty() ? "-" : info.size, QString::number(info.childPartitions.size()));
        } else {
            subtitle = QString("Size: %1 | UUID: %2")
            .arg(info.size.isEmpty() ? "-" : info.size, info.uuid.isEmpty() ? "-" : info.uuid.left(8));
        }
    }

    // ============================================================
    // SMART SUMMARY
    // Solo per i dischi fisici che hanno dati SMART dal daemon.
    // ============================================================

    if (!info.isPartition && info.hasSmartData)
    {
        QString smartText;

        if (!info.smartPresent) { smartText = QStringLiteral("SMART: Missing"); }
        else if (!info.smartReadOk) { smartText = QStringLiteral("SMART: Read error"); }
        else if (info.smartSupported == 0) { smartText = QStringLiteral("SMART: N/A"); }
        else if (info.smartSupported < 0) { smartText = QStringLiteral("SMART: Unknown"); }
        else
        {
            switch (info.smartHealthState)
            {
            case 1: smartText = QStringLiteral("SMART: OK"); break;
            case 2: smartText = QStringLiteral("SMART: Warning"); break;
            case 3: smartText = QStringLiteral("SMART: Error"); break;
            case 0:
            default: smartText = QStringLiteral("SMART: Unknown"); break;
            }
        }

        subtitle += QStringLiteral(" | ") + smartText;
    }

    // ============================================================
    // APPLICA TITOLO E SOTTOTITOLO
    // ============================================================

    if (ui->labelTitolto)
    {
        ui->labelTitolto->setText(title);
    }

    if (ui->labelSottotitolo)
    {
        ui->labelSottotitolo->setText(subtitle);
    }

    // ============================================================
    // STATO SMART VISIVO
    // Applicato solo ai dischi fisici.
    // SMART unsupported (smartSupported == 0) resta neutro.
    // ============================================================

    const bool smartError =
        !info.isPartition &&
        info.hasSmartData &&
        info.smartPresent &&
        info.smartReadOk &&
        info.smartSupported > 0 &&
        info.smartHealthState == 3;

    const bool smartWarning =
        !info.isPartition &&
        info.hasSmartData &&
        (
            !info.smartPresent ||
            !info.smartReadOk ||
            (
                info.smartSupported > 0 &&
                info.smartHealthState == 2
                )
            );

    const bool smartAttention =
        smartWarning || smartError;

    // ============================================================
    // STATUS
    //
    // Priorità:
    // 1. Btrfs / stato disco
    // 2. SMART Error
    // 3. SMART Warning
    // 4. stato normale
    // ============================================================

    QString statusText;
    QString statusColor;

    if (info.status == "error")
    {
        statusText = "❌ Error";
        statusColor = "#F44336";
    }
    else if (info.status == "missing")
    {
        statusText = "❓ Missing";
        statusColor = "#FF9800";
    }
    else if (info.status == "warning")
    {
        statusText = "⚠️ Warning";
        statusColor = "#FF9800";
    }
    else if (smartError)
    {
        statusText = "❌ SMART Error";
        statusColor = "#F44336";
    }
    else if (smartWarning)
    {
        statusText = "⚠️ SMART Warning";
        statusColor = "#FF9800";
    }
    else if (info.status == "healthy")
    {
        statusText = "✅ Healthy";
        statusColor = "#4CAF50";
    }
    else
    {
        statusText = "Status: " + info.status;
        statusColor = "#888";
    }

    if (ui->labelStatus)
    {
        ui->labelStatus->setText(statusText);
        ui->labelStatus->setStyleSheet(
            QString("color: %1; font-weight: bold;")
                .arg(statusColor));
    }

    // ============================================================
    // ICONE
    // ============================================================

    QString finalIconPath;

    if (info.isPartition) {

        // ========================================================
        // PARTIZIONI
        // SMART non viene applicato alle partizioni.
        // ========================================================

        if (isBtrfs) {

            if (info.status == "missing") {
                finalIconPath = ":/icons/btrfs_missing.png";

            } else if (info.status == "error") {
                finalIconPath = ":/icons/btrfs_error.png";

            } else {
                finalIconPath = ":/icons/btrfs_ok.png";
            }

        } else {

            if (info.status == "missing") {
                finalIconPath = ":/icons/hhd_ko.png";

            } else if (info.status == "error") {
                finalIconPath = ":/icons/hhd_alarm.png";

            } else {
                finalIconPath = ":/icons/hhd_ok.png";
            }
        }

    } else {

        // ========================================================
        // DISCHI FISICI
        //
        // Priorità:
        // 1. stato Btrfs / disco
        // 2. stato SMART
        //
        // SMART unsupported (smartSupported == 0)
        // NON genera warning.
        // ========================================================

        if (info.filesystem == "disk" &&
            !info.childPartitions.isEmpty())
        {
            // Disco fisico con partizioni Btrfs.

            if (info.status == "error") {
                finalIconPath = ":/icons/hhd_alarm.png";

            } else if (info.status == "warning" || info.status == "missing") {
                finalIconPath = ":/icons/hhd_alarm.png";

            } else if (smartAttention) {
                finalIconPath = ":/icons/hhd_alarm.png";

            } else {
                finalIconPath = ":/icons/hhd_ok.png";
            }

        } else {

            // Disco fisico senza partizioni Btrfs.

            if (info.status == "missing") {
                finalIconPath = ":/icons/hhd_ko.png";

            } else if (info.status == "error") {
                finalIconPath = ":/icons/hhd_alarm.png";

            } else if (info.status == "warning") {
                finalIconPath = ":/icons/hhd_alarm.png";

            } else if (smartAttention) {
                finalIconPath = ":/icons/hhd_alarm.png";

            } else {
                finalIconPath = ":/icons/hhd_ok.png";
            }
        }
    }

    QIcon diskIcon(finalIconPath);
    QSize iconSize = info.isPartition ? QSize(40, 40) : QSize(48, 48);
    if (ui->labelIcona) {
        ui->labelIcona->setPixmap(diskIcon.pixmap(iconSize));
        ui->labelIcona->setFixedSize(iconSize);
    }

    // Debug
    qDebug() << "setDiskData chiamato per:" << info.name
             << "isPartition:" << info.isPartition
             << "status:" << info.status;
}

// ============================================================
// setSelected - GESTISCE LA SELEZIONE
// ============================================================

void diskrowwidget::setSelected(bool selected)
{
    m_selected = selected;

    qDebug() << "setSelected chiamato con:" << selected << "per:" << diskInfo.name;

    if (ui->frame) {
        if (selected) {
            ui->frame->setStyleSheet(
                "QFrame#frame {"
                "   background-color: palette(highlight);"
                "   border: 2px solid palette(highlight);"
                "   border-radius: 4px;"
                "}"
                "QFrame#frame QLabel#labelTitolto,"
                "QFrame#frame QLabel#labelSottotitolo {"
                "   color: palette(highlighted-text);"
                "}"
                );
        } else {
            ui->frame->setStyleSheet(m_baseStyle);
        }
    }
}

// ============================================================
// mousePressEvent
// ============================================================

void diskrowwidget::mousePressEvent(QMouseEvent *event)
{
    qDebug() << "mousePressEvent su:" << diskInfo.name;
    emit diskClicked(this);
    QWidget::mousePressEvent(event);
}

// ============================================================
// getDiskInfo
// ============================================================

const DiskInfo &diskrowwidget::getDiskInfo() const
{
    return diskInfo;
}