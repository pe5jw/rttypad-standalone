/* RigctlPtt.h
 * PTT control via rigctld TCP connection (Hamlib).
 * Sends "T 1\n" for TX and "T 0\n" for RX.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QTcpSocket>

class RigctlPtt : public QObject {
    Q_OBJECT
public:
    explicit RigctlPtt(QObject *parent = nullptr);

    void setHost(const QString &host) { host_ = host; }
    void setPort(int port)            { port_ = port; }
    QString host() const { return host_; }
    int     port() const { return port_; }

    /* Async PTT — fire and forget; emits pttError on failure */
    void setPtt(bool tx);

signals:
    void pttError(const QString &msg);

private:
    void sendCommand(const QByteArray &cmd);
    QString host_ = QStringLiteral("127.0.0.1");
    int     port_ = 4532;
};
