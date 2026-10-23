#pragma once

#include <QObject>
#include <QString>

class TCIClient;

#ifdef HAVE_STANDALONE_TX
class StandaloneRTTYTx;
#endif

// Native FSK control — TCI mode (deskHPSDR) or Standalone mode (miniaudio).
class NativeRTTYTx final : public QObject {
  Q_OBJECT
public:
  enum class State { Disconnected, Idle, Sending, Draining };

  /* TX backend selection */
  enum class Mode { TCI, Standalone };

  explicit NativeRTTYTx(TCIClient *client, QObject *parent = nullptr);
  ~NativeRTTYTx() override;

  /* ---- Mode switch --------------------------------------------- */
  void setMode(Mode mode);
  Mode mode() const { return mode_; }

  /* ---- TCI state (only meaningful in TCI mode) ----------------- */
  State state() const { return state_; }
  bool  bufferEmpty() const { return bufferEmpty_; }
  void  setNativeRttyConfirmed(bool confirmed) { nativeRttyConfirmed_ = confirmed; }

  /* ---- Unified control ----------------------------------------- */
  bool send(const QString &text);
  bool start();
  bool drain();
  bool stop();

#ifdef HAVE_STANDALONE_TX
  StandaloneRTTYTx *standaloneEngine() const { return standalone_; }
#endif

signals:
  void stateChanged(State state);
  void bufferEmptyReceived();
  void drainBufferEmpty();

private:
  void setState(State state);
  static QString escapePayload(const QString &text);

  TCIClient *client_;
  Mode  mode_                 = Mode::TCI;
  State state_                = State::Disconnected;
  bool  bufferEmpty_          = true;
  bool  nativeRttyConfirmed_  = false;

#ifdef HAVE_STANDALONE_TX
  StandaloneRTTYTx *standalone_ = nullptr;
#endif
};
