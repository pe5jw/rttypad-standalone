#pragma once

#include <QMainWindow>
#include "TCIClient.h"
#include "NativeRTTYTx.h"
#include "RTTYRx.h"
#include <vector>

class StandaloneRTTYRx;

class QLabel;
class RTTYScopeWidget;
class RTTYIQPanWidget;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QSpinBox;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;

class MainWindow final : public QMainWindow {
public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

private:
  enum class DecodePickTarget { Call, Name };
  DecodePickTarget decodePickTarget_ = DecodePickTarget::Call;
  void updateConnectionUi(bool connected);
  void configureRx();
  void updateScope();
  void updateAfc(const float *samples, size_t count, double sampleRate, bool carrier);
  void updateTuningIndicators(const RTTY_RX_SCOPE &scope);
  void processTciParameters(const QString &message);
  void applyTciPitch();
  void beginRttySession();
  void maybeStartRttySession();
  int tciBandForFrequency(double hz) const;
  void applyPendingBandAutoSetup();
  void disconnectRestoringSession();
  double rxCenterHz() const;
  bool rxReverse() const;
  void updateRxStatus();
  void processRxAudio(const QByteArray &data, const TCIStreamInfo &info);
  void processRxSamples(const float *buf, int frames, double sampleRate, bool isUsb);
  TCIClient tci_;
  std::vector<float> tmpSamples_;
  NativeRTTYTx tx_{&tci_};
  void updateTxUi();
  void appendTxTraffic(const QString &text);
  void appendMonitorLogText(const QString &text);
  void sendNativeTxParameters();
  void runMacro(int index);
  void editMacro(int index);
  void refreshMacroButtons();
  void editStationSettings();
  void logQsoAdif();
  void logContestQso();
  void updateContestUi();
  void showContestLog();
  void exportContestCabrillo();
  QString contestLogPath() const;
  QString contestBand() const;
  QPushButton *macroButtons_[8] {};
  RTTY_RX *rx_ = nullptr;
  quint32 rxSampleRate_ = 0;
  std::vector<float> samples_;
  StandaloneRTTYRx *standaloneRx_ = nullptr;
  void startStandaloneRx();
  void stopStandaloneRx();
  void openAudioSettingsDialog();
  void openPttSettingsDialog();
  void openTciSettingsDialog();
  void applyConnectionMode(bool tciMode);  // true = TCI, false = audio/soundcard
  void applyTciDialect(TCIClient::Dialect dialect);
  void connectTciTxAudio();   // wire txAudioBlock → tci_.sendTxAudioFrame
  bool tciConnMode_ = true;  // true = TCI connection, false = soundcard audio
  QLineEdit *host_ = nullptr;
  QLineEdit *port_ = nullptr;
  QPushButton *connectButton_ = nullptr;
  QLabel *connectionLabel_ = nullptr;
  QLabel *audioLabel_ = nullptr;
  QLabel *scopeLabel_ = nullptr;
  QLabel *afcLabel_ = nullptr;
  QLabel *markLed_ = nullptr;
  QLabel *spaceLed_ = nullptr;
  double afcErrorHz_ = 0.0;
  bool afcValid_ = false;
  RTTYScopeWidget *scopeWidget_ = nullptr;
  RTTYIQPanWidget *iqPan_ = nullptr;
  double iqDragStartFrequencyHz_ = 0.0;
  QDoubleSpinBox *center_ = nullptr;
  QDoubleSpinBox *shift_ = nullptr;
  QDoubleSpinBox *baud_ = nullptr;
  QCheckBox *reverse_ = nullptr;
  QCheckBox *strictLock_ = nullptr;
  int strictGateBlocks_ = 0;
  int noCarrierBlocks_ = 0;
  bool decodedLocked_ = false;
  QString pendingDecoded_;
  QSpinBox *sql_ = nullptr;
  QCheckBox *contestMode_ = nullptr;
  QComboBox *contestType_ = nullptr;
  QLineEdit *contestExchange_ = nullptr;
  QSpinBox *contestSerial_ = nullptr;
  QLabel *dupeLabel_ = nullptr;
  QLineEdit *qsoCall_ = nullptr;
  QLineEdit *qsoName_ = nullptr;
  QLineEdit *qsoSent_ = nullptr;
  QLineEdit *qsoReceived_ = nullptr;
  QPlainTextEdit *log_ = nullptr;
  QPlainTextEdit *decoder_ = nullptr;
  QCheckBox *writeLog_ = nullptr;
  QString monitorLogActivePath_;
  bool monitorLogHeaderWritten_ = false;
  QPlainTextEdit *txInput_ = nullptr;
  bool txEchoLineActive_ = false;
  int liveTxCommitted_ = 0;
  bool liveTxActive_ = false;
  bool txOffDraining_ = false;
  QLabel *txStatus_ = nullptr;
  QComboBox *txFill_ = nullptr;
  QComboBox *txStopBits_ = nullptr;
  QCheckBox *txUos_ = nullptr;
  QCheckBox *txStartCrlf_ = nullptr;
  QSpinBox *idleTimeout_ = nullptr;
  QPushButton *sendButton_ = nullptr;
  QPushButton *drainButton_ = nullptr;
  QPushButton *stopButton_ = nullptr;
  quint64 rxFrames_ = 0;
  QString tciMode_ = QStringLiteral("DIGL");
  QString originalMode_;
  double originalDiglOffsetHz_ = 0.0;
  double originalDiguOffsetHz_ = 0.0;
  bool snapshotModeReady_ = false;
  bool snapshotDiglReady_ = false;
  bool snapshotDiguReady_ = false;
  bool autoSetupSession_ = true;
  bool autoSetupBandChangeSession_ = true;
  int lastAutoSetupBand_ = 0;
  bool pendingAutoSetupBand_ = false;

  bool sessionStarted_ = false;
  double requestedRttyPitchHz_ = 2210.0;
  bool sessionChangedMode_ = false;
  bool sessionChangedDigl_ = false;
  bool sessionChangedDigu_ = false;
  bool sessionRestoring_ = false;
  bool closingAfterRestore_ = false;
  quint64 sessionGeneration_ = 0;
  double diglOffsetHz_ = 2210.0;
  double diguOffsetHz_ = 1500.0;
  double tciFrequencyHz_ = 0.0;
  double tciRxDbm_ = 0.0;
  bool tciRxDbmValid_ = false;
  QLabel *rxStatus_ = nullptr;
};
