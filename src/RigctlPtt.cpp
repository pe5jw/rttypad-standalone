/* RigctlPtt.cpp
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "RigctlPtt.h"
#include <QDebug>
#include <QTimer>

RigctlPtt::RigctlPtt(QObject *parent) : QObject(parent) {}

void RigctlPtt::setPtt(bool tx)
{
    sendCommand(tx ? QByteArray("T 1\n") : QByteArray("T 0\n"));
}

void RigctlPtt::sendCommand(const QByteArray &cmd)
{
    // Each PTT command opens a fresh connection; rigctld is stateless per command.
    auto *sock = new QTcpSocket(this);

    connect(sock, &QTcpSocket::connected, this, [sock, cmd] {
        sock->write(cmd);
        sock->flush();
        // Give rigctld a moment to process before closing.
        QTimer::singleShot(200, sock, [sock] {
            sock->disconnectFromHost();
        });
    });

    connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);

    connect(sock, &QAbstractSocket::errorOccurred, this,
            [this, sock](QAbstractSocket::SocketError) {
        emit pttError(QStringLiteral("rigctld PTT error: %1").arg(sock->errorString()));
        qWarning() << "RigctlPtt:" << sock->errorString();
        sock->deleteLater();
    });

    sock->connectToHost(host_, static_cast<quint16>(port_));
    qDebug() << "RigctlPtt: sending" << cmd.trimmed() << "to" << host_ << ":" << port_;
}
