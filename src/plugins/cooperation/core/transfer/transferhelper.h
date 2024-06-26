// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSFERHELPER_H
#define TRANSFERHELPER_H

#include "global_defines.h"
#include "info/deviceinfo.h"

namespace cooperation_core {

class TransferHelperPrivate;
class TransferHelper : public QObject
{
    Q_OBJECT

public:
    enum TransferStatus {
        Idle,
        Confirming,
        Connecting,
        Transfering
    };

    static TransferHelper *instance();

    void regist();
    void sendFiles(const QString &ip, const QString &devName, const QStringList &fileList);
    void searchDevice(const QString &ip);
    TransferStatus transferStatus();

    static void buttonClicked(const QString &id, const DeviceInfoPointer info);
    static bool buttonVisible(const QString &id, const DeviceInfoPointer info);
    static bool buttonClickable(const QString &id, const DeviceInfoPointer info);

public Q_SLOTS:
    void onConnectStatusChanged(int result, const QString &msg, const bool isself);
    void onTransJobStatusChanged(int id, int result, const QString &msg);
    void onFileTransStatusChanged(const QString &status);
    void waitForConfirm();
    void accepted();
    void rejected();
    void cancelTransfer();

private:
    explicit TransferHelper(QObject *parent = nullptr);
    ~TransferHelper();

private:
    QSharedPointer<TransferHelperPrivate> d { nullptr };
};

}   // namespace cooperation_core

#endif   // TRANSFERHELPER_H
