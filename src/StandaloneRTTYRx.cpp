/* StandaloneRTTYRx.cpp
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* miniaudio — capture only, no codec */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_GENERATION
/* MINIAUDIO_IMPLEMENTATION is defined in rtty_engine_standalone.c */
#include "miniaudio.h"

#include "StandaloneRTTYRx.h"
#include <QDebug>
#include <new>

StandaloneRTTYRx::StandaloneRTTYRx(QObject *parent)
    : QObject(parent)
{
    m_device = new (std::nothrow) ma_device;
}

StandaloneRTTYRx::~StandaloneRTTYRx()
{
    stop();
    delete m_device;
    m_device = nullptr;
}

QStringList StandaloneRTTYRx::availableInputDevices()
{
    QStringList list;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
        return list;

    ma_device_info *capture = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, nullptr, nullptr, &capture, &count) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; i++)
            list << QString("[%1] %2").arg(i).arg(capture[i].name);
    }
    ma_context_uninit(&ctx);
    return list;
}

bool StandaloneRTTYRx::setInputDevice(int idx)
{
    m_devIndex = idx;
    return true;
}

void StandaloneRTTYRx::setSampleRate(double sr)
{
    m_sampleRate = sr;
}

void StandaloneRTTYRx::setAudioCallback(AudioCallback cb)
{
    m_callback = std::move(cb);
}

bool StandaloneRTTYRx::start()
{
    if (m_running) return true;
    if (!m_device) return false;

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.sampleRate       = (ma_uint32)m_sampleRate;
    cfg.dataCallback     = &StandaloneRTTYRx::maDataCallback;
    cfg.pUserData        = this;

    if (m_devIndex >= 0) {
        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) == MA_SUCCESS) {
            ma_device_info *cap = nullptr;
            ma_uint32 cnt = 0;
            if (ma_context_get_devices(&ctx, nullptr, nullptr, &cap, &cnt) == MA_SUCCESS
                && (ma_uint32)m_devIndex < cnt) {
                cfg.capture.pDeviceID = &cap[m_devIndex].id;
            }
            ma_context_uninit(&ctx);
        }
    }

    if (ma_device_init(nullptr, &cfg, m_device) != MA_SUCCESS) {
        emit audioError("StandaloneRTTYRx: ma_device_init failed");
        return false;
    }
    if (ma_device_start(m_device) != MA_SUCCESS) {
        emit audioError("StandaloneRTTYRx: ma_device_start failed");
        ma_device_uninit(m_device);
        return false;
    }
    m_running = true;
    return true;
}

void StandaloneRTTYRx::stop()
{
    if (!m_running || !m_device) return;
    ma_device_stop(m_device);
    ma_device_uninit(m_device);
    m_running = false;
}

void StandaloneRTTYRx::maDataCallback(ma_device *dev, void * /*out*/,
                                       const void *in, unsigned int frames)
{
    auto *self = static_cast<StandaloneRTTYRx *>(dev->pUserData);
    if (self->m_callback && frames > 0)
        self->m_callback(static_cast<const float *>(in),
                         static_cast<int>(frames),
                         dev->sampleRate);
}
