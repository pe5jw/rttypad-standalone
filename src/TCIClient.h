#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QWebSocket>
#include <QtGlobal>

struct TCIStreamInfo {
  bool valid = false;
  bool payloadSizeMatches = false;
  bool isRxAudioFloat32 = false;
  bool isIQFloat32 = false;
  quint32 receiver = 0;
  quint32 sampleRate = 0;
  quint32 format = 0;
  quint32 codec = 0;
  quint32 crc = 0;
  quint32 length = 0;
  quint32 type = 0;
  quint32 channels = 0;
  qsizetype frameLength = 0;
  qsizetype payloadLength = 0;
  quint64 expectedPayloadLength = 0;
  qsizetype payloadOffset = 0;
  quint32 audioFrames = 0;
  float peakLeft = 0.0f;
  float peakRight = 0.0f;
};
Q_DECLARE_METATYPE(TCIStreamInfo)

class TCIClient final : public QObject {
  Q_OBJECT
public:
  // TCI dialect — controls which non-standard commands are sent on connect.
  // "deskhpsdr" : sends rtty_enable:1 and rtty_* parameters (deskHPSDR extension)
  // anything else: plain TCI only (iq_start, audio_start, no rtty_ commands)
  enum class Dialect { Generic, DeskHPSDR };

  explicit TCIClient(QObject *parent = nullptr);
  bool isConnected() const;
  void connectToHost(const QString &host, quint16 port);
  void disconnectFromHost();
  void sendText(const QString &text);
  void sendBinaryData(const QByteArray &data);

  void setDialect(Dialect d) { dialect_ = d; }
  Dialect dialect() const { return dialect_; }
  bool isDeskHPSDR() const { return dialect_ == Dialect::DeskHPSDR; }

signals:
  void logLine(const QString &line);
  void connected();
  void disconnected(const QString &error);
  void textMessageReceived(const QString &message);
  void binaryMessageReceived(qsizetype length, TCIStreamInfo info);
  void rxAudioFrameReceived(const QByteArray &data, TCIStreamInfo info);
  void iqFrameReceived(const QByteArray &data, TCIStreamInfo info);

private:
  static TCIStreamInfo parseStreamInfo(const QByteArray &data);
  void sendInitialTCICommands();
  QWebSocket socket_;
  bool intentionallyClosed_ = false;
  quint64 connectionGeneration_ = 0;
  Dialect dialect_ = Dialect::Generic;
};
