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
  // TCI dialect — controls which non-standard commands are sent on connect
  // and whether IQ streaming is attempted.
  //
  //  Auto       : detect from "device:" in the init block; fallback = Generic
  //  Generic    : plain TCI (ExpertSDR, Zeus, NereusSDR, Kainos)
  //               IQ stream ✓, RX/TX audio ✓, port 50001
  //  DeskHPSDR  : plain TCI + rtty_enable:1 on connect
  //               IQ stream ✓, RX/TX audio ✓, port 50001
  //  BoltSDR    : no IQ stream (binary stream omitted), RX audio only port 40001
  //  SDROxide   : no IQ stream; acts as TCI client+server; partial CAT pass-through
  enum class Dialect { Auto, Generic, DeskHPSDR, BoltSDR, SDROxide };

  explicit TCIClient(QObject *parent = nullptr);
  bool isConnected() const;
  void connectToHost(const QString &host, quint16 port);
  void disconnectFromHost();
  void sendText(const QString &text);
  void sendBinaryData(const QByteArray &data);

  void setDialect(Dialect d) { dialect_ = d; }
  Dialect dialect() const { return dialect_; }
  // Active dialect: if Auto, returns the resolved dialect (from device: header)
  // or Generic while not yet resolved.
  Dialect activeDialect() const { return resolvedDialect_; }

  bool isDeskHPSDR() const { return resolvedDialect_ == Dialect::DeskHPSDR; }
  // Returns true for dialects that do NOT support IQ streaming.
  bool hasNoIqStream() const {
    return resolvedDialect_ == Dialect::BoltSDR ||
           resolvedDialect_ == Dialect::SDROxide;
  }

  // Human-readable name for a dialect.
  static QString dialectName(Dialect d);
  // Resolve a "device:" string to a dialect (used for auto-detect).
  static Dialect detectDialect(const QString &deviceString);

  // Send a block of mono float32 PCM samples as a TCI TX audio stream frame.
  // sampleRate must match what the server expects (typically 8000 Hz for RTTY).
  void sendTxAudioFrame(const float *samples, quint32 count, quint32 sampleRate);

signals:
  void logLine(const QString &line);
  void connected();
  void disconnected(const QString &error);
  // Emitted when the active dialect changes (Auto resolves, or user changes it).
  void dialectResolved(TCIClient::Dialect dialect);
  void textMessageReceived(const QString &message);
  void binaryMessageReceived(qsizetype length, TCIStreamInfo info);
  void rxAudioFrameReceived(const QByteArray &data, TCIStreamInfo info);
  void iqFrameReceived(const QByteArray &data, TCIStreamInfo info);

private:
  static TCIStreamInfo parseStreamInfo(const QByteArray &data);
  void sendInitialTCICommands();
  void handleInitLine(const QString &line);
  QWebSocket socket_;
  bool intentionallyClosed_ = false;
  quint64 connectionGeneration_ = 0;
  Dialect dialect_ = Dialect::Auto;
  Dialect resolvedDialect_ = Dialect::Generic;  // effective dialect during session
  bool initDone_ = false;  // true after "ready" line received
};
