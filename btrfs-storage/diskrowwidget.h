// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DISKROWWIDGET_H
#define DISKROWWIDGET_H

#include <QWidget>
#include "diskinfo.h"
#include <QRegularExpression>

namespace Ui {
class diskrowwidget;
}

class diskrowwidget : public QWidget
{
    Q_OBJECT

public:
    explicit diskrowwidget(QWidget *parent = nullptr);
    ~diskrowwidget();

   /*************************************
    * DATI DISCO
   **************************************/
   void setDiskData(const DiskInfo &info, bool isBtrfs);

    const DiskInfo &getDiskInfo() const;


    //gestire lo stato visivo di selezione
    void setSelected(bool selected);

signals:
    // Segnale inviato a MainWindow quando l'utente clicca sul riquadro
    void diskClicked(diskrowwidget* clickedWidget);

protected:
    // Sovrascrivo l'evento del mouse nativo di Qt
    void mousePressEvent(QMouseEvent *event) override;


private:
    Ui::diskrowwidget *ui;
    DiskInfo diskInfo;
    bool m_selected = false;
    QString m_baseStyle;
    QString m_selectionStyle;
    bool m_isBtrfs = false;


};

#endif // DISKROWWIDGET_H
