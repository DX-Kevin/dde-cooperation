// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MAINCONTROLLER_H
#define MAINCONTROLLER_H

#include <QObject>
#include <QDBusInterface>

namespace daemon_cooperation {

class MainController : public QObject
{
    Q_OBJECT
public:
    struct TransferInfo
    {
        int64_t totalSize = 0;   // 总量
        int64_t transferSize = 0;   // 当前传输量
        int32_t maxTimeSec = 0;   // 耗时

        void clear()
        {
            totalSize = 0;
            transferSize = 0;
            maxTimeSec = 0;
        }
    };

    static MainController *instance();

    void regist();
    void unregist();

public Q_SLOTS:
    void onAppAttributeChanged(const QString &group, const QString &key, const QVariant &value);
    void waitForConfirm(const QString &name);
    void onActionTriggered(uint replacesId, const QString &action);
    void onTransJobStatusChanged(int id, int result, const QString &msg);
    void onFileTransStatusChanged(const QString &status);

private:
    explicit MainController(QObject *parent = nullptr);

    void initConnect();
    uint notifyMessage(uint replacesId, const QString &body,
                       const QStringList &actions, QVariantMap hitMap, int expireTimeout);
    void transferResult(bool result, const QString &msg);
    void updateProgress(int value, const QString &remainTime);
    void openFileLocation(const QString &path);

private:
    QDBusInterface *notifyIfc { nullptr };
    TransferInfo transferInfo;
    QString recvFilesSavePath;
    QMap<int, int64_t> fileIds;   // <file_id, last_current_size> 统计正在传输的文件量<文件id，上次已传输量>
    uint recvNotifyId { 0 };
    bool isTransTimeout = false;

};

}   // namespace daemon_cooperation

#endif   // MAINCONTROLLER_H