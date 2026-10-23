/* StandaloneRTTYRx.h
 * miniaudio capture → RTTY RX decoder feed.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

struct ma_device;

class StandaloneRTTYRx : public QObject
{
    Q_OBJECT
public:
    /* Callback: called on the audio thread with mono float32 samples */
    using AudioCallback = std::function<void(const float *samples, int count, double sampleRate)>;

    explicit StandaloneRTTYRx(QObject *parent = nullptr);
    ~StandaloneRTTYRx() override;

    static QStringList availableInputDevices();  /* "[idx] Name" */
    bool setInputDevice(int deviceIndex);         /* -1 = default */
    void setSampleRate(double sr);
    void setAudioCallback(AudioCallback cb);

    bool start();
    void stop();
    bool isRunning() const { return m_running; }

signals:
    void audioError(const QString &msg);

private:
    static void maDataCallback(ma_device *dev, void *out,
                               const void *in, unsigned int frames);

    ma_device      *m_device     = nullptr;
    int             m_devIndex   = -1;
    double          m_sampleRate = 48000.0;
    bool            m_running    = false;
    AudioCallback   m_callback;
};
