#include "NativeRTTYTx.h"
#include "TCIClient.h"
#include "RigctlPtt.h"
#include "SerialPtt.h"
#include "StandaloneRTTYTx.h"

NativeRTTYTx::NativeRTTYTx(TCIClient *client, QObject *parent)
  : QObject(parent), client_(client)
{
  rigctl_    = new RigctlPtt(this);
  serialPtt_ = new SerialPtt(this);

  connect(rigctl_,    &RigctlPtt::pttError, this, &NativeRTTYTx::pttError);
  connect(serialPtt_, &SerialPtt::pttError, this, &NativeRTTYTx::pttError);

  standalone_ = new StandaloneRTTYTx(this);
  connect(standalone_, &StandaloneRTTYTx::bufferEmptyReceived,
          this, &NativeRTTYTx::bufferEmptyReceived);
  connect(standalone_, &StandaloneRTTYTx::drainBufferEmpty,
          this, &NativeRTTYTx::drainBufferEmpty);
  connect(standalone_, &StandaloneRTTYTx::stateChanged,
          this, [this](StandaloneRTTYTx::State s) {
    switch (s) {
    case StandaloneRTTYTx::State::Idle:
      dropPtt();
      setState(State::Idle);
      break;
    case StandaloneRTTYTx::State::Sending:
      setState(State::Sending);
      break;
    case StandaloneRTTYTx::State::Draining:
      setState(State::Draining);
      break;
    }
  });
  connect(standalone_, &StandaloneRTTYTx::audioError,
          this, [](const QString &e){ qWarning() << "StandaloneRTTYTx:" << e; });

  connect(client_, &TCIClient::connected, this, [this] {
    bufferEmpty_ = true;
    setState(State::Idle);
  });
  connect(client_, &TCIClient::disconnected, this, [this](const QString &) {
    bufferEmpty_ = true;
    setState(State::Disconnected);
  });
}

NativeRTTYTx::~NativeRTTYTx() = default;

void NativeRTTYTx::setState(State state) {
  if (state_ == state) return;
  state_ = state;
  emit stateChanged(state_);
}

void NativeRTTYTx::setRigctlHost(const QString &host) { rigctl_->setHost(host); }
void NativeRTTYTx::setRigctlPort(int port)            { rigctl_->setPort(port); }

void NativeRTTYTx::setSerialPttPort(const QString &portName)
{
  // Close existing port before switching
  if (serialPtt_->isOpen()) serialPtt_->close();
  serialPtt_->setPortName(portName);
}

void NativeRTTYTx::raisePtt()
{
  switch (pttMode_) {
  case PttMode::TCI:
    if (client_->isConnected()) {
      client_->sendText(QStringLiteral("trx:0,true%1;").arg(tciPttSuffix_));
      qDebug() << "NativeRTTYTx: PTT ON via TCI";
    }
    break;
  case PttMode::SerialDTR:
    serialPtt_->setLine(SerialPtt::Line::DTR);
    serialPtt_->setPtt(true);
    qDebug() << "NativeRTTYTx: PTT ON via DTR";
    break;
  case PttMode::SerialRTS:
    serialPtt_->setLine(SerialPtt::Line::RTS);
    serialPtt_->setPtt(true);
    qDebug() << "NativeRTTYTx: PTT ON via RTS";
    break;
  case PttMode::Rigctld:
    rigctl_->setPtt(true);
    qDebug() << "NativeRTTYTx: PTT ON via rigctld";
    break;
  case PttMode::None:
    break;
  }
}

void NativeRTTYTx::dropPtt()
{
  switch (pttMode_) {
  case PttMode::TCI:
    if (client_->isConnected()) {
      client_->sendText(QStringLiteral("trx:0,false%1;").arg(tciPttSuffix_));
      qDebug() << "NativeRTTYTx: PTT OFF via TCI";
    }
    break;
  case PttMode::SerialDTR:
    serialPtt_->setLine(SerialPtt::Line::DTR);
    serialPtt_->setPtt(false);
    qDebug() << "NativeRTTYTx: PTT OFF via DTR";
    break;
  case PttMode::SerialRTS:
    serialPtt_->setLine(SerialPtt::Line::RTS);
    serialPtt_->setPtt(false);
    qDebug() << "NativeRTTYTx: PTT OFF via RTS";
    break;
  case PttMode::Rigctld:
    rigctl_->setPtt(false);
    qDebug() << "NativeRTTYTx: PTT OFF via rigctld";
    break;
  case PttMode::None:
    break;
  }
}

bool NativeRTTYTx::send(const QString &text)
{
  if (state_ == State::Idle)
    raisePtt();
  return standalone_->send(text);
}

bool NativeRTTYTx::start()  { return standalone_->start(); }

bool NativeRTTYTx::drain()  { return standalone_->drain(); }

bool NativeRTTYTx::stop()
{
  const bool ok = standalone_->stop();
  if (ok) dropPtt();
  return ok;
}
