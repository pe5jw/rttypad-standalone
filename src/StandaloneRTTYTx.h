/* StandaloneRTTYTx.h
 * Qt6 wrapper around rtty_engine_standalone + miniaudio output.
 * Mirrors the NativeRTTYTx interface so MainWindow can switch modes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <atomic>

/* Forward-declare miniaudio device to keep it out of the public header */
struct ma_device;

#include "rtty_engine_standalone.h"

class StandaloneRTTYTx : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Sending, Draining };

    explicit StandaloneRTTYTx(QObject *parent = nullptr);
    ~StandaloneRTTYTx() override;

    /* ---- Audio device ------------------------------------------- */
    static QStringList availableOutputDevices(); /* "[idx] Name" list */
    bool setOutputDevice(int deviceIndex);        /* -1 = system default */
    void setSampleRate(double sr);                /* default 48000 */

    // Enable/disable forwarding rendered TX audio via the txAudioBlock signal.
    // When enabled, each rendered audio block is also emitted as txAudioBlock
    // (QueuedConnection — safe to connect to main-thread slots like TCIClient).
    void setTciTxAudioEnabled(bool en) { m_tciTxEnabled.store(en); }

    /* ---- FSK config --------------------------------------------- */
    void setBaud(double baud);
    void setShift(int shiftHz);
    void setReverse(bool reverse);
    void setStopBits(double stopBits);
    void setUOS(bool uos);
    void setFillMode(RTTY_FILL_MODE mode);
    void setStartCRLF(bool enabled);
    void setIdleTimeout(int seconds);
    void setAfCenter(double hz);        /* AFSK centre tone, default 1500 Hz */
    void setSideband(RTTY_SIDEBAND sb); /* DIGL (LSB) or DIGU (USB) */

    /* ---- Control ------------------------------------------------- */
    bool send(const QString &text);     /* queue + auto-start */
    bool start();
    bool drain();
    bool stop();
    void abort();

    State state()       const { return m_state; }
    bool  isActive()    const;
    bool  bufferEmpty() const { return m_bufferEmpty; }

signals:
    void stateChanged(State state);
    void bufferEmptyReceived();
    void drainBufferEmpty();
    void audioError(const QString &msg);
    // Emitted from the audio thread when TCI TX audio tap is active.
    // Connect with Qt::QueuedConnection to process on the main thread.
    void txAudioBlock(QByteArray samples, quint32 sampleRate);

private:
    void setState(State s);
    bool openDevice();
    void closeDevice();

    /* miniaudio device (heap-allocated to avoid header pollution) */
    ma_device *m_device   = nullptr;
    int        m_devIndex = -1;
    double     m_sampleRate = 48000.0;
    bool       m_deviceOpen = false;

    State m_state       = State::Idle;
    bool  m_bufferEmpty = true;
    std::atomic<bool> m_tciTxEnabled{false};

    /* miniaudio data callback (static C shim) */
    static void maDataCallback(ma_device *dev, void *out,
                               const void *in, unsigned int frames);

    /* Engine callbacks — bounce to Qt main thread */
    static void onBufferEmpty();
    static void onTxDone();
    static StandaloneRTTYTx *s_instance;
};
