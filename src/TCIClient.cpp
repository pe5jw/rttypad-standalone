#include "TCIClient.h"

#include <QAbstractSocket>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <QStringList>

namespace {
constexpr quint32 kRxAudio = 1;
constexpr quint32 kFloat32 = 3;
constexpr qsizetype kHeaderSize = 64;

quint32 readU32LE(const char *p) {
  const auto *b = reinterpret_cast<const unsigned char *>(p);
  return quint32(b[0]) | (quint32(b[1]) << 8) |
         (quint32(b[2]) << 16) | (quint32(b[3]) << 24);
}

float readFloat32LE(const char *p) {
  const quint32 bits = readU32LE(p);
  float value;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
} // namespace

TCIClient::TCIClient(QObject *parent) : QObject(parent) {
  connect(&socket_, &QWebSocket::connected, this, [this] {
    emit logLine(QStringLiteral("TCI connected"));
    emit connected();
    sendInitialTCICommands();
  });
  connect(&socket_, &QWebSocket::textMessageReceived, this, &TCIClient::textMessageReceived);
  connect(&socket_, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray & data) {
    const TCIStreamInfo info = parseStreamInfo(data);
    emit binaryMessageReceived(data.size(), info);
    if (info.isRxAudioFloat32) {
      emit rxAudioFrameReceived(data, info);
    }
    if (info.isIQFloat32) emit iqFrameReceived(data, info);
  });
  connect(&socket_, &QWebSocket::disconnected, this, [this] {
    // Notify consumers on both intentional and unexpected disconnects.
    // Otherwise TX and UI state can remain stuck in Sending/Draining.
    emit disconnected(intentionallyClosed_ ? QString() : socket_.errorString());
  });
  connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
    emit logLine(QStringLiteral("TCI socket error: %1").arg(socket_.errorString()));
  });
}

bool TCIClient::isConnected() const {
  return socket_.state() == QAbstractSocket::ConnectedState;
}

void TCIClient::connectToHost(const QString &host, quint16 port) {
  if (socket_.state() != QAbstractSocket::UnconnectedState) {
    ++connectionGeneration_;
    socket_.abort();
  }
  intentionallyClosed_ = false;
  QString hostname = host.trimmed();
  if (hostname.isEmpty()) {
    hostname = QStringLiteral("127.0.0.1");
  }
  QUrl url;
  url.setScheme(QStringLiteral("ws"));
  url.setHost(hostname);
  url.setPort(port);
  if (!url.isValid() || hostname.isEmpty() || port == 0) {
    emit logLine(QStringLiteral("Invalid TCI endpoint"));
    return;
  }
  ++connectionGeneration_;
  emit logLine(QStringLiteral("Connecting to %1").arg(url.toString()));
  socket_.open(url);
}

void TCIClient::disconnectFromHost() {
  intentionallyClosed_ = true;
  ++connectionGeneration_;
  if (isConnected()) {
    sendText(QStringLiteral("iq_stop:0;"));
    sendText(QStringLiteral("audio_stop:0;"));
    socket_.close(QWebSocketProtocol::CloseCodeNormal);
  } else {
    socket_.abort();
    // An in-progress connection may not emit disconnected() on abort.
    emit disconnected(QString());
  }
}

void TCIClient::sendText(const QString &text) {
  if (!isConnected()) {
    return;
  }
  emit logLine(QStringLiteral("TX TEXT: %1").arg(text));
  socket_.sendTextMessage(text);
}

void TCIClient::sendBinaryData(const QByteArray &data) {
  if (isConnected() && !data.isEmpty()) {
    socket_.sendBinaryMessage(data);
  }
}

void TCIClient::sendInitialTCICommands() {
  const quint64 generation = connectionGeneration_;

  // deskHPSDR only: native RTTY engine must be enabled explicitly.
  // Other TCI servers (SDRoxide, Bolt, Thetis, …) do not understand this
  // command and may reject the session if it is sent.
  if (dialect_ == Dialect::DeskHPSDR) {
    sendText(QStringLiteral("rtty_enable:1;"));
  }

  // Standard TCI: request RX audio and IQ streams.
  const QStringList commands = {
    QStringLiteral("audio_stream_sample_type:float32;"),
    QStringLiteral("audio_stream_channels:1;"),
    QStringLiteral("audio_stream_samples;"),
    QStringLiteral("audio_start:0;"),
    QStringLiteral("iq_start:0;")
  };
  for (int i = 0; i < commands.size(); ++i) {
    QTimer::singleShot((i + 1) * 50, this, [this, generation, cmd = commands.at(i)] {
      if (generation == connectionGeneration_ && isConnected())
        sendText(cmd);
    });
  }
}

TCIStreamInfo TCIClient::parseStreamInfo(const QByteArray &data) {
  TCIStreamInfo info;
  info.frameLength = data.size();
  if (data.size() < kHeaderSize) {
    return info;
  }
  const char *p = data.constData();
  info.receiver = readU32LE(p + 0);
  info.sampleRate = readU32LE(p + 4);
  info.format = readU32LE(p + 8);
  info.codec = readU32LE(p + 12);
  info.crc = readU32LE(p + 16);
  info.length = readU32LE(p + 20);
  info.type = readU32LE(p + 24);
  info.channels = readU32LE(p + 28);
  info.payloadOffset = kHeaderSize;
  info.payloadLength = data.size() - kHeaderSize;
  quint32 bytesPerSample = 0;
  switch (info.format) {
  case 0:
    bytesPerSample = 2;
    break;
  case 1:
    bytesPerSample = 3;
    break;
  case 2:
    bytesPerSample = 4;
    break;
  case 3:
    bytesPerSample = 4;
    break;
  default:
    break;
  }
  info.expectedPayloadLength = quint64(info.length) * bytesPerSample;
  info.payloadSizeMatches = info.expectedPayloadLength > 0 &&
                            quint64(info.payloadLength) >= info.expectedPayloadLength;
  info.valid = info.channels > 0 && info.length > 0 && info.payloadSizeMatches;
  info.audioFrames = info.channels ? info.length / info.channels : 0;
  info.isIQFloat32 = info.valid && info.type == 0 && info.format == kFloat32 && info.channels == 2;
  info.isRxAudioFloat32 = info.valid && info.type == kRxAudio &&
                          info.format == kFloat32 && info.audioFrames > 0;
  if (info.isRxAudioFloat32) {
    const quint32 frames = std::min(info.audioFrames, quint32(256));
    for (quint32 i = 0; i < frames; ++i) {
      const quint64 base = quint64(i) * info.channels * sizeof(float);
      const float left = readFloat32LE(p + kHeaderSize + base);
      const float right = info.channels > 1
                          ? readFloat32LE(p + kHeaderSize + base + sizeof(float)) : left;
      if (std::isfinite(left)) {
        info.peakLeft = std::max(info.peakLeft, std::fabs(left));
      }
      if (std::isfinite(right)) {
        info.peakRight = std::max(info.peakRight, std::fabs(right));
      }
    }
  }
  return info;
}
