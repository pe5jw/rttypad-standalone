/* SerialPtt.cpp
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "SerialPtt.h"

#include <QSerialPortInfo>
#include <QDebug>

SerialPtt::SerialPtt(QObject *parent) : QObject(parent) {}

SerialPtt::~SerialPtt()
{
    close();
}

bool SerialPtt::open()
{
    if (port_.isOpen()) return true;
    if (portName_.isEmpty()) {
        emit pttError(QStringLiteral("Serial PTT: no port configured"));
        return false;
    }
    port_.setPortName(portName_);
    // Baud rate / data bits don't matter for pure DTR/RTS control,
    // but the port must be opened with some valid config.
    port_.setBaudRate(QSerialPort::Baud9600);
    port_.setDataBits(QSerialPort::Data8);
    port_.setParity(QSerialPort::NoParity);
    port_.setStopBits(QSerialPort::OneStop);
    port_.setFlowControl(QSerialPort::NoFlowControl);

    if (!port_.open(QIODevice::ReadWrite)) {
        emit pttError(QStringLiteral("Serial PTT: cannot open %1: %2")
                      .arg(portName_, port_.errorString()));
        return false;
    }
    // Start with PTT off
    if (line_ == Line::DTR) port_.setDataTerminalReady(false);
    else                    port_.setRequestToSend(false);

    qDebug() << "SerialPtt: opened" << portName_
             << (line_ == Line::DTR ? "DTR" : "RTS");
    return true;
}

void SerialPtt::close()
{
    if (!port_.isOpen()) return;
    // Release PTT before closing
    if (line_ == Line::DTR) port_.setDataTerminalReady(false);
    else                    port_.setRequestToSend(false);
    port_.close();
    qDebug() << "SerialPtt: closed" << portName_;
}

bool SerialPtt::isOpen() const
{
    return port_.isOpen();
}

void SerialPtt::setPtt(bool tx)
{
    if (!port_.isOpen()) {
        if (!open()) return;   // lazy open; error already emitted
    }
    const bool ok = (line_ == Line::DTR)
        ? port_.setDataTerminalReady(tx)
        : port_.setRequestToSend(tx);

    if (!ok) {
        emit pttError(QStringLiteral("Serial PTT: %1 set failed on %2: %3")
                      .arg(line_ == Line::DTR ? "DTR" : "RTS",
                           portName_, port_.errorString()));
    }
    qDebug() << "SerialPtt:" << (line_ == Line::DTR ? "DTR" : "RTS")
             << (tx ? "ON" : "OFF") << "on" << portName_;
}

QStringList SerialPtt::availablePorts()
{
    QStringList list;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        list << info.portName();
    }
    return list;
}
