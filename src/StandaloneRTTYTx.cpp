/* StandaloneRTTYTx.cpp
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* miniaudio — only device/playback, no codec */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_GENERATION
/* NOTE: MINIAUDIO_IMPLEMENTATION must appear exactly once per binary.
 * It is defined in rtty_engine_standalone.c — do NOT define it here.  */
#include "miniaudio.h"

#include "StandaloneRTTYTx.h"

#include <QMetaObject>
#include <QDebug>
#include <cstring>
#include <new>

StandaloneRTTYTx *StandaloneRTTYTx::s_instance = nullptr;

/* ------------------------------------------------------------------ */
/*  Construction / destruction                                          */
/* ------------------------------------------------------------------ */

StandaloneRTTYTx::StandaloneRTTYTx(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
    rttye_init();
    rttye_set_buffer_empty_callback(&StandaloneRTTYTx::onBufferEmpty);
    rttye_set_tx_done_callback(&StandaloneRTTYTx::onTxDone);
    m_device = new (std::nothrow) ma_device;
}

StandaloneRTTYTx::~StandaloneRTTYTx()
{
    abort();
    closeDevice();
    rttye_destroy();
    delete m_device;
    m_device = nullptr;
    if (s_instance == this) s_instance = nullptr;
}

/* ------------------------------------------------------------------ */
/*  Config                                                              */
/* ------------------------------------------------------------------ */

void StandaloneRTTYTx::setBaud(double b)          { rttye_set_baud(b); }
void StandaloneRTTYTx::setShift(int hz)           { rttye_set_shift(hz); }
void StandaloneRTTYTx::setReverse(bool r)         { rttye_set_reverse(r ? 1 : 0); }
void StandaloneRTTYTx::setStopBits(double sb)     { rttye_set_stop_bits(sb); }
void StandaloneRTTYTx::setUOS(bool uos)           { rttye_set_uos(uos ? 1 : 0); }
void StandaloneRTTYTx::setFillMode(RTTY_FILL_MODE m){ rttye_set_fill(m); }
void StandaloneRTTYTx::setStartCRLF(bool e)       { rttye_set_start_crlf(e ? 1 : 0); }
void StandaloneRTTYTx::setIdleTimeout(int s)      { rttye_set_idle_timeout(s); }
void StandaloneRTTYTx::setAfCenter(double hz)     { rttye_set_af_center(hz); }
void StandaloneRTTYTx::setSideband(RTTY_SIDEBAND sb){ rttye_set_sideband(sb); }
void StandaloneRTTYTx::setSampleRate(double sr)   { m_sampleRate = sr; }

/* ------------------------------------------------------------------ */
/*  Device listing                                                      */
/* ------------------------------------------------------------------ */

QStringList StandaloneRTTYTx::availableOutputDevices()
{
    QStringList list;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return list;

    ma_device_info *playback = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; i++)
            list << QString("[%1] %2").arg(i).arg(playback[i].name);
    }
    ma_context_uninit(&ctx);
    return list;
}

bool StandaloneRTTYTx::setOutputDevice(int idx)
{
    m_devIndex = idx;
    return true;
}

/* ------------------------------------------------------------------ */
/*  miniaudio device                                                    */
/* ------------------------------------------------------------------ */

bool StandaloneRTTYTx::openDevice()
{
    if (m_deviceOpen) return true;
    if (!m_device) return false;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 1;           /* mono AFSK */
    cfg.sampleRate        = (ma_uint32)m_sampleRate;
    cfg.dataCallback      = &StandaloneRTTYTx::maDataCallback;
    cfg.pUserData         = this;

    /* Specific device by index, or NULL for system default */
    if (m_devIndex >= 0) {
        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
            ma_device_info *pbi = nullptr;
            ma_uint32 cnt = 0;
            if (ma_context_get_devices(&ctx, &pbi, &cnt,
                                        nullptr, nullptr) == MA_SUCCESS
                && (ma_uint32)m_devIndex < cnt) {
                cfg.playback.pDeviceID = &pbi[m_devIndex].id;
            }
            ma_context_uninit(&ctx);
        }
    }

    if (ma_device_init(nullptr, &cfg, m_device) != MA_SUCCESS) {
        emit audioError("miniaudio: ma_device_init failed");
        return false;
    }
    if (ma_device_start(m_device) != MA_SUCCESS) {
        emit audioError("miniaudio: ma_device_start failed");
        ma_device_uninit(m_device);
        return false;
    }
    m_deviceOpen = true;
    return true;
}

void StandaloneRTTYTx::closeDevice()
{
    if (!m_deviceOpen || !m_device) return;
    ma_device_stop(m_device);
    ma_device_uninit(m_device);
    m_deviceOpen = false;
}

/* miniaudio audio thread callback */
void StandaloneRTTYTx::maDataCallback(ma_device *dev, void *out,
                                       const void * /*in*/, ma_uint32 frames)
{
    auto *self = static_cast<StandaloneRTTYTx *>(dev->pUserData);
    float *buf = static_cast<float *>(out);
    int sr = (int)dev->sampleRate;
    rttye_render(buf, (int)frames, sr, 1 /* mono */);
    (void)self;
}

/* ------------------------------------------------------------------ */
/*  Control — mirrors NativeRTTYTx interface                            */
/* ------------------------------------------------------------------ */

bool StandaloneRTTYTx::start()
{
    if (!openDevice()) return false;
    if (rttye_start() == 0) {
        m_bufferEmpty = true;
        setState(State::Idle);
        return true;
    }
    return false;
}

bool StandaloneRTTYTx::send(const QString &text)
{
    if (m_state == State::Draining) return false;
    if (!m_deviceOpen) openDevice();

    const QByteArray utf8 = text.toUtf8();
    int queued = rttye_queue_text(utf8.constData());
    if (queued <= 0) return false;

    m_bufferEmpty = false;
    setState(State::Sending);
    return true;
}

bool StandaloneRTTYTx::drain()
{
    if (m_state != State::Sending) return false;
    rttye_drain();
    setState(State::Draining);
    return true;
}

bool StandaloneRTTYTx::stop()
{
    if (m_state == State::Idle) return true;
    rttye_stop();
    m_bufferEmpty = false;
    setState(State::Draining);
    return true;
}

void StandaloneRTTYTx::abort()
{
    rttye_abort();
    m_bufferEmpty = true;
    setState(State::Idle);
}

bool StandaloneRTTYTx::isActive() const
{
    return rttye_is_active() != 0;
}

void StandaloneRTTYTx::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

/* ------------------------------------------------------------------ */
/*  Engine callbacks — audio thread → Qt main thread                   */
/* ------------------------------------------------------------------ */

void StandaloneRTTYTx::onBufferEmpty()
{
    if (!s_instance) return;
    QMetaObject::invokeMethod(s_instance, []{
        if (!s_instance) return;
        s_instance->m_bufferEmpty = true;
        if (s_instance->m_state == State::Draining)
            emit s_instance->drainBufferEmpty();
        emit s_instance->bufferEmptyReceived();
    }, Qt::QueuedConnection);
}

void StandaloneRTTYTx::onTxDone()
{
    if (!s_instance) return;
    QMetaObject::invokeMethod(s_instance, []{
        if (!s_instance) return;
        s_instance->m_bufferEmpty = true;
        s_instance->setState(State::Idle);
    }, Qt::QueuedConnection);
}
