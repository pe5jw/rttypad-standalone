/* SerialPtt.h
 * PTT control via serial port DTR or RTS line (no daemon needed).
 * Works directly with USB-serial adapters (CP2102, CH340, FTDI, …).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QSerialPort>

class SerialPtt : public QObject {
    Q_OBJECT
public:
    enum class Line { DTR, RTS };

    explicit SerialPtt(QObject *parent = nullptr);
    ~SerialPtt() override;

    void setPortName(const QString &port) { portName_ = port; }
    void setLine(Line line)               { line_ = line; }
    QString portName() const { return portName_; }
    Line    line()     const { return line_; }

    /* Open the port (done lazily on first setPtt call if not called explicitly) */
    bool open();
    void close();
    bool isOpen() const;

    void setPtt(bool tx);

    /* List available serial port names */
    static QStringList availablePorts();

signals:
    void pttError(const QString &msg);

private:
    QString      portName_;
    Line         line_ = Line::DTR;
    QSerialPort  port_;
};
