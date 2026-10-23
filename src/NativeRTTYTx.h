#pragma once

#include <QObject>
#include <QString>

class TCIClient;
class RigctlPtt;
class SerialPtt;
class StandaloneRTTYTx;

// RTTY TX controller — standalone AFSK via miniaudio + configurable PTT.
class NativeRTTYTx final : public QObject {
  Q_OBJECT
public:
  enum class State { Disconnected, Idle, Sending, Draining };

  /* PTT method:
   *   None      — no PTT (VOX or manual)
   *   TCI       — trx:0,true/false via TCI WebSocket
   *   SerialDTR — DTR line on a COM port (QSerialPort, no daemon)
   *   SerialRTS — RTS line on a COM port (QSerialPort, no daemon)
   *   Rigctld   — T 1 / T 0 via rigctld TCP (Hamlib daemon)
   */
  enum class PttMode {
    None,       // geen PTT (VOX of handmatig)
    TCI,        // trx:0,true/false via TCI WebSocket
    SerialDTR,  // DTR lijn op COM poort (QSerialPort, geen daemon)
    SerialRTS,  // RTS lijn op COM poort (QSerialPort, geen daemon)
    Rigctld,    // T 1 / T 0 via rigctld TCP (Hamlib daemon)
  };

  explicit NativeRTTYTx(TCIClient *client, QObject *parent = nullptr);
  ~NativeRTTYTx() override;

  /* ---- State --------------------------------------------------- */
  State state()       const { return state_; }
  bool  bufferEmpty() const { return bufferEmpty_; }

  /* ---- PTT ----------------------------------------------------- */
  void setPttMode(PttMode mode) { pttMode_ = mode; }
  PttMode pttMode() const { return pttMode_; }

  /* TCI PTT suffix — some servers need ",tci" to route audio to TX */
  void setTciPttSuffix(const QString &suffix) { tciPttSuffix_ = suffix; }

  /* COM port name for SerialDTR / SerialRTS / TciAndDTR / TciAndRTS */
  void setSerialPttPort(const QString &portName);

  /* rigctld host/port for Rigctld / TciAndRigctl PTT */
  void setRigctlHost(const QString &host);
  void setRigctlPort(int port);

  /* ---- Audio engine -------------------------------------------- */
  StandaloneRTTYTx *standaloneEngine() const { return standalone_; }

  /* ---- Unified control ----------------------------------------- */
  bool send(const QString &text);
  bool start();
  bool drain();
  bool stop();

signals:
  void stateChanged(State state);
  void bufferEmptyReceived();
  void drainBufferEmpty();
  void pttError(const QString &msg);

private:
  void setState(State state);
  void raisePtt();
  void dropPtt();

  TCIClient        *client_;
  RigctlPtt        *rigctl_     = nullptr;
  SerialPtt        *serialPtt_  = nullptr;
  PttMode           pttMode_    = PttMode::TCI;
  State             state_      = State::Disconnected;
  bool              bufferEmpty_ = true;
  QString           tciPttSuffix_;

  StandaloneRTTYTx *standalone_ = nullptr;
};
