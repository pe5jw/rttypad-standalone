#pragma once

#include <QObject>
#include <QString>

class TCIClient;
class RigctlPtt;

#ifdef HAVE_STANDALONE_TX
class StandaloneRTTYTx;
#endif

// Native FSK control — TCI mode (deskHPSDR) or Standalone mode (miniaudio AFSK + PTT).
class NativeRTTYTx final : public QObject {
  Q_OBJECT
public:
  enum class State { Disconnected, Idle, Sending, Draining };

  /* TX backend selection */
  enum class Mode { TCI, Standalone };

  /* PTT method for standalone AFSK mode:
   *   None         — no PTT (VOX or manual)
   *   TCI          — trx:0,true / trx:0,false via TCI WebSocket
   *   Serial       — T 1 / T 0 via rigctld (Hamlib TCP)
   *   TCIAndSerial — both simultaneously
   */
  enum class PttMode { None, TCI, Serial, TCIAndSerial };

  explicit NativeRTTYTx(TCIClient *client, QObject *parent = nullptr);
  ~NativeRTTYTx() override;

  /* ---- Mode switch --------------------------------------------- */
  void setMode(Mode mode);
  Mode mode() const { return mode_; }

  /* ---- TCI state (only meaningful in TCI mode) ----------------- */
  State state() const { return state_; }
  bool  bufferEmpty() const { return bufferEmpty_; }
  void  setNativeRttyConfirmed(bool confirmed) { nativeRttyConfirmed_ = confirmed; }

  /* ---- PTT (standalone mode only) ------------------------------ */
  void setPttMode(PttMode mode) { pttMode_ = mode; }
  PttMode pttMode() const { return pttMode_; }

  /* rigctld host/port for Serial PTT */
  void setRigctlHost(const QString &host);
  void setRigctlPort(int port);

  /* TCI PTT suffix — some servers need ",tci" to route audio to TX */
  void setTciPttSuffix(const QString &suffix) { tciPttSuffix_ = suffix; }

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
  void pttError(const QString &msg);

private:
  void setState(State state);
  void raisePtt();
  void dropPtt();
  static QString escapePayload(const QString &text);

  TCIClient  *client_;
  RigctlPtt  *rigctl_              = nullptr;
  Mode        mode_                = Mode::TCI;
  PttMode     pttMode_             = PttMode::TCI;
  State       state_               = State::Disconnected;
  bool        bufferEmpty_         = true;
  bool        nativeRttyConfirmed_ = false;
  QString     tciPttSuffix_;           // e.g. ",tci" for SDRoxide/Bolt

#ifdef HAVE_STANDALONE_TX
  StandaloneRTTYTx *standalone_ = nullptr;
#endif
};
