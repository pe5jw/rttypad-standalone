#include "MainWindow.h"
#include "RTTYScopeWidget.h"
#include "RTTYIQPanWidget.h"
#include "StandaloneRTTYRx.h"
#include "StandaloneRTTYTx.h"

#include <QFormLayout>
#include <QFont>
#include <QDialog>
#include <QDateTime>
#include <QDate>
#include <QDateEdit>
#include <QTimeEdit>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUdpSocket>
#include <QHostAddress>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDialogButtonBox>
#include <QMenuBar>
#include <QInputDialog>
#include <QFileDialog>
#include <QTextStream>
#include <QScrollArea>
#include <QMessageBox>
#include <QApplication>
#include <QPixmap>
#include <QMenu>
#include <QActionGroup>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QSet>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QTimer>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSizePolicy>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QColor>
#include <QTextBlock>
#include <QTableWidget>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QMouseEvent>
#include <QEvent>
#include <QCloseEvent>
#include <functional>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace {
// DARC ShortRY: DA-DR identify German-issued callsigns.  A leading
// foreign operating prefix (e.g. F/DL1ABC) takes precedence; suffixes
// such as /P and /T do not change the country of operation.
bool isDarcGermanCall(const QString &rawCall) {
  const QString call = rawCall.trimmed().toUpper();
  const QStringList parts = call.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (parts.isEmpty()) { return false; }
  static const QRegularExpression german(QStringLiteral("^D[A-R][0-9]"));
  static const QRegularExpression foreignPrefix(QStringLiteral("^[A-Z0-9]{1,4}$"));
  if (parts.size() > 1 && foreignPrefix.match(parts.first()).hasMatch() &&
      !german.match(parts.first()).hasMatch() &&
      german.match(parts.at(1)).hasMatch()) { return false; }
  return german.match(parts.first()).hasMatch();
}

// WPX-style prefix estimate for the on-screen ShortRY score. The DARC
// adjudicator remains authoritative, especially for unusual portable calls.
QString shortRyPrefix(const QString &raw) {
  const QStringList parts = raw.trimmed().toUpper().split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (parts.isEmpty()) return {};
  static const QRegularExpression ordinary(QStringLiteral("^([A-Z0-9]*?[A-Z][A-Z0-9]*?[0-9])"));
  static const QRegularExpression operating(QStringLiteral("^[A-Z]{1,3}[0-9]?$"));
  static const QRegularExpression digit(QStringLiteral("^[0-9]$"));
  const QString base = parts.first();
  QString prefix;
  const auto match = ordinary.match(base);
  if (match.hasMatch()) { prefix = match.captured(1); }
  if (parts.size() > 1) {
    const QString suffix = parts.last();
    if (digit.match(suffix).hasMatch() && !prefix.isEmpty()) {
      prefix.chop(1);
      return prefix + suffix;
    }
    if (operating.match(suffix).hasMatch() && suffix != QStringLiteral("P") &&
        suffix != QStringLiteral("M") && suffix != QStringLiteral("T") &&
        suffix != QStringLiteral("AM") && suffix != QStringLiteral("MM") &&
        suffix != QStringLiteral("QRP")) {
      return suffix.back().isDigit() ? suffix : suffix + QStringLiteral("0");
    }
    if (operating.match(base).hasMatch() && !ordinary.match(base).hasMatch() &&
        ordinary.match(suffix).hasMatch()) {
      return base.back().isDigit() ? base : base + QStringLiteral("0");
    }
  }
  return prefix;
}

bool shortRyTrainee(const QString &raw) {
  const QString call = raw.trimmed().toUpper();
  static const QRegularExpression dn(QStringLiteral("^DN[1-8][A-Z]{1,4}(?:/.*)?$"));
  return dn.match(call).hasMatch() || call.endsWith(QStringLiteral("/T"));
}

struct ShortRyScore { int qsos = 0; int prefixes = 0; int doks = 0; };

ShortRyScore shortRyScore(const QString &path) {
  ShortRyScore score;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { return score; }
  QSet<QString> worked, prefixes, doks;
  while (!file.atEnd()) {
    const QList<QByteArray> row = file.readLine().trimmed().split('\t');
    if (row.size() < 10 || row.at(0) != "DARC-RTTY") { continue; }
    const QString band = QString::fromUtf8(row.at(3)).trimmed().toUpper();
    if (band != QStringLiteral("80M") && band != QStringLiteral("40M")) { continue; }
    const QString call = QString::fromUtf8(row.at(4)).trimmed().toUpper();
    if (call.isEmpty() || worked.contains(band + QLatin1Char(':') + call)) { continue; }
    worked.insert(band + QLatin1Char(':') + call);
    ++score.qsos;
    const QString prefix = shortRyPrefix(call);
    if (!prefix.isEmpty()) { prefixes.insert(band + QLatin1Char(':') + prefix); }
    const QString exchange = QString::fromUtf8(row.at(8)).trimmed().toUpper();
    if (isDarcGermanCall(call) && !exchange.isEmpty() && exchange != QStringLiteral("NM")) {
      doks.insert(band + QLatin1Char(':') + exchange);
    }
  }
  score.prefixes = prefixes.size();
  score.doks = doks.size();
  return score;
}

// Explicit operating location overrides callsign-based defaults.  The
// setting is intentionally shared by macro TX, QSO logging and Cabrillo.
bool darcOperatingInDl(const QString &stationCall) {
  const QSettings settings;
  const QString selected = settings.value(QStringLiteral("contest/darcLocation")).toString();
  if (selected == QStringLiteral("DL")) { return true; }
  if (selected == QStringLiteral("Non-DL")) { return false; }
  return isDarcGermanCall(stationCall);
}

// Transliterate Unicode to the 7-bit character repertoire used by RTTY/ITA2.
// Transliteration is applied before whitespace compaction and uppercasing.
QString rttyUpperAscii(const QString &value) {
  QString result;
  result.reserve(value.size());
  const QString normalized = value.normalized(QString::NormalizationForm_D);
  for (int i = 0; i < normalized.size(); ++i) {
    const QChar ch = normalized.at(i);
    if (i + 1 < normalized.size() && normalized.at(i + 1).unicode() == 0x0308) {
      const QChar base = ch.toUpper();
      if (base == QLatin1Char('A') || base == QLatin1Char('O') || base == QLatin1Char('U')) {
        result += base;
        result += QLatin1Char('E');
        ++i;
        continue;
      }
    }
    const ushort code = ch.unicode();
    // German umlauts must retain their conventional two-letter spelling.
    switch (code) {
    case 0x00C4:
    case 0x00E4:
      result += QStringLiteral("AE");
      continue;
    case 0x00D6:
    case 0x00F6:
      result += QStringLiteral("OE");
      continue;
    case 0x00DC:
    case 0x00FC:
      result += QStringLiteral("UE");
      continue;
    case 0x00DF:
    case 0x1E9E:
      result += QStringLiteral("SS");
      continue;
    case 0x00C6:
    case 0x00E6:
      result += QStringLiteral("AE");
      continue;
    case 0x0152:
    case 0x0153:
      result += QStringLiteral("OE");
      continue;
    case 0x00D8:
    case 0x00F8:
      result += QLatin1Char('O');
      continue;
    case 0x0141:
    case 0x0142:
      result += QLatin1Char('L');
      continue;
    case 0x00D0:
    case 0x00F0:
      result += QLatin1Char('D');
      continue;
    case 0x00DE:
    case 0x00FE:
      result += QStringLiteral("TH");
      continue;
    default:
      break;
    }
    // Unicode NFD combining accents: drop accents, preserve base letters.
    if (code >= 0x0300 && code <= 0x036F) { continue; }
    const QChar upper = ch.toUpper();
    if (upper.unicode() < 128) { result += upper; }
    else if (ch.isSpace()) { result += QLatin1Char(' '); }
    else { result += QLatin1Char(' '); } // Unsupported glyph cannot be sent via ITA2.
  }
  return result;
}

// Match rttyTCI field normalization: uppercase and collapse whitespace.
// Keep a trailing space during editing; trim edges only on editingFinished.
QString normalizedUpperField(const QString &value, bool trimEdges) {
  QString result;
  result.reserve(value.size());
  bool lastSpace = false;
  for (const QChar ch : rttyUpperAscii(value)) {
    if (ch.isSpace()) {
      if (!lastSpace) { result.append(QLatin1Char(' ')); }
      lastSpace = true;
    } else {
      result.append(ch);
      lastSpace = false;
    }
  }
  return trimEdges ? result.trimmed() : result;
}

void normalizeUpperEdit(QLineEdit *edit, bool compact = true) {
  auto normalize = [edit, compact](bool trimEdges) {
    const QString before = edit->text();
    const QString after = compact ? normalizedUpperField(before, trimEdges) : rttyUpperAscii(before);
    if (before == after) { return; }
    const int cursor = edit->cursorPosition();
    const int mapped = (compact ? normalizedUpperField(before.left(cursor), trimEdges)
                        : rttyUpperAscii(before.left(cursor))).size();
    const QSignalBlocker blocker(edit);
    edit->setText(after);
    edit->setCursorPosition(std::min(mapped, int(after.size())));
  };
  normalize(false);
  QObject::connect(edit, &QLineEdit::textEdited, edit, [normalize](const QString &) {
    normalize(false);
  });
  if (compact) QObject::connect(edit, &QLineEdit::editingFinished, edit, [normalize] {
    normalize(true);
  });
}

// Preserve the current TX selection when normalizing pasted or typed text.
void uppercaseTxEditor(QPlainTextEdit *edit) {
  QObject::connect(edit, &QPlainTextEdit::textChanged, edit, [edit] {
    const QString before = edit->toPlainText();
    const QString after = rttyUpperAscii(before);
    if (before == after) return;
    QTextCursor cursor = edit->textCursor();
    const int anchor = rttyUpperAscii(before.left(cursor.anchor())).size();
    const int position = rttyUpperAscii(before.left(cursor.position())).size();
    const QSignalBlocker blocker(edit);
    edit->setPlainText(after);
    cursor = edit->textCursor();
    cursor.setPosition(std::min(anchor, int(after.size())));
    cursor.setPosition(std::min(position, int(after.size())), QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
  });
}

class DecodePickView final : public QPlainTextEdit {
public:
  using QPlainTextEdit::QPlainTextEdit;
  std::function<void(const QString &)> onPicked;
protected:
  void mouseDoubleClickEvent(QMouseEvent *event) override {
    // Determine the word under the mouse, not the previous selection.
    QTextCursor cursor = cursorForPosition(event->pos());
    cursor.select(QTextCursor::WordUnderCursor);
    QString picked = cursor.selectedText().trimmed();
    // Preserve slash and hyphen inside callsigns when Qt's word
    // selection splits them into separate tokens.
    const QString line = cursor.block().text();
    const int column = cursorForPosition(event->pos()).positionInBlock();
    if (column >= 0 && column < line.size()) {
      auto isToken = [](QChar c) {
        return c.isLetterOrNumber() || c == QLatin1Char('/') || c == QLatin1Char('-');
      };
      int first = column;
      int last = column;
      if (!isToken(line.at(first)) && first > 0 && isToken(line.at(first - 1))) { --first; }
      if (isToken(line.at(first))) {
        last = first + 1;
        while (first > 0 && isToken(line.at(first - 1))) { --first; }
        while (last < line.size() && isToken(line.at(last))) { ++last; }
        picked = line.mid(first, last - first);
      }
    }
    QPlainTextEdit::mouseDoubleClickEvent(event);
    if (!picked.isEmpty() && onPicked) { onPicked(picked); }
  }
};

// Match the native AppKit CQ WW state/province validation.  DX is the
// Cabrillo placeholder for contacts outside continental USA/Canada.
bool validCQWWQthR(const QString &value) {
  static const QSet<QString> codes = {
    QStringLiteral("AL"), QStringLiteral("AK"), QStringLiteral("AZ"), QStringLiteral("AR"),
    QStringLiteral("CA"), QStringLiteral("CO"), QStringLiteral("CT"), QStringLiteral("DE"),
    QStringLiteral("FL"), QStringLiteral("GA"), QStringLiteral("HI"), QStringLiteral("ID"),
    QStringLiteral("IL"), QStringLiteral("IN"), QStringLiteral("IA"), QStringLiteral("KS"),
    QStringLiteral("KY"), QStringLiteral("LA"), QStringLiteral("ME"), QStringLiteral("MD"),
    QStringLiteral("MA"), QStringLiteral("MI"), QStringLiteral("MN"), QStringLiteral("MS"),
    QStringLiteral("MO"), QStringLiteral("MT"), QStringLiteral("NE"), QStringLiteral("NV"),
    QStringLiteral("NH"), QStringLiteral("NJ"), QStringLiteral("NM"), QStringLiteral("NY"),
    QStringLiteral("NC"), QStringLiteral("ND"), QStringLiteral("OH"), QStringLiteral("OK"),
    QStringLiteral("OR"), QStringLiteral("PA"), QStringLiteral("RI"), QStringLiteral("SC"),
    QStringLiteral("SD"), QStringLiteral("TN"), QStringLiteral("TX"), QStringLiteral("UT"),
    QStringLiteral("VT"), QStringLiteral("VA"), QStringLiteral("WA"), QStringLiteral("WV"),
    QStringLiteral("WI"), QStringLiteral("WY"), QStringLiteral("DC"), QStringLiteral("AB"),
    QStringLiteral("BC"), QStringLiteral("MB"), QStringLiteral("NB"), QStringLiteral("NL"),
    QStringLiteral("NS"), QStringLiteral("NT"), QStringLiteral("NU"), QStringLiteral("ON"),
    QStringLiteral("PE"), QStringLiteral("QC"), QStringLiteral("SK"), QStringLiteral("YT")
  };
  return value == QStringLiteral("DX") || codes.contains(value);
}

double tonePower(const float *samples, size_t count, double sampleRate, double frequency) {
  const double coefficient = 2.0 * std::cos(2.0 * 3.14159265358979323846 * frequency / sampleRate);
  double q1 = 0.0, q2 = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double q0 = coefficient * q1 - q2 + samples[i];
    q2 = q1;
    q1 = q0;
  }
  return std::max(0.0, q1 * q1 + q2 * q2 - coefficient * q1 * q2);
}

double localTonePeak(const float *samples, size_t count, double sampleRate,
                     double centerHz, double &level) {
  level = 0.0;
  double bestHz = 0.0;
  const double low = std::max(50.0, centerHz - 75.0);
  const double high = std::min(sampleRate * 0.45, centerHz + 75.0);
  for (double hz = low; hz <= high; hz += 5.0) {
    const double power = tonePower(samples, count, sampleRate, hz);
    if (power > level) { level = power; bestHz = hz; }
  }
  return bestHz;
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("rttyPad TCI by DL1BZ — DIGL — Disconnected"));
  resize(1380, 740);
  setMinimumSize(1380, 740);
  auto *setupMenu = menuBar()->addMenu(QStringLiteral("Setup"));
  // The compact in-window Setup button is retained. On macOS, expose
  // Help -> About in the native menu bar as well.
#ifdef Q_OS_MACOS
  menuBar()->setNativeMenuBar(true);
  auto *helpMenu = menuBar()->addMenu(QStringLiteral("Help"));
  auto *aboutMenuAction = helpMenu->addAction(QStringLiteral("About rttyPad…"));
  aboutMenuAction->setMenuRole(QAction::AboutRole);
#else
  menuBar()->hide();
#endif
  auto *central = new QWidget(this);
  central->setObjectName(QStringLiteral("rttyRoot"));
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(20, 12, 20, 10);
  layout->setSpacing(5);
  central->setStyleSheet(QStringLiteral(
                                 "QWidget#rttyRoot { background: #1c1f20; color: #e0e0e0; }"
                                 "QLineEdit, QDoubleSpinBox, QPlainTextEdit { background: #212121; color: #e0e0e0; border: 1px solid #2b3034; border-radius: 3px; }"
                                 "QPushButton { padding: 2px 8px; min-height: 17px; }"
                                 "QLabel { color: #e0e0e0; }"));
  // AppKit top line: connection and operating controls on one compact row.
  auto *connectionRow = new QHBoxLayout();
  connectionRow->setSpacing(6);
  host_ = new QLineEdit(central);
  host_->setObjectName(QStringLiteral("tciHost"));
  host_->setFixedWidth(150);
  host_->setText(QSettings().value(QStringLiteral("tci/host"), QStringLiteral("127.0.0.1")).toString());
  port_ = new QLineEdit(central);
  port_->setObjectName(QStringLiteral("tciPort"));
  port_->setValidator(new QIntValidator(1, 65535, port_));
  port_->setFixedWidth(72);
  port_->setText(QSettings().value(QStringLiteral("tci/port"), 50001).toString());
  connectButton_ = new QPushButton(QStringLiteral("Connect"), central);
  connectionLabel_ = new QLabel(QStringLiteral("Disconnected"), central);
  auto *setupButton = new QPushButton(QStringLiteral("Setup"), central);
  // Keep the compact Setup control, with decoder typography configurable.
  auto *setupPopup = new QMenu(setupButton);
  setupPopup->addAction(QStringLiteral("Macros…"), this, [this] { editMacro(0); });
  setupPopup->addAction(QStringLiteral("Station settings…"), this, [this] { editStationSettings(); });
  setupPopup->addSeparator();
  // ---- Verbindingsmodus: TCI of Audio (geluidskaart) ------------------
  // Dit is één keuze: TCI verbindt met de SDR app (IQ RX, binary TX, TCI PTT).
  // Audio gebruikt geluidskaart in/uit en serieel/rigctld PTT — geen WebSocket.
  {
    auto *modeGroup = new QActionGroup(setupPopup);
    modeGroup->setExclusive(true);
    auto *tciModeAct   = setupPopup->addAction(QStringLiteral("Verbinding: TCI"));
    auto *audioModeAct = setupPopup->addAction(QStringLiteral("Verbinding: Audio (geluidskaart)"));
    tciModeAct->setCheckable(true);
    audioModeAct->setCheckable(true);
    modeGroup->addAction(tciModeAct);
    modeGroup->addAction(audioModeAct);
    tciModeAct->setToolTip(QStringLiteral(
      "Verbind via TCI WebSocket met de SDR app.\n"
      "RX: IQ stream  TX: binary audio frames  PTT: trx:0,true/false"));
    audioModeAct->setToolTip(QStringLiteral(
      "Gebruik geluidskaart in/uit voor RX/TX.\n"
      "Geen TCI verbinding. PTT via COM poort (DTR/RTS) of rigctld."));

    // 0 = TCI (default), 1 = Audio
    const bool audioMode = QSettings().value(QStringLiteral("conn/mode"), 0).toInt() == 1;
    tciModeAct->setChecked(!audioMode);
    audioModeAct->setChecked(audioMode);
    tciConnMode_ = !audioMode;

    connect(modeGroup, &QActionGroup::triggered, this,
            [this, tciModeAct](QAction *) {
      const bool tci = tciModeAct->isChecked();
      QSettings().setValue(QStringLiteral("conn/mode"), tci ? 0 : 1);
      applyConnectionMode(tci);
    });

    setupPopup->addSeparator();

    // ---- PTT methode (alleen relevant in Audio-modus) ---------------
    // In TCI-modus is PTT altijd via TCI. In Audio-modus kies je hier.
    auto *pttMenu = setupPopup->addMenu(QStringLiteral("PTT methode"));
    auto *pttGroup = new QActionGroup(pttMenu);
    pttGroup->setExclusive(true);

    auto makePttAct = [&](const QString &label) {
      auto *a = pttMenu->addAction(label);
      a->setCheckable(true);
      pttGroup->addAction(a);
      return a;
    };
    auto *pttNoneAct    = makePttAct(QStringLiteral("Geen  (VOX / handmatig)"));
    auto *pttDtrAct     = makePttAct(QStringLiteral("DTR  (COM poort)"));
    auto *pttRtsAct     = makePttAct(QStringLiteral("RTS  (COM poort)"));
    auto *pttRigctlAct  = makePttAct(QStringLiteral("rigctld  (Hamlib TCP)"));
    auto *pttTciAct     = makePttAct(QStringLiteral("TCI  (trx:0,true/false)"));
    pttMenu->addSeparator();
    auto *pttCfgAct     = pttMenu->addAction(QStringLiteral("PTT instellingen…"));

    // PttMode opgeslagen als int, mapping:
    // 0=None 1=DTR 2=RTS 3=Rigctld 4=TCI
    using PM = NativeRTTYTx::PttMode;
    const int savedPtt = QSettings().value(QStringLiteral("tx/pttMode"), 4).toInt();
    PM initPtt = PM::TCI;
    auto setPttCheck = [&](int idx, PM mode, QAction *act) {
      if (savedPtt == idx) { act->setChecked(true); initPtt = mode; }
    };
    setPttCheck(0, PM::None,      pttNoneAct);
    setPttCheck(1, PM::SerialDTR, pttDtrAct);
    setPttCheck(2, PM::SerialRTS, pttRtsAct);
    setPttCheck(3, PM::Rigctld,   pttRigctlAct);
    setPttCheck(4, PM::TCI,       pttTciAct);
    if (!pttGroup->checkedAction()) pttTciAct->setChecked(true);

    tx_.setPttMode(initPtt);
    tx_.setRigctlHost(QSettings().value(QStringLiteral("tx/rigctlHost"),
                                         QStringLiteral("127.0.0.1")).toString());
    tx_.setRigctlPort(QSettings().value(QStringLiteral("tx/rigctlPort"), 4532).toInt());
    tx_.setSerialPttPort(QSettings().value(QStringLiteral("tx/serialPttPort"),
                                            QString()).toString());

    connect(pttGroup, &QActionGroup::triggered, this,
            [this, pttNoneAct, pttDtrAct, pttRtsAct, pttRigctlAct, pttTciAct](QAction *) {
      using PM = NativeRTTYTx::PttMode;
      PM m = PM::TCI; int idx = 4;
      if (pttNoneAct->isChecked())   { m = PM::None;      idx = 0; }
      if (pttDtrAct->isChecked())    { m = PM::SerialDTR; idx = 1; }
      if (pttRtsAct->isChecked())    { m = PM::SerialRTS; idx = 2; }
      if (pttRigctlAct->isChecked()) { m = PM::Rigctld;   idx = 3; }
      if (pttTciAct->isChecked())    { m = PM::TCI;       idx = 4; }
      QSettings().setValue(QStringLiteral("tx/pttMode"), idx);
      tx_.setPttMode(m);
    });

    connect(pttCfgAct, &QAction::triggered, this, [this] { openPttSettingsDialog(); });
    setupPopup->addAction(QStringLiteral("Audio apparaten…"), this,
                          [this] { openAudioSettingsDialog(); });
  }
  setupPopup->addSeparator();

  // Apply saved connection mode on startup (always call so engine state is initialised)
  {
    const bool audioMode = QSettings().value(QStringLiteral("conn/mode"), 0).toInt() == 1;
    QTimer::singleShot(0, this, [this, audioMode] { applyConnectionMode(!audioMode); });
  }

  // Apply saved TCI dialect (sets PTT suffix etc.)
  {
    const int dialectInt = QSettings().value(QStringLiteral("tci/dialect"),
                             static_cast<int>(TCIClient::Dialect::Auto)).toInt();
    applyTciDialect(static_cast<TCIClient::Dialect>(dialectInt));
  }

  // Wire TX audio tap → TCI binary frames (safe cross-thread via Qt queued)
  connectTciTxAudio();

  // Log auto-detect dialect changes to status bar
  connect(&tci_, &TCIClient::dialectResolved, this, [this](TCIClient::Dialect d) {
    statusBar()->showMessage(
      QStringLiteral("TCI dialect auto-detect: %1").arg(TCIClient::dialectName(d)), 5000);
    qDebug() << "TCI dialect resolved (auto):" << TCIClient::dialectName(d);
  });

  // ---- Settings TCI… dialoog ------------------------------------
  setupPopup->addAction(QStringLiteral("Settings TCI…"), this, [this] { openTciSettingsDialog(); });

  setupPopup->addSeparator();
  auto *sendLogAction = setupPopup->addAction(QStringLiteral("Send log via UDP"));
  sendLogAction->setCheckable(true);
  sendLogAction->setChecked(QSettings().value(QStringLiteral("log/sendViaUdp"), true).toBool());
  sendLogAction->setToolTip(
          QStringLiteral("Send ADIF to the configured UDP target after successful local save; disabled in Contest Mode."));
  connect(sendLogAction, &QAction::toggled, this, [](bool enabled) {
    QSettings().setValue(QStringLiteral("log/sendViaUdp"), enabled);
  });
  setupPopup->addAction(QStringLiteral("UDP Log Settings…"), this, [this, sendLogAction] {
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("UDP Log Settings"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *enabled = new QCheckBox(QStringLiteral("Send log via UDP"), &dialog);
    enabled->setChecked(settings.value(QStringLiteral("log/sendViaUdp"), true).toBool());
    auto *host = new QLineEdit(settings.value(QStringLiteral("log/udpHost"),
      QStringLiteral("127.0.0.1")).toString(), &dialog);
    auto *port = new QSpinBox(&dialog);
    port->setRange(1, 65535);
    port->setValue(settings.value(QStringLiteral("log/udpPort"), 12063).toInt());
    form->addRow(enabled);
    form->addRow(QStringLiteral("UDP Host"), host);
    form->addRow(QStringLiteral("UDP Port"), port);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(enabled, &QCheckBox::toggled, &dialog, [host, port](bool on) {
      host->setEnabled(on);
      port->setEnabled(on);
    });
    host->setEnabled(enabled->isChecked());
    port->setEnabled(enabled->isChecked());
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
      if (enabled->isChecked()) {
      QHostAddress address;
      if (!address.setAddress(host->text().trimmed()) ||
            address.protocol() != QAbstractSocket::IPv4Protocol) {
          QMessageBox::warning(&dialog, QStringLiteral("Invalid UDP target"),
                               QStringLiteral("Enter a valid IPv4 address."));
          return;
        }
      }
      dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    settings.setValue(QStringLiteral("log/sendViaUdp"), enabled->isChecked());
    settings.setValue(QStringLiteral("log/udpHost"), host->text().trimmed());
    settings.setValue(QStringLiteral("log/udpPort"), port->value());
    sendLogAction->setChecked(enabled->isChecked());
  });
  setupPopup->addSeparator();
  setupPopup->addAction(QStringLiteral("Decoder font size…"), this, [this] {
    const int current = QSettings().value(QStringLiteral("ui/decoderFontSize"), 11).toInt();
    bool accepted = false;
    const int size = QInputDialog::getInt(this, QStringLiteral("Decoder font size"),
                                          QStringLiteral("JetBrains Mono (pt):"),
                                          std::clamp(current, 8, 20), 8, 20, 1, &accepted);
    if (!accepted) return;
    QFont font(QStringLiteral("JetBrains Mono"));
    font.setPointSize(size);
    font.setWeight(QFont::Normal);
    font.setStyleHint(QFont::Monospace);
    decoder_->setFont(font);
#ifdef Q_OS_WIN
    if (txInput_) txInput_->setFont(font);
#endif
    QSettings().setValue(QStringLiteral("ui/decoderFontSize"), size);
  });
  setupPopup->addSeparator();
  auto showAbout = [this] {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("About rttyPad"));
    dialog.setModal(true);
    dialog.setMinimumWidth(330);

    auto *content = new QVBoxLayout(&dialog);
    content->setContentsMargins(26, 22, 26, 18);
    content->setSpacing(10);

    auto *icon = new QLabel(&dialog);
    const QPixmap logo(QStringLiteral(":/icons/AppIcon.png"));
    icon->setPixmap(logo.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    icon->setAlignment(Qt::AlignCenter);
    content->addWidget(icon);

    auto *name = new QLabel(QStringLiteral("<b>rttyPad TCI</b>"), &dialog);
    name->setAlignment(Qt::AlignCenter);
    content->addWidget(name);

    auto *version = new QLabel(QStringLiteral("Version %1").arg(QStringLiteral(RTTYPAD_VERSION)), &dialog);
    version->setAlignment(Qt::AlignCenter);
    content->addWidget(version);

    auto *description = new QLabel(
            QStringLiteral("RTTY Terminal and Contest Logger — TCI / Standalone"), &dialog);
    description->setAlignment(Qt::AlignCenter);
    description->setWordWrap(true);
    content->addWidget(description);

    auto *author = new QLabel(QStringLiteral("Developed by DL1BZ"), &dialog);
    author->setAlignment(Qt::AlignCenter);
    content->addWidget(author);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    content->addWidget(buttons);
    dialog.exec();
  };
  setupPopup->addAction(QStringLiteral("About rttyPad…"), this, showAbout);
#ifdef Q_OS_MACOS
  connect(aboutMenuAction, &QAction::triggered, this, showAbout);
#endif
  // Both entry points share the exact same QAction instances: identical
  // commands, check states and enabled states without duplicate handlers.
  for (QAction *action : setupPopup->actions()) {
#ifdef Q_OS_MACOS
    // macOS already exposes About in the native Help/application menu.
    if (action->text() == QStringLiteral("About rttyPad…")) { continue; }
#endif
    setupMenu->addAction(action);
  }
  setupButton->setMenu(setupPopup);
  connectionRow->addWidget(host_);
  connectionRow->addWidget(port_);
  connectionRow->addWidget(connectButton_);
  connectionRow->addWidget(connectionLabel_);
  reverse_ = new QCheckBox(QStringLiteral("Reverse"), central);
  reverse_->setChecked(QSettings().value(QStringLiteral("rx/reverse"), false).toBool());
  connectionRow->addWidget(reverse_);
  // AppKit keeps mode selection in the top toolbar, not in the QSO entry row.
  auto *strictToggle = new QCheckBox(QStringLiteral("Strict"), central);
  strictToggle->setToolTip(QStringLiteral("Require six stable gate blocks and a plausible RTTY sequence"));
  strictToggle->setChecked(QSettings().value(QStringLiteral("rx/strictLock"), false).toBool());
  strictLock_ = strictToggle;
  connect(strictToggle, &QCheckBox::toggled, this, [this](bool enabled) {
    QSettings().setValue(QStringLiteral("rx/strictLock"), enabled);
    strictGateBlocks_ = 0;
    noCarrierBlocks_ = 0;
    decodedLocked_ = false;
    pendingDecoded_.clear();
  });
  connectionRow->addWidget(strictToggle);
  connectionRow->addWidget(setupButton);
  contestMode_ = new QCheckBox(QStringLiteral("Contest"), central);
  contestMode_->setChecked(QSettings().value(QStringLiteral("contest/enabled"), false).toBool());
  connectionRow->addWidget(contestMode_);
  contestType_ = new QComboBox(central);
  contestType_->addItems({QStringLiteral("DARC-RTTY"), QStringLiteral("SARTG-WW-RTTY"), QStringLiteral("CQ-WW-RTTY"), QStringLiteral("JARL-WW-RTTY")});
  contestType_->setCurrentText(QSettings().value(QStringLiteral("contest/type"), QStringLiteral("DARC-RTTY")).toString());
  contestType_->setFixedWidth(132);
  connectionRow->addWidget(contestType_);
  auto *exportContest = new QPushButton(QStringLiteral("Contest Log"), central);
  connectionRow->addWidget(exportContest);
  connectionRow->addStretch(1);
  layout->addLayout(connectionRow);
  // Parameters occupy a narrow strip immediately below the connection row.
  auto *parameterRow = new QHBoxLayout();
  parameterRow->setSpacing(9);
  auto addParameter = [central, parameterRow](const QString & name, QWidget * field) {
    auto *column = new QVBoxLayout();
    column->setSpacing(2);
    column->addWidget(new QLabel(name, central));
    column->addWidget(field);
    parameterRow->addLayout(column);
  };
  center_ = new QDoubleSpinBox(central);
  center_->setRange(100.0, 12000.0);
  center_->setDecimals(1);
  center_->setSingleStep(10.0);
  center_->setFixedWidth(82);
  center_->setValue(QSettings().value(QStringLiteral("rx/pitchHz"), 2210.0).toDouble());
  addParameter(QStringLiteral("Pitch"), center_);
  shift_ = new QDoubleSpinBox(central);
  shift_->setRange(50.0, 1000.0);
  shift_->setDecimals(1);
  shift_->setSingleStep(10.0);
  shift_->setFixedWidth(78);
  shift_->setValue(QSettings().value(QStringLiteral("rx/shiftHz"), 170.0).toDouble());
  addParameter(QStringLiteral("Shift"), shift_);
  baud_ = new QDoubleSpinBox(central);
  baud_->setRange(30.0, 300.0);
  baud_->setDecimals(4);
  baud_->setSingleStep(0.01);
  baud_->setFixedWidth(90);
  baud_->setValue(QSettings().value(QStringLiteral("rx/baud"), 45.45).toDouble());
  addParameter(QStringLiteral("Baud"), baud_);
  sql_ = new QSpinBox(central);
  sql_->setRange(0, 100);
  sql_->setSingleStep(1);
  sql_->setFixedWidth(80);
  // Preserve the former raw threshold setting on first use of the 0–100 scale.
  const QSettings sqlSettings;
  const QVariant savedSql = sqlSettings.value(QStringLiteral("rx/sqlLevel"));
  const double oldThreshold = sqlSettings.value(QStringLiteral("rx/sqlOpen"), 0.029).toDouble();
  const int sqlLevel = savedSql.isValid()
                       ? savedSql.toInt()
                       : qRound((oldThreshold - 0.005) * (100.0 / 0.080));
  sql_->setValue(sqlLevel);
  addParameter(QStringLiteral("SQL"), sql_);
  idleTimeout_ = new QSpinBox(central);
  idleTimeout_->setRange(1, 600);
  idleTimeout_->setFixedWidth(64);
  idleTimeout_->setValue(QSettings().value(QStringLiteral("tx/idleTimeout"), 25).toInt());
  addParameter(QStringLiteral("Idle s"), idleTimeout_);
  auto *applyButton = new QPushButton(QStringLiteral("Apply"), central);
  connect(applyButton, &QPushButton::clicked, this, [this] {
    configureRx();
    if (!tci_.isConnected()) return;
    // Only an explicit user Apply writes the offset to deskHPSDR.
    // Incoming TCI updates and local decoder reconfiguration never echo it.
    const int hz = qRound(center_->value());
    if (hz < 100 || hz > 4000) {
      statusBar()->showMessage(QStringLiteral("DIGI offset must be between 100 and 4000 Hz"), 5000);
        return;
      }
    const bool digu = tciMode_ == QStringLiteral("DIGU");
    if (sessionRestoring_ || (autoSetupSession_ &&
                              (!snapshotModeReady_ || !snapshotDiglReady_ || !snapshotDiguReady_))) {
    statusBar()->showMessage(QStringLiteral("Waiting for original TCI settings"), 5000);
      return;
    }
    // Without Auto Setup, Apply remains a manual change and is not
    // automatically restored. Never create an incomplete snapshot.
    if (autoSetupSession_) {
    if (digu) { sessionChangedDigu_ = true; }
      else { sessionChangedDigl_ = true; }
    }
    tci_.sendText(QStringLiteral("%1:%2;")
                  .arg(digu ? QStringLiteral("digu_offset") : QStringLiteral("digl_offset"))
                  .arg(hz));
  });
  parameterRow->addWidget(applyButton, 0, Qt::AlignBottom);
  parameterRow->addStretch();
  layout->addLayout(parameterRow);
  // Upper band: compact scope, I/Q panadapter and TCI diagnostics.
  auto *upper = new QHBoxLayout();
  upper->setSpacing(8);
  scopeWidget_ = new RTTYScopeWidget(central);
  scopeWidget_->setFixedSize(420, 200);
  upper->addWidget(scopeWidget_, 0, Qt::AlignTop);
  iqPan_ = new RTTYIQPanWidget(central);
  iqPan_->setTones(diglOffsetHz_, shift_->value(), false, reverse_->isChecked());
  iqPan_->clickTune = [this](double offset) {
    if (!tci_.isConnected() || tciFrequencyHz_ <= 0) return;
    const qint64 hz = qRound64(tciFrequencyHz_ + offset);
    if (hz > 0) tci_.sendText(QStringLiteral("vfo:0,0,%1;").arg(hz));
  };
  iqPan_->dragTune = [this](double delta, bool begin) {
    if (begin) { iqDragStartFrequencyHz_ = tci_.isConnected() ? tciFrequencyHz_ : 0; return; }
    if (!tci_.isConnected() || iqDragStartFrequencyHz_ <= 0) return;
    const qint64 hz = qRound64(iqDragStartFrequencyHz_ - delta);
    if (hz > 0) tci_.sendText(QStringLiteral("vfo:0,0,%1;").arg(hz));
  };
  upper->addWidget(iqPan_, 0, Qt::AlignTop);
  log_ = new QPlainTextEdit(central);
  log_->setObjectName(QStringLiteral("tciLog"));
  log_->setReadOnly(true);
  log_->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #191a1b; color: #a8a8a8; border: 0; }"));
  QFont tciFont(QStringLiteral("JetBrains Mono"), 11);
#ifdef Q_OS_WIN
  tciFont.setPointSize(tciFont.pointSize() - 2);
#endif
  log_->setFont(tciFont);
  log_->document()->setMaximumBlockCount(1000);
  log_->setMinimumWidth(190);
  log_->setFixedHeight(200);
  log_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  upper->addWidget(log_, 1, Qt::AlignTop);
  layout->addLayout(upper);
  // Full-width decoder below the scope, matching AppKit's reading order.
  auto *decoderHeading = new QHBoxLayout();
  decoderHeading->addWidget(new QLabel(QStringLiteral("Decoded RTTY"), central));
  rxStatus_ = new QLabel(QStringLiteral("QRG —  |  MODE DIGL"), central);
  decoderHeading->addWidget(rxStatus_, 1);
  // Keep the tuning indicators alongside the decoder status, not between
  // the XY scope and I/Q panadapter. Fixed LED cells avoid horizontal jitter.
  decoderHeading->setSpacing(8);
  auto *markText = new QLabel(QStringLiteral("MARK"), central);
  decoderHeading->addWidget(markText);
  markLed_ = new QLabel(QStringLiteral("●"), central);
  markLed_->setAlignment(Qt::AlignCenter);
  markLed_->setFixedWidth(22);
  markLed_->setStyleSheet(QStringLiteral("color: #45565c; font-size: 20px; font-weight: bold;"));
  decoderHeading->addWidget(markLed_);
  auto *spaceText = new QLabel(QStringLiteral("SPACE"), central);
  decoderHeading->addWidget(spaceText);
  spaceLed_ = new QLabel(QStringLiteral("●"), central);
  spaceLed_->setAlignment(Qt::AlignCenter);
  spaceLed_->setFixedWidth(22);
  spaceLed_->setStyleSheet(QStringLiteral("color: #45565c; font-size: 20px; font-weight: bold;"));
  decoderHeading->addWidget(spaceLed_);
  afcLabel_ = new QLabel(QStringLiteral("AFC -- Hz"), central);
  afcLabel_->setFixedWidth(100);
  decoderHeading->addWidget(afcLabel_);
  auto *clearRx = new QPushButton(QStringLiteral("Clear"), central);
  decoderHeading->addWidget(clearRx);
  writeLog_ = new QCheckBox(QStringLiteral("Write Log"), central);
  writeLog_->setChecked(QSettings().value(QStringLiteral("rx/writeMonitorLog"), true).toBool());
  writeLog_->setToolTip(
          QStringLiteral("Write decoded RX and TX monitor text to ~/Documents/rttyPad/rttyPad-YYYY-MM-DD.txt"));
  decoderHeading->addWidget(writeLog_);
  connect(writeLog_, &QCheckBox::toggled, this, [](bool enabled) {
    QSettings().setValue(QStringLiteral("rx/writeMonitorLog"), enabled);
  });
  layout->addLayout(decoderHeading);
  auto *decodeView = new DecodePickView(central);
  decoder_ = decodeView;
  decoder_->setReadOnly(true);
  decoder_->setObjectName(QStringLiteral("rttyDecoder"));
  decoder_->document()->setMaximumBlockCount(2000);
  decoder_->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #181a1b; color: #e0e0e0; border: 0; }"));
  QFont decoderFont(QStringLiteral("JetBrains Mono"));
  decoderFont.setPointSize(std::clamp(QSettings().value(QStringLiteral("ui/decoderFontSize"), 11).toInt(), 8, 20));
  decoderFont.setWeight(QFont::Normal);
  decoderFont.setStyleHint(QFont::Monospace);
  decoder_->setFont(decoderFont);
  connect(clearRx, &QPushButton::clicked, decoder_, &QPlainTextEdit::clear);
  layout->addWidget(decoder_, 1);
  // Diagnostics are retained without stealing the decoder's vertical space.
  audioLabel_ = new QLabel(QStringLiteral("RX audio: no frames"), central);
  scopeLabel_ = new QLabel(QStringLiteral("MARK —  SPACE —  Quality —  Gate —"), central);
  audioLabel_->hide();
  scopeLabel_->hide();
  // Normal ADIF and contest logging use independent persistence.
  auto *qsoRow = new QHBoxLayout();
  qsoRow->setSpacing(6);
  auto qsoField = [central, qsoRow](const QString & label, int width,
  const QString &initial = QString()) {
    qsoRow->addWidget(new QLabel(label, central));
    auto *edit = new QLineEdit(central);
    edit->setFixedWidth(width);
    edit->setText(initial);
    qsoRow->addWidget(edit);
    return edit;
  };
  qsoCall_ = qsoField(QStringLiteral("CALL"), 125);
  qsoName_ = qsoField(QStringLiteral("NAME"), 125);
  qsoSent_ = qsoField(QStringLiteral("S"), 50, QStringLiteral("599"));
  qsoReceived_ = qsoField(QStringLiteral("R"), 50, QStringLiteral("599"));
  for (QLineEdit *edit : {qsoCall_, qsoName_, qsoSent_, qsoReceived_})
    normalizeUpperEdit(edit);
  // Contest-only QSO fields form one unit so their labels disappear too.
  auto *contestFields = new QWidget(central);
  auto *contestFieldsLayout = new QHBoxLayout(contestFields);
  contestFieldsLayout->setContentsMargins(0, 0, 0, 0);
  contestFieldsLayout->setSpacing(5);
  auto *contestExchangeLabel = new QLabel(QStringLiteral("EXCH"), contestFields);
  contestExchangeLabel->setObjectName(QStringLiteral("contestExchangeLabel"));
  contestFieldsLayout->addWidget(contestExchangeLabel);
  contestExchange_ = new QLineEdit(contestFields);
  normalizeUpperEdit(contestExchange_);
  contestExchange_->setFixedWidth(75);
  contestFieldsLayout->addWidget(contestExchange_);
  contestSerial_ = new QSpinBox(central);
  contestSerial_->setRange(-1, 99999);
  contestSerial_->setValue(QSettings().value(QStringLiteral("contest/cqwwZone"),
      QSettings().value(QStringLiteral("contest/serial"), 14)).toInt());
  auto *contestOwnLabel = new QLabel(QStringLiteral("NR"), contestFields);
  contestOwnLabel->setObjectName(QStringLiteral("contestOwnLabel"));
  contestFieldsLayout->addWidget(contestOwnLabel);
  contestSerial_->setFixedWidth(72);
  contestFieldsLayout->addWidget(contestSerial_);
  dupeLabel_ = new QLabel(contestFields);
  contestFieldsLayout->addWidget(dupeLabel_);
  contestFields->setObjectName(QStringLiteral("contestFields"));
  connect(exportContest, &QPushButton::clicked, this, [this] { showContestLog(); });
  connect(contestMode_, &QCheckBox::toggled, this, [this](bool enabled) {
    QSettings().setValue(QStringLiteral("contest/enabled"), enabled);
    updateContestUi();
    refreshMacroButtons();
  });
  connect(contestType_, &QComboBox::currentTextChanged, this, [this](const QString & type) {
    QSettings().setValue(QStringLiteral("contest/type"), type);
    // Restore each contest's independent outgoing exchange on selection.
    const bool cqww = type == QStringLiteral("CQ-WW-RTTY");
    QSettings settings;
    const QSignalBlocker blocker(contestSerial_);
    contestSerial_->setRange(type == QStringLiteral("JARL-WW-RTTY") ? -1 : 1, cqww ? 40 : (type == QStringLiteral("JARL-WW-RTTY") ? 99 : 99999));
    contestSerial_->setValue(settings.value(type == QStringLiteral("JARL-WW-RTTY") ? QStringLiteral("contest/jarlManualAge") : (cqww ? QStringLiteral("contest/cqwwZone")
                                            : (type == QStringLiteral("DARC-RTTY") ? QStringLiteral("contest/darcSerial") : QStringLiteral("contest/sartgSerial"))),
                                            type == QStringLiteral("JARL-WW-RTTY") ? -1 : (cqww ? 14 : 1)).toInt());
    updateContestUi();
    refreshMacroButtons();
  });
  connect(contestSerial_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int serial) {
    QSettings().setValue(contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") ? QStringLiteral("contest/jarlManualAge") : (contestType_->currentText() == QStringLiteral("CQ-WW-RTTY")
                         ? QStringLiteral("contest/cqwwZone") : (contestType_->currentText() == QStringLiteral("DARC-RTTY")
                             ? QStringLiteral("contest/darcSerial") : QStringLiteral("contest/sartgSerial"))), serial);
  });
  connect(qsoCall_, &QLineEdit::textChanged, this, [this] { updateContestUi(); });
  exportContest->setObjectName(QStringLiteral("contestExport"));
  contestType_->setObjectName(QStringLiteral("contestType"));
  contestExchange_->setObjectName(QStringLiteral("contestExchange"));
  contestSerial_->setObjectName(QStringLiteral("contestSerial"));
  // Apply the persisted exchange of the initially selected contest too.
  {
    const bool cqww = contestType_->currentText() == QStringLiteral("CQ-WW-RTTY");
    const QSignalBlocker blocker(contestSerial_);
    contestSerial_->setRange(contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") ? -1 : 1, cqww ? 40 : (contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") ? 99 : 99999));
    contestSerial_->setValue(QSettings().value(contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") ? QStringLiteral("contest/jarlManualAge") : (cqww ? QStringLiteral("contest/cqwwZone")
        : (contestType_->currentText() == QStringLiteral("DARC-RTTY") ? QStringLiteral("contest/darcSerial") :
           QStringLiteral("contest/sartgSerial"))), contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") ? -1 : (cqww ? 14 : 1)).toInt());
  }
  updateContestUi();
  qsoCall_->installEventFilter(this);
  qsoName_->installEventFilter(this);
  // Keep keyboard QSO entry and decoder double-click targeting in sync.
  connect(qsoCall_, &QLineEdit::returnPressed, this, [this] {
    decodePickTarget_ = DecodePickTarget::Name;
    qsoName_->setFocus();
  });
  connect(qsoName_, &QLineEdit::returnPressed, this, [this] {
    qsoReceived_->setFocus();
    qsoReceived_->selectAll();
  });
  decodeView->onPicked = [this](const QString & token) {
    if (decodePickTarget_ == DecodePickTarget::Name) {
      qsoName_->setText(token.toUpper());
      qsoName_->setFocus();
    } else {
      qsoCall_->setText(token.toUpper());
      decodePickTarget_ = DecodePickTarget::Name;
      qsoName_->setFocus();
    }
  };
  auto *newQso = new QPushButton(QStringLiteral("New QSO"), central);
  connect(newQso, &QPushButton::clicked, this, [this] {
    qsoCall_->clear();
    qsoName_->clear();
    qsoReceived_->setText(QStringLiteral("599"));
    contestExchange_->clear();
    decodePickTarget_ = DecodePickTarget::Call;
    appendMonitorLogText(QStringLiteral("\n--- NEW QSO %1 ---\n")
    .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"))));
    qsoCall_->setFocus();
  });
  qsoRow->addWidget(newQso);
  auto *logQso = new QPushButton(QStringLiteral("Log"), central);
  logQso->setToolTip(QStringLiteral("Review and save QSO to ~/Documents/rttyPad/rttyPad.adi"));
  connect(logQso, &QPushButton::clicked, this, [this] {
    if (contestMode_->isChecked()) logContestQso();
    else { logQsoAdif(); }
  });
  qsoRow->addWidget(logQso);
  qsoRow->addWidget(contestFields);
  qsoRow->addStretch(1);
  layout->addLayout(qsoRow);
  // TX: a single compact status/control strip and a full-width text editor.
  auto *txButtons = new QHBoxLayout();
  txButtons->setSpacing(6);
  txButtons->addWidget(new QLabel(QStringLiteral("TX"), central));
  txStatus_ = new QLabel(QStringLiteral("Disconnected"), central);
  txStatus_->setMinimumWidth(150);
  txButtons->addWidget(txStatus_);
  sendButton_ = new QPushButton(QStringLiteral("TX ON"), central);
  drainButton_ = new QPushButton(QStringLiteral("Drain / RX"), central);
  stopButton_ = new QPushButton(QStringLiteral("TX OFF"), central);
  auto *clearTx = new QPushButton(QStringLiteral("Clear"), central);
  txButtons->addWidget(new QLabel(QStringLiteral("Fill"), central));
  txFill_ = new QComboBox(central);
  txFill_->addItems({QStringLiteral("MARK"), QStringLiteral("SPACE"), QStringLiteral("LTRS")});
  txFill_->setCurrentText(QSettings().value(QStringLiteral("tx/fill"), QStringLiteral("LTRS")).toString());
  txFill_->setFixedWidth(85);
  txButtons->addWidget(txFill_);
  txButtons->addWidget(new QLabel(QStringLiteral("Stop"), central));
  txStopBits_ = new QComboBox(central);
  txStopBits_->addItems({QStringLiteral("1"), QStringLiteral("1.5"), QStringLiteral("2")});
  txStopBits_->setCurrentText(QSettings().value(QStringLiteral("tx/stopBits"), QStringLiteral("1.5")).toString());
  txStopBits_->setFixedWidth(65);
  txButtons->addWidget(txStopBits_);
  txUos_ = new QCheckBox(QStringLiteral("UOS"), central);
  txUos_->setChecked(QSettings().value(QStringLiteral("tx/uos"), true).toBool());
  txButtons->addWidget(txUos_);
  txStartCrlf_ = new QCheckBox(QStringLiteral("Start CR/LF"), central);
  txStartCrlf_->setChecked(QSettings().value(QStringLiteral("tx/startCrlf"), true).toBool());
  txButtons->addWidget(txStartCrlf_);
  txButtons->addStretch(1);
  txButtons->addWidget(drainButton_);
  txButtons->addWidget(clearTx);
  txButtons->addWidget(sendButton_);
  txButtons->addWidget(stopButton_);
  layout->addLayout(txButtons);
  // Native FSK is generated inside deskHPSDR (TCI native mode). AppKit's TX gain, BPF
  // and shaping controls belong to its separate AFSK audio path.
  connect(txFill_, &QComboBox::currentTextChanged, this, [this](const QString &) { sendNativeTxParameters(); });
  connect(txStopBits_, &QComboBox::currentTextChanged, this, [this](const QString &) { sendNativeTxParameters(); });
  connect(txUos_, &QCheckBox::toggled, this, [this](bool) { sendNativeTxParameters(); });
  connect(txStartCrlf_, &QCheckBox::toggled, this, [this](bool) { sendNativeTxParameters(); });
  connect(idleTimeout_, QOverload<int>::of(&QSpinBox::valueChanged), this,
  [this](int) { sendNativeTxParameters(); });
  txInput_ = new QPlainTextEdit(central);
  txInput_->setPlaceholderText(QStringLiteral("Enter RTTY text to transmit"));
  txInput_->setFixedHeight(52);
  txInput_->setStyleSheet(QStringLiteral("QPlainTextEdit { background: #0e0e0e; color: #e0e0e0; border: 0; }"));
#ifdef Q_OS_WIN
  txInput_->setFont(decoder_->font());
#else
  txInput_->setFont(QFont(QStringLiteral("Menlo"), 14));
#endif
  uppercaseTxEditor(txInput_);
  txInput_->installEventFilter(this);
  connect(clearTx, &QPushButton::clicked, this, [this] {
    if (!liveTxActive_ && !txOffDraining_) { txInput_->clear(); liveTxCommitted_ = 0; }
  });
  layout->addWidget(txInput_);
  // AppKit echoes outgoing text into the decoded traffic pane, not a
  // separate widget below the TX editor.
  auto *macroRow = new QHBoxLayout();
  macroRow->setSpacing(6);
  for (int i = 0; i < 8; ++i) {
    auto *button = new QPushButton(central);
    macroButtons_[i] = button;
    // F1–F8 activate the corresponding macro without moving focus away
    // from the decoder or QSO entry fields.
    button->setShortcut(QKeySequence(Qt::Key_F1 + i));
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(button, &QPushButton::clicked, this, [this, i] { runMacro(i); });
    connect(button, &QPushButton::customContextMenuRequested, this, [this, i](const QPoint &) { editMacro(i); });
    button->setFixedHeight(28);
    macroRow->addWidget(button, 1);
  }
  layout->addLayout(macroRow);
  refreshMacroButtons();
  connect(sendButton_, &QPushButton::clicked, this, [this] {
    if (liveTxActive_) { return; }
    if (!tx_.start()) { updateTxUi(); return; }
    liveTxActive_ = true;
    liveTxCommitted_ = 0;
    txInput_->clear();
    txEchoLineActive_ = false;
    txInput_->setFocus();
    updateTxUi();
  });
  connect(drainButton_, &QPushButton::clicked, this, [this] {
    if (tx_.drain()) { liveTxActive_ = false; }
    updateTxUi();
  });
  connect(stopButton_, &QPushButton::clicked, this, [this] {
    if (txOffDraining_ || tx_.state() != NativeRTTYTx::State::Sending) { return; }
    // Match AppKit TX OFF: commit the final word without adding SPACE,
    // then drain the server queue before returning to RX.
    if (liveTxActive_) {
      const QString contents = txInput_->toPlainText();
      const QString pending = contents.mid(qMin(liveTxCommitted_, int(contents.size())));
      if (!pending.isEmpty()) {
        if (!tx_.send(pending)) { return; }
        appendTxTraffic(pending);
        liveTxCommitted_ = int(contents.size());
      }
    }
    if (!tx_.drain()) { return; }
    txOffDraining_ = true;
    liveTxActive_ = false;
    updateTxUi();
  });
  connect(&tx_, &NativeRTTYTx::stateChanged, this, [this](NativeRTTYTx::State state) {
    if (txOffDraining_ && (state == NativeRTTYTx::State::Idle ||
                           state == NativeRTTYTx::State::Disconnected)) {
      txOffDraining_ = false;
      liveTxActive_ = false;
      liveTxCommitted_ = 0;
      txInput_->clear();
      // Finish the displayed TX line before the next decoded RX text.
      // This is a local LF, not an additional on-air RTTY character.
      if (txEchoLineActive_ && decoder_) {
        QTextCursor cursor = decoder_->textCursor();
        cursor.movePosition(QTextCursor::End);
        if (!decoder_->toPlainText().endsWith(QLatin1Char('\n'))) {
          cursor.insertText(QStringLiteral("\n"));
          decoder_->setTextCursor(cursor);
        }
      }
      txEchoLineActive_ = false;
    }
    updateTxUi();
  });
  updateTxUi();
  RTTY_RX_CONFIG cfg{};
  cfg.samplerate = 48000.0;
  cfg.baud = baud_->value();
  cfg.center_hz = rxCenterHz();
  cfg.shift_hz = shift_->value();
  cfg.reverse = rxReverse() ? 1 : 0;
  rx_ = rtty_rx_create(&cfg);
  configureRx();
  connect(center_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { configureRx(); });
  connect(reverse_, &QCheckBox::toggled, this, [this] { configureRx(); applyTciPitch(); });
  connect(shift_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { configureRx(); sendNativeTxParameters(); applyTciPitch(); });
  connect(baud_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { configureRx(); sendNativeTxParameters(); });
  connect(sql_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { configureRx(); });
  auto *scopeTimer = new QTimer(this);
  connect(scopeTimer, &QTimer::timeout, this, [this] { updateScope(); });
  scopeTimer->start(100);
  setCentralWidget(central);
  statusBar()->showMessage(QStringLiteral("TCI disconnected — RTTY RX active when audio arrives"));
  connect(connectButton_, &QPushButton::clicked, this, [this] {
    if (tci_.isConnected()) {
    disconnectRestoringSession();
      return;
    }
    bool ok = false;
    const uint port = port_->text().toUInt(&ok);
    if (!ok || port < 1 || port > 65535 || host_->text().trimmed().isEmpty()) {
    statusBar()->showMessage(QStringLiteral("Invalid TCI host or port"));
      return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("tci/host"), host_->text().trimmed());
    settings.setValue(QStringLiteral("tci/port"), port);
    connectionLabel_->setText(QStringLiteral("Connecting…"));
    tci_.connectToHost(host_->text().trimmed(), quint16(port));
  });
  connect(&tci_, &TCIClient::connected, this, [this] {
    updateConnectionUi(true);
    sendNativeTxParameters();
    beginRttySession();
    updateTxUi();
    if (QSettings().value(QStringLiteral("rx/show_smeter"), false).toBool())
      tci_.sendText(QStringLiteral("rx_sensors_enable:true,500;"));
  });
  connect(&tci_, &TCIClient::disconnected, this, [this](const QString & error) {
    ++sessionGeneration_;
    sessionRestoring_ = false;
    updateConnectionUi(false);
    log_->appendPlainText(QStringLiteral("TCI disconnected: %1").arg(error));
    if (closingAfterRestore_) QTimer::singleShot(0, this, [this] { close(); });
  });
  connect(&tci_, &TCIClient::logLine, this, [this](const QString & line) {
    log_->appendPlainText(line);
  });
  connect(&tci_, &TCIClient::textMessageReceived, this, [this](const QString & message) {
    log_->appendPlainText(QStringLiteral("RX TEXT: %1").arg(message));
    processTciParameters(message);
  });
  connect(&tci_, &TCIClient::iqFrameReceived, this,
          [this](const QByteArray &data, const TCIStreamInfo &info) {
    if (iqPan_) iqPan_->appendIQ(data, info.sampleRate, info.channels, info.payloadOffset, info.length);
  });
  connect(&tci_, &TCIClient::rxAudioFrameReceived, this,
  [this](const QByteArray & data, const TCIStreamInfo & info) {
    processRxAudio(data, info);
    ++rxFrames_;
    if (rxFrames_ % 20 == 1) {
      audioLabel_->setText(QStringLiteral("RX audio: %1 frames | %2 Hz | %3 ch | peak %4")
                           .arg(rxFrames_).arg(info.sampleRate).arg(info.channels)
                           .arg(info.peakLeft, 0, 'f', 3));
    }
  });
}

MainWindow::~MainWindow() {
  rtty_rx_destroy(rx_);
}

/* Shared decoder core — called from TCI path and standalone mic path */
void MainWindow::processRxSamples(const float *buf, int frames, double sr, bool isUsb)
{
  if (!rx_ || frames <= 0 || sr < 8000.0) return;
  if (rxSampleRate_ != (quint32)sr) {
    rxSampleRate_ = (quint32)sr;
    configureRx();
  }
  samples_.assign(buf, buf + frames);
  RTTY_RX_SCOPE scope{};
  char decoded[512]{};
  const int count = rtty_rx_process(rx_, samples_.data(), frames, decoded, sizeof(decoded));
  rtty_rx_get_scope(rx_, &scope);
  scopeWidget_->updateWaterfall(samples_.data(), frames, sr,
                                center_->value(), shift_->value(), isUsb, scope.carrier != 0);
  scopeWidget_->updateSample(scope);
  updateTuningIndicators(scope);
  updateAfc(samples_.data(), frames, sr, scope.carrier != 0);
  bool outputCarrier = scope.carrier != 0;
  if (strictLock_ && strictLock_->isChecked()) {
    strictGateBlocks_ = scope.gate_open ? std::min(strictGateBlocks_ + 1, 1000) : 0;
    outputCarrier = strictGateBlocks_ >= 6;
  } else {
    strictGateBlocks_ = 0;
  }
  if (!outputCarrier) {
    if (++noCarrierBlocks_ >= 50) { noCarrierBlocks_ = 0; decodedLocked_ = false; pendingDecoded_.clear(); }
    return;
  }
  noCarrierBlocks_ = 0;
  if (count <= 0) return;
  QString decodedText = QString::fromLatin1(decoded, count);
  if (strictLock_ && strictLock_->isChecked() && !decodedLocked_) {
    pendingDecoded_ += decodedText;
    if (pendingDecoded_.size() > 256) pendingDecoded_ = pendingDecoded_.right(256);
    const qsizetype ry = pendingDecoded_.indexOf(QStringLiteral("RYRY"));
    if (ry >= 0) {
      decodedText = pendingDecoded_.mid(ry);
    } else {
      qsizetype plausible = 0;
      for (qsizetype i = pendingDecoded_.size(); i > 0; --i) {
        const QChar ch = pendingDecoded_.at(i - 1);
        const ushort c = ch.unicode();
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              QStringLiteral(" \r\n.,?!/-+=:;'\"()&$").contains(ch))) break;
        ++plausible;
      }
      if (plausible < 8) return;
      decodedText = pendingDecoded_.right(plausible);
    }
    pendingDecoded_.clear();
    decodedLocked_ = true;
  }
  QTextCursor cursor = decoder_->textCursor();
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat rxFormat;
  rxFormat.setForeground(QColor(QStringLiteral("#e0e0e0")));
  cursor.insertText(decodedText, rxFormat);
  txEchoLineActive_ = false;
  decoder_->setTextCursor(cursor);
  decoder_->ensureCursorVisible();
  appendMonitorLogText(decodedText);
}

void MainWindow::processRxAudio(const QByteArray &data, const TCIStreamInfo &info) {
  if (!rx_ || !info.isRxAudioFloat32 || info.sampleRate < 8000 ||
      info.channels == 0 || info.channels > 8 || info.audioFrames == 0 ||
      info.audioFrames > 65536 || info.payloadLength < 0) {
    return;
  }
  const quint64 needed = quint64(info.audioFrames) * info.channels * sizeof(float);
  if (needed > quint64(info.payloadLength) ||
      quint64(data.size()) < quint64(info.payloadOffset) + needed) {
    return;
  }
  // Extract first channel (downmix multichannel to mono)
  tmpSamples_.resize(info.audioFrames);
  const char *payload = data.constData() + info.payloadOffset;
  for (quint32 i = 0; i < info.audioFrames; ++i) {
    float value;
    std::memcpy(&value, payload + size_t(i) * info.channels * sizeof(float), sizeof(float));
    tmpSamples_[i] = std::isfinite(value) ? value : 0.0f;
  }
  processRxSamples(tmpSamples_.data(), (int)info.audioFrames, info.sampleRate,
                   tciMode_ == QStringLiteral("DIGU"));
}

void MainWindow::appendTxTraffic(const QString &text) {
  if (text.isEmpty() || !decoder_) { return; }
  QTextCursor cursor = decoder_->textCursor();
  cursor.movePosition(QTextCursor::End);
  QTextCharFormat format;
  format.setForeground(QColor(QStringLiteral("#66ccff")));
  const bool newTxLine = !txEchoLineActive_;
  if (!txEchoLineActive_) {
    const QString existing = decoder_->toPlainText();
    if (!existing.isEmpty() && !existing.endsWith(QLatin1Char('\n'))) {
      cursor.insertText(QStringLiteral("\n"), format);
    }
    cursor.insertText(QStringLiteral("TX: "), format);
    txEchoLineActive_ = true;
  }
  cursor.insertText(text, format);
  decoder_->setTextCursor(cursor);
  decoder_->ensureCursorVisible();
  appendMonitorLogText((newTxLine ? QStringLiteral("\nTX: ") : QString()) + text);
}

// Use CRLF for newly written text files on every platform. When appending to
// an existing log, retain its line endings instead of mixing conventions.
static QByteArray logLineEnding(const QString &path) {
  QFile existing(path);
  if (existing.open(QIODevice::ReadOnly)) {
    const QByteArray sample = existing.read(4096);
    const int newline = sample.indexOf('\n');
    if (newline >= 0) return newline > 0 && sample.at(newline - 1) == '\r'
                               ? QByteArray("\r\n") : QByteArray("\n");
  }
  return QByteArray("\r\n");
}

static QByteArray withLineEnding(QByteArray data, const QByteArray &eol) {
  data.replace("\r\n", "\n");
  if (eol != "\n") { data.replace("\n", eol); }
  return data;
}

void MainWindow::appendMonitorLogText(const QString &text) {
  if (text.isEmpty() || !writeLog_ || !writeLog_->isChecked()) { return; }
  const QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                            + QStringLiteral("/rttyPad");
  if (!QDir().mkpath(directory)) { return; }
  const QString path = directory + QStringLiteral("/rttyPad-")
                       + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd"))
                       + QStringLiteral(".txt");
  if (monitorLogActivePath_ != path) {
    monitorLogActivePath_ = path;
    monitorLogHeaderWritten_ = false;
  }
  const QByteArray eol = logLineEnding(path);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) { return; }
  QByteArray output;
  if (!monitorLogHeaderWritten_) {
    const QString frequency = tciFrequencyHz_ > 0.0
                              ? QStringLiteral("QRG %1 MHz").arg(tciFrequencyHz_ / 1000000.0, 0, 'f', 6)
                              : QStringLiteral("QRG ---.------ MHz");
    output += QStringLiteral("\n[%1] %2 MODE %3\n")
              .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")),
                   frequency, tciMode_).toUtf8();
  }
  output += text.toUtf8();
  output = withLineEnding(output, eol);
  if (file.write(output) == output.size()) { monitorLogHeaderWritten_ = true; }
}

void MainWindow::updateConnectionUi(bool connected) {
  if (!connected) {
    rxSampleRate_ = 0;
    tciFrequencyHz_ = 0.0;
    if (iqPan_) { iqPan_->reset(); iqPan_->setFrequency(0); }
    tciRxDbmValid_ = false;
    updateRxStatus();
    if (rx_) { rtty_rx_reset(rx_); }
    strictGateBlocks_ = 0;
    noCarrierBlocks_ = 0;
    decodedLocked_ = false;
    pendingDecoded_.clear();
    rxFrames_ = 0;
    audioLabel_->setText(QStringLiteral("RX audio: no frames"));
    scopeLabel_->setText(QStringLiteral("MARK —  SPACE —  Quality —  Gate —"));
    scopeWidget_->clear();
    afcValid_ = false;
    afcErrorHz_ = 0.0;
    afcLabel_->setText(QStringLiteral("AFC -- Hz"));
    afcLabel_->setStyleSheet(QString());
    markLed_->setStyleSheet(QStringLiteral("color: #45565c; font-weight: bold;"));
    spaceLed_->setStyleSheet(QStringLiteral("color: #45565c; font-weight: bold;"));
  }
  connectButton_->setText(connected ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
  connectionLabel_->setText(connected ? QStringLiteral("Verbonden") : QStringLiteral("Niet verbonden"));
  setWindowTitle(QStringLiteral("rttyPad — %1 offset %2 Hz — %3")
                 .arg(tciConnMode_ ? QStringLiteral("TCI") : QStringLiteral("Audio"))
                 .arg(center_->value(), 0, 'f', 0)
                 .arg(connected ? QStringLiteral("Verbonden") : QStringLiteral("Niet verbonden")));
  host_->setEnabled(!connected);
  port_->setEnabled(!connected);
  statusBar()->showMessage(connected ? QStringLiteral("TCI verbonden")
                           : QStringLiteral("TCI niet verbonden"));
}

void MainWindow::configureRx() {
  if (!rx_) { return; }
  RTTY_RX_CONFIG cfg{};
  cfg.samplerate = rxSampleRate_ ? rxSampleRate_ : 48000.0;
  cfg.baud = baud_->value();
  cfg.center_hz = rxCenterHz();
  cfg.shift_hz = shift_->value();
  cfg.reverse = rxReverse() ? 1 : 0;
  cfg.sql_open = float(0.005 + (sql_->value() / 100.0) * 0.080);
  cfg.sql_close = cfg.sql_open * 0.5f;
  rtty_rx_configure(rx_, &cfg);
  // A new tuning or sample rate invalidates the previous XY trace.
  if (scopeWidget_) { scopeWidget_->clear(); }
  QSettings settings;
  settings.setValue(QStringLiteral("rx/pitchHz"), center_->value());
  settings.setValue(QStringLiteral("rx/reverse"), reverse_->isChecked());
  settings.setValue(QStringLiteral("rx/shiftHz"), shift_->value());
  settings.setValue(QStringLiteral("rx/baud"), baud_->value());
  settings.setValue(QStringLiteral("rx/sqlLevel"), sql_->value());
}

void MainWindow::updateScope() {
  if (!rx_) { return; }
  RTTY_RX_SCOPE scope{};
  rtty_rx_get_scope(rx_, &scope);
  scopeLabel_->setText(QStringLiteral("MARK %1  SPACE %2  Quality %3  Gate %4")
                       .arg(scope.mark_mag, 0, 'f', 3)
                       .arg(scope.space_mag, 0, 'f', 3)
                       .arg(scope.decision_quality, 0, 'f', 2)
                       .arg(scope.gate_open ? QStringLiteral("OPEN") : QStringLiteral("CLOSED")));
}

void MainWindow::updateTuningIndicators(const RTTY_RX_SCOPE &scope) {
  const bool carrier = scope.carrier != 0;
  const double confidence = 0.45 + 0.55 * std::min(1.0, std::fabs(double(scope.soft)));
  const double activeIntensity = 0.55 + 0.45 * confidence;
  const bool markActive = scope.bit != 0;
  auto setLed = [carrier, activeIntensity](QLabel * led, bool active) {
    const int intensity = carrier ? int(255.0 * (active ? activeIntensity : 0.08)) : 0;
    led->setStyleSheet(QStringLiteral("color: rgb(0,%1,%2); font-weight: bold;")
                       .arg(intensity).arg(intensity));
  };
  setLed(markLed_, markActive);
  setLed(spaceLed_, !markActive);
}

void MainWindow::updateAfc(const float *samples, size_t count, double sampleRate, bool carrier) {
  // Display-only AFC, matching AppKit: never retune the decoder.
  const double centerHz = rxCenterHz();
  const double markHz = centerHz - shift_->value() / 2.0;
  const double spaceHz = centerHz + shift_->value() / 2.0;
  bool valid = carrier && samples && count && sampleRate > 0.0 && markHz >= 50.0;
  double errorHz = 0.0;
  if (valid) {
    double markLevel = 0.0, spaceLevel = 0.0;
    const double markPeak = localTonePeak(samples, count, sampleRate, markHz, markLevel);
    const double spacePeak = localTonePeak(samples, count, sampleRate, spaceHz, spaceLevel);
    const double total = markLevel + spaceLevel;
    valid = markPeak > 0.0 && spacePeak > 0.0 && total > 1.0e-12;
    if (valid) {
      errorHz = ((markPeak - markHz) * markLevel + (spacePeak - spaceHz) * spaceLevel) / total;
    }
  }
  if (!valid || !std::isfinite(errorHz)) {
    afcValid_ = false;
    afcErrorHz_ *= 0.95;
    afcLabel_->setText(QStringLiteral("AFC -- Hz"));
    afcLabel_->setStyleSheet(QString());
    return;
  }
  if (!afcValid_) { afcErrorHz_ = errorHz; afcValid_ = true; }
  else { afcErrorHz_ += 0.06 * (errorHz - afcErrorHz_); }
  double displayHz = std::round(afcErrorHz_ / 2.0) * 2.0;
  if (std::fabs(displayHz) < 1.0) { displayHz = 0.0; }
  afcLabel_->setText(QStringLiteral("AFC %1%2 Hz")
                     .arg(displayHz >= 0.0 ? QStringLiteral("+") : QString())
                     .arg(displayHz, 0, 'f', 0));
  afcLabel_->setStyleSheet(std::fabs(displayHz) <= 25.0
                           ? QStringLiteral("color: #d6d6d6;") : QStringLiteral("color: #ffb340;"));
}

void MainWindow::updateTxUi() {
  const auto state = tx_.state();
  // TX is always via standalone engine now; no TCI binary audio TX.
    // In Audio-modus werken TX/macro's zonder TCI verbinding.
  const bool tciOk = !tciConnMode_ || tci_.isConnected();
  const bool connected = tciOk && state != NativeRTTYTx::State::Disconnected;
  const bool draining = state == NativeRTTYTx::State::Draining;
  sendButton_->setEnabled(connected && !draining);
  drainButton_->setEnabled(connected && state == NativeRTTYTx::State::Sending);
  stopButton_->setEnabled(connected && state == NativeRTTYTx::State::Sending && !txOffDraining_);
  switch (state) {
  case NativeRTTYTx::State::Disconnected:
    txStatus_->setText(QStringLiteral("Disconnected"));
    break;
  case NativeRTTYTx::State::Idle:
    txStatus_->setText(QStringLiteral("Idle"));
    break;
  case NativeRTTYTx::State::Sending:
    txStatus_->setText(QStringLiteral("Sending"));
    break;
  case NativeRTTYTx::State::Draining:
    txStatus_->setText(QStringLiteral("Waiting for RX confirmation"));
    break;
  }
}

namespace {
// Validate all tokens before any TX action. AppKit treats TX/RX as flags,
// and LTRS/FIGS as ITA2 control symbols (ignored by native deskHPSDR FSK).
QString macroSyntaxError(const QString &source) {
  const QRegularExpression tags(QStringLiteral("<([^<>]+)>"));
  const QStringList allowed = {
    QStringLiteral("TX"), QStringLiteral("RX"),
    QStringLiteral("BR"), QStringLiteral("CRLF"),
    QStringLiteral("LTRS"), QStringLiteral("FIGS"),
    QStringLiteral("MYCALL"), QStringLiteral("MYNAME"),
    QStringLiteral("MYQTH"), QStringLiteral("MYLOC"), QStringLiteral("MYDOK"),
    QStringLiteral("PCALL"), QStringLiteral("PNAME"),
    QStringLiteral("MYZONE"), QStringLiteral("SERIAL"), QStringLiteral("EXCH"),
    QStringLiteral("RST"), QStringLiteral("PRST")
  };
  auto it = tags.globalMatch(source);
  int txCount = 0, rxCount = 0;
  while (it.hasNext()) {
    const auto match = it.next();
    const QString name = match.captured(1).trimmed().toUpper();
    if (!allowed.contains(name)) {
      return QStringLiteral("Unsupported token <%1>.").arg(name);
    }
    if (name == QStringLiteral("TX")) { ++txCount; }
    if (name == QStringLiteral("RX")) { ++rxCount; }
  }
  if (txCount > 1 || rxCount > 1) {
    return QStringLiteral("Only one <TX> and one <RX> are permitted.");
  }
  QString withoutTags = source;
  withoutTags.remove(tags);
  if (withoutTags.contains(QLatin1Char('<')) || withoutTags.contains(QLatin1Char('>'))) {
    return QStringLiteral("Malformed macro token.");
  }
  for (const QChar ch : source) {
    if (ch.unicode() == 0x0e || ch.unicode() == 0x0f) {
      return QStringLiteral("Use <LTRS> or <FIGS> instead of raw shift controls.");
    }
  }
  return {};
}

QString defaultMacroText(int index) {
  static const char *const texts[] = {
    "<TX>CQ CQ CQ DE <MYCALL> <MYCALL> <MYCALL><BR><RX>",
    "<TX><PCALL> DE <MYCALL> <RST> <RST><BR><RX>",
    "<TX>NAME <MYNAME> QTH <MYQTH> LOC <MYLOC><BR><RX>",
    "<TX>TU 73 DE <MYCALL><BR><RX>",
    "<TX><MYCALL><BR><RX>",
    "<TX><RST><BR><RX>",
    "<TX>QTH <MYQTH><BR><RX>",
    "<TX>LOC <MYLOC><BR><RX>"
  };
  return (index >= 0 && index < 8) ? QLatin1String(texts[index]) : QString();
}
}

// JARL WW RTTY: age on the third Saturday of October (UTC).
// A zero manual setting means automatic DOB calculation; invalid/missing DOB sends 00.
int jarlAge(const QSettings &settings) {
  const QString value = settings.value(QStringLiteral("contest/jarlBirthDate")).toString().trimmed();
  static const QRegularExpression format(QStringLiteral("^([0-9]{2})\\.([0-9]{2})\\.([0-9]{4})$"));
  const auto match = format.match(value);
  if (!match.hasMatch()) return 0;
  const QDate birth(match.captured(3).toInt(), match.captured(2).toInt(), match.captured(1).toInt());
  if (!birth.isValid() || birth.year() < 1900) return 0;
  const int year = QDateTime::currentDateTimeUtc().date().year();
  const QDate octoberFirst(year, 10, 1);
  const int thirdSaturday = 1 + (6 - octoberFirst.dayOfWeek() + 7) % 7 + 14;
  const int age = year - birth.year() - ((birth.month() > 10 || (birth.month() == 10 && birth.day() > thirdSaturday)) ? 1 : 0);
  return age >= 0 && age <= 99 ? age : 0;
}

int jarlOutgoingAge(const QSettings &settings) {
  const int manual = settings.value(QStringLiteral("contest/jarlManualAge"), -1).toInt();
  return manual >= 0 && manual <= 99 ? manual : jarlAge(settings);
}

QString jarlExchange(const QSettings &settings) {
  return QStringLiteral("%1").arg(jarlOutgoingAge(settings), 2, 10, QLatin1Char('0'));
}

// Contest macros are separate from the normal-mode macros and from each other.
QString contestMacroPrefix(const QCheckBox *mode, const QComboBox *type) {
  if (!mode->isChecked()) { return QStringLiteral("macros/"); }
  return type->currentText() == QStringLiteral("JARL-WW-RTTY") ? QStringLiteral("contest/jarl/macros/")
         : type->currentText() == QStringLiteral("CQ-WW-RTTY")
         ? QStringLiteral("contest/cqww/macros/")
         : (type->currentText() == QStringLiteral("DARC-RTTY")
            ? QStringLiteral("contest/darc/macros/") : QStringLiteral("contest/sartg/macros/"));
}

QString defaultContestMacroText(int index, bool cqww, bool darc, bool jarl = false) {
  static const char *const common[] = {
    "<TX>CQ TEST DE <MYCALL> <MYCALL><BR><RX>",
    "<TX><PCALL> DE <MYCALL><BR><RX>",
    "<TX><PCALL> <RST> <EXCH> <EXCH><BR><RX>",
    "<TX><PCALL> TU <MYCALL><BR><RX>",
    "<TX>AGN AGN<BR><RX>",
    "<TX>NR NR?<BR><RX>",
    "<TX>QRZ DE <MYCALL><BR><RX>",
    "<TX>TU 73 DE <MYCALL><BR><RX>"
  };
  if (jarl) {
    static const char *const texts[] = {
      "<TX>CQ JARL TEST DE <MYCALL> <MYCALL><BR><RX>",
      "<TX><PCALL> DE <MYCALL><BR><RX>",
      "<TX><PCALL> <RST> <EXCH><BR><RX>",
      "<TX><PCALL> TU <MYCALL><BR><RX>",
      "<TX>AGN AGN<BR><RX>",
      "<TX>AGE AGE?<BR><RX>",
      "<TX>QRZ DE <MYCALL><BR><RX>",
      "<TX>TU 73 DE <MYCALL><BR><RX>"
    };
    return QLatin1String(texts[index]);
  }
  if (darc) { return QLatin1String(common[index]); }
  if (cqww && index == 2) { return QStringLiteral("<TX><PCALL> <RST> <MYZONE> <MYZONE><BR><RX>"); }
  if (cqww && index == 5) { return QStringLiteral("<TX>ZONE ZONE?<BR><RX>"); }
  if (!cqww && index == 0) { return QStringLiteral("<TX>CQ SARTG DE <MYCALL> <MYCALL><BR><RX>"); }
  if (!cqww && index == 2) { return QStringLiteral("<TX><PCALL> <RST> <SERIAL> <SERIAL><BR><RX>"); }
  return QLatin1String(common[index]);
}

void MainWindow::refreshMacroButtons() {
  QSettings settings;
  for (int i = 0; i < 8; ++i) {
    const bool contest = contestMode_->isChecked();
    const bool cqww = contestType_->currentText() == QStringLiteral("CQ-WW-RTTY");
    const QString key = contestMacroPrefix(contestMode_, contestType_) + QString::number(i) + QStringLiteral("/label");
    static const char *const normalTitles[] = {"CQ", "ANS", "INFO", "73", "CALL", "RST", "QTH", "LOC"};
    static const char *const contestTitles[] = {"CQ", "CALL", "EXCH", "TU", "AGN", "NR?", "QRZ", "73"};
    const QString fallback = contest ? (cqww && i == 5 ? QStringLiteral("ZONE?") : QLatin1String(contestTitles[i]))
                             : QLatin1String(normalTitles[i]);
    macroButtons_[i]->setText(settings.value(key, fallback).toString());
    macroButtons_[i]->setToolTip(QStringLiteral("F%1: send macro; right-click to edit").arg(i + 1));
  }
}

void MainWindow::editMacro(int index) {
  Q_UNUSED(index);
  QSettings settings;
  QDialog dialog(this);
  const bool contest = contestMode_->isChecked();
  const bool darc = contest && contestType_->currentText() == QStringLiteral("DARC-RTTY");
  const bool cqww = contest && contestType_->currentText() == QStringLiteral("CQ-WW-RTTY");
  const bool jarl = contest && contestType_->currentText() == QStringLiteral("JARL-WW-RTTY");
  dialog.setWindowTitle(contest ? QStringLiteral("%1 Setup").arg(contestType_->currentText())
                        : QStringLiteral("Macro Setup — Normal RTTY"));
  dialog.resize(790, 570);
  auto *outer = new QVBoxLayout(&dialog);
  outer->setSpacing(7);
  if (contest) {
    outer->addWidget(new QLabel(darc ?
                                QStringLiteral("DARC RTTY Kurzcontest — DL sends DOK/NM, Non-DL sends serial 001 onward.")
                                : (cqww ? QStringLiteral("CQ WW RTTY — exchange is RST plus CQ zone.")
                                   : (jarl ? QStringLiteral("JARL WW RTTY — exchange is RST plus age (00–99).") : QStringLiteral("SARTG RTTY — exchange is RST plus serial number."))), &dialog));
  }
  auto *station = new QGroupBox(QStringLiteral("Station Tokens"), &dialog);
  auto *stationGrid = new QGridLayout(station);
  const char *stationKeys[] = {"callsign", "name", "locator", "qth"};
  const char *stationLabels[] = {"CALLSIGN", "NAME", "LOC", "QTH"};
  QLineEdit *stationEdits[4] {};
  for (int i = 0; i < 4; ++i) {
    const int row = i / 2;
    const int col = (i % 2) * 2;
    stationGrid->addWidget(new QLabel(QLatin1String(stationLabels[i]), station), row, col);
    stationEdits[i] = new QLineEdit(settings.value(
                                            QStringLiteral("station/%1").arg(QLatin1String(stationKeys[i]))).toString(), station);
    normalizeUpperEdit(stationEdits[i]);
    stationGrid->addWidget(stationEdits[i], row, col + 1);
  }
  QLineEdit *dokEdit = nullptr;
  QComboBox *darcLocation = nullptr;
  if (darc) {
    stationGrid->addWidget(new QLabel(QStringLiteral("Station Location"), station), 2, 0);
    darcLocation = new QComboBox(station);
    darcLocation->addItems({QStringLiteral("DL"), QStringLiteral("Non-DL")});
    const QString storedLocation = settings.value(QStringLiteral("contest/darcLocation")).toString();
    darcLocation->setCurrentText(storedLocation == QStringLiteral("DL") || storedLocation == QStringLiteral("Non-DL")
                                 ? storedLocation : (isDarcGermanCall(stationEdits[0]->text()) ? QStringLiteral("DL") : QStringLiteral("Non-DL")));
    darcLocation->setToolTip(QStringLiteral("Operating country for ShortRY. Override for portable or foreign operation."));
    stationGrid->addWidget(darcLocation, 2, 1);
    stationGrid->addWidget(new QLabel(QStringLiteral("DOK"), station), 0, 4);
    dokEdit = new QLineEdit(settings.value(QStringLiteral("contest/dok"), QStringLiteral("NM")).toString(), station);
    normalizeUpperEdit(dokEdit);
    dokEdit->setMaximumWidth(110);
    stationGrid->addWidget(dokEdit, 0, 5);
    dokEdit->setEnabled(darcLocation->currentText() == QStringLiteral("DL"));
    connect(darcLocation, &QComboBox::currentTextChanged, dokEdit, [dokEdit](const QString & value) {
      dokEdit->setEnabled(value == QStringLiteral("DL"));
    });
  }
  QLineEdit *jarlDob = nullptr;
  if (jarl) {
    stationGrid->addWidget(new QLabel(QStringLiteral("Date of Birth"), station), 2, 0);
    jarlDob = new QLineEdit(settings.value(QStringLiteral("contest/jarlBirthDate")).toString(), station);
    jarlDob->setPlaceholderText(QStringLiteral("DD.MM.YYYY"));
    jarlDob->setToolTip(QStringLiteral("Age at JARL contest start; empty means 00. AGE override is in the contest row."));
    stationGrid->addWidget(jarlDob, 2, 1);
    stationGrid->addWidget(new QLabel(QStringLiteral("TX: 599 %1").arg(jarlExchange(settings)), station), 2, 2, 1, 2);
  }
  auto *showSMeter = new QCheckBox(QStringLiteral("Show S-meter"), station);
  showSMeter->setChecked(settings.value(QStringLiteral("rx/show_smeter"), false).toBool());
  stationGrid->addWidget(showSMeter, 1, 4, 1, 2);
  outer->addWidget(station);
  auto *content = new QWidget(&dialog);
  auto *grid = new QGridLayout(content);
  grid->setContentsMargins(0, 3, 0, 3);
  grid->setVerticalSpacing(5);
  grid->addWidget(new QLabel(QStringLiteral("Key"), content), 0, 0);
  grid->addWidget(new QLabel(QStringLiteral("Button"), content), 0, 1);
  grid->addWidget(new QLabel(QStringLiteral("Text"), content), 0, 2);
  QLineEdit *labels[8] {};
  QLineEdit *texts[8] {};
  static const char *const defaults[] = {"CQ", "ANS", "INFO", "73", "CALL", "RST", "QTH", "LOC"};
  for (int i = 0; i < 8; ++i) {
    const QString base = contestMacroPrefix(contestMode_, contestType_) + QString::number(i) + QLatin1Char('/');
    grid->addWidget(new QLabel(QStringLiteral("F%1").arg(i + 1), content), i + 1, 0);
    labels[i] = new QLineEdit(settings.value(base + QStringLiteral("label"),
      contestMode_->isChecked() ? (contestType_->currentText() == QStringLiteral("CQ-WW-RTTY") &&
                                   i == 5 ? QStringLiteral("ZONE?") : QStringList{QStringLiteral("CQ"), QStringLiteral("CALL"), QStringLiteral("EXCH"), QStringLiteral("TU"), QStringLiteral("AGN"), QStringLiteral("NR?"), QStringLiteral("QRZ"), QStringLiteral("73")}.at(
                                           i)) : QLatin1String(defaults[i])).toString(), content);
    normalizeUpperEdit(labels[i], false);
    labels[i]->setMaximumWidth(110);
    grid->addWidget(labels[i], i + 1, 1);
    texts[i] = new QLineEdit(settings.value(base + QStringLiteral("text"),
                                            contestMode_->isChecked() ? defaultContestMacroText(i, contestType_->currentText() == QStringLiteral("CQ-WW-RTTY"),
                                              contestType_->currentText() == QStringLiteral("DARC-RTTY"), contestType_->currentText() == QStringLiteral("JARL-WW-RTTY")) : defaultMacroText(i)).toString(), content);
    normalizeUpperEdit(texts[i], false);
    grid->addWidget(texts[i], i + 1, 2);
  }
  grid->setColumnStretch(2, 1);
  outer->addWidget(content, 1);
  outer->addWidget(new QLabel(QStringLiteral(
                                      "Tokens: <TX> <RX> <BR>/<CRLF> <MYCALL> <MYNAME> <MYQTH> <MYLOC> "
                                      "<PCALL> <PNAME> <RST> <PRST> <MYDOK> <MYZONE> <SERIAL> <EXCH>"), &dialog));
  auto *actions = new QHBoxLayout();
  auto *importButton = new QPushButton(QStringLiteral("Import…"), &dialog);
  auto *exportButton = new QPushButton(QStringLiteral("Export…"), &dialog);
  actions->addWidget(importButton);
  actions->addWidget(exportButton);
  actions->addStretch();
  outer->addLayout(actions);
  // Export/import uses the AppKit LABEL=TEXT format. Import is staged in
  // the editor and only persisted after OK, so Cancel remains safe.
  connect(importButton, &QPushButton::clicked, &dialog, [&] {
    const QString path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Import macro texts"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QMessageBox::warning(&dialog, QStringLiteral("Import failed"), file.errorString());
      return;
    }
    const QString data = QString::fromUtf8(file.readAll());
    int imported = 0;
    for (const QString &line : data.split(QLatin1Char('\n'))) {
      const QString entry = line.trimmed();
      if (entry.isEmpty() || entry.startsWith(QLatin1Char('#'))) { continue; }
      const int equal = entry.indexOf(QLatin1Char('='));
      if (equal <= 0) { continue; }
      const QString label = entry.left(equal).trimmed();
      const QString body = entry.mid(equal + 1).trimmed();
      if (label.isEmpty()) { continue; }
      for (int i = 0; i < 8; ++i) {
        if (labels[i]->text().compare(label, Qt::CaseInsensitive) == 0) {
          texts[i]->setText(body);
          ++imported;
          break;
        }
      }
    }
    QMessageBox::information(&dialog, QStringLiteral("Macro import"),
                             QStringLiteral("Imported %1 macro texts by label.").arg(imported));
  });
  connect(exportButton, &QPushButton::clicked, &dialog, [&] {
    const QString path = QFileDialog::getSaveFileName(&dialog,
      QStringLiteral("Export macro texts"), QStringLiteral("rtty-macros.txt"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
      QMessageBox::warning(&dialog, QStringLiteral("Export failed"), file.errorString());
      return;
    }
    QByteArray data;
    for (int i = 0; i < 8; ++i) {
      data += labels[i]->text().trimmed().toUtf8() + '=' + texts[i]->text().toUtf8() + '\n';
    }
    if (file.write(data) != data.size() || !file.flush())
      QMessageBox::warning(&dialog, QStringLiteral("Export failed"), file.errorString());
  });
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  outer->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    for (int i = 0; i < 8; ++i) {
      const QString error = macroSyntaxError(texts[i]->text());
      if (!error.isEmpty()) {
        QMessageBox::warning(&dialog, QStringLiteral("Invalid macro F%1").arg(i + 1), error);
        texts[i]->setFocus();
        return;
      }
      if (labels[i]->text().trimmed().isEmpty()) {
        QMessageBox::warning(&dialog, QStringLiteral("Invalid macro"),
                             QStringLiteral("F%1 needs a button label.").arg(i + 1));
        labels[i]->setFocus();
        return;
      }
    }
    dialog.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) { return; }
  if (jarlDob) {
    const QString dob = jarlDob->text().trimmed();
    if (!dob.isEmpty()) {
      static const QRegularExpression dobPattern(QStringLiteral("^[0-9]{2}\\.[0-9]{2}\\.[0-9]{4}$"));
      const QStringList parts = dob.split(QLatin1Char('.'));
      const QDate date(parts.size() == 3 ? parts.at(2).toInt() : 0,
                       parts.size() == 3 ? parts.at(1).toInt() : 0,
                       parts.size() == 3 ? parts.at(0).toInt() : 0);
      if (!dobPattern.match(dob).hasMatch() || !date.isValid() || date.year() < 1900) {
        QMessageBox::warning(this, QStringLiteral("JARL WW RTTY"), QStringLiteral("Date of birth must be DD.MM.YYYY or empty."));
        return;
      }
    }
    settings.setValue(QStringLiteral("contest/jarlBirthDate"), dob);
  }
  for (int i = 0; i < 4; ++i)
    settings.setValue(QStringLiteral("station/%1").arg(QLatin1String(stationKeys[i])),
                      stationEdits[i]->text().trimmed().toUpper());
  if (darcLocation) { settings.setValue(QStringLiteral("contest/darcLocation"), darcLocation->currentText()); }
  if (dokEdit) settings.setValue(QStringLiteral("contest/dok"),
                                   dokEdit->text().trimmed().toUpper().isEmpty() ? QStringLiteral("NM") : dokEdit->text().trimmed().toUpper());
  settings.setValue(QStringLiteral("rx/show_smeter"), showSMeter->isChecked());
  if (tci_.isConnected()) tci_.sendText(showSMeter->isChecked()
                                          ? QStringLiteral("rx_sensors_enable:true,500;") : QStringLiteral("rx_sensors_enable:false;"));
  if (!showSMeter->isChecked()) { tciRxDbmValid_ = false; }
  updateRxStatus();
  for (int i = 0; i < 8; ++i) {
    const QString base = contestMacroPrefix(contestMode_, contestType_) + QString::number(i) + QLatin1Char('/');
    settings.setValue(base + QStringLiteral("label"), labels[i]->text().trimmed());
    settings.setValue(base + QStringLiteral("text"), texts[i]->text());
  }
  refreshMacroButtons();
}


void MainWindow::logQsoAdif() {
  // Capture UTC at the Log click, as in the AppKit application.
  const QDateTime timestamp = QDateTime::currentDateTimeUtc();
  const QString utcDate = timestamp.toString(QStringLiteral("yyyyMMdd"));
  const QString utcTime = timestamp.toString(QStringLiteral("HHmmss"));
  const QSettings settings;
  const QString stationCall = settings.value(QStringLiteral("station/callsign")).toString().trimmed().toUpper();
  const QString myGrid = settings.value(QStringLiteral("station/locator")).toString().trimmed().toUpper();
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Log QSO — %1 %2 UTC").arg(utcDate, utcTime));
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout();
  auto field = [&dialog, form](const QString & label, const QString & value) {
    auto *edit = new QLineEdit(value, &dialog);
    form->addRow(label, edit);
    return edit;
  };
  auto *call = field(QStringLiteral("CALL*"), qsoCall_->text());
  auto *name = field(QStringLiteral("NAME"), qsoName_->text());
  auto *sent = field(QStringLiteral("RST-S*"), qsoSent_->text());
  auto *received = field(QStringLiteral("RST-R*"), qsoReceived_->text());
  auto *frequency = field(QStringLiteral("QRG (MHz)*"),
                          tciFrequencyHz_ > 0.0 ? QString::number(tciFrequencyHz_ / 1e6, 'f', 6) : QString());
  auto *grid = field(QStringLiteral("LOC"), QString());
  auto *qth = field(QStringLiteral("QTH"), QString());
  auto *comment = field(QStringLiteral("COMMENT"), QString());
  for (QLineEdit *edit : {call, name, sent, received, grid, qth})
    normalizeUpperEdit(edit);
  layout->addLayout(form);
  // UDP destination is configured in Setup, never in the QSO entry dialog.
  const bool sendUdp = settings.value(QStringLiteral("log/sendViaUdp"), true).toBool();
  const QString udpHost = settings.value(QStringLiteral("log/udpHost"),
                                         QStringLiteral("127.0.0.1")).toString();
  const int udpPort = settings.value(QStringLiteral("log/udpPort"), 12063).toInt();
  layout->addWidget(new QLabel(QStringLiteral("Station: %1    MY GRID: %2    MODE: RTTY")
                               .arg(stationCall.isEmpty() ? QStringLiteral("--") : stationCall,
                                    myGrid.isEmpty() ? QStringLiteral("--") : myGrid), &dialog));
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  // Validate inside the dialog. Invalid input must not discard the QSO
  // details the operator has already entered.
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    bool ok = false;
    const double mhz = frequency->text().trimmed().toDouble(&ok);
    if (call->text().trimmed().isEmpty() || sent->text().trimmed().isEmpty() ||
        received->text().trimmed().isEmpty() || stationCall.isEmpty() ||
        myGrid.isEmpty() || !ok || !std::isfinite(mhz) ||
        mhz <= 0.0 || mhz > 100000.0) {
      QMessageBox::warning(&dialog, QStringLiteral("Invalid QSO"),
                           QStringLiteral("CALL, QRG, RST-S, RST-R, MYCALL and MYLOC are required. "
                                          "Correct the fields or cancel the dialog."));
      return;
    }
    dialog.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) { return; }
  const QString callValue = call->text().trimmed().toUpper();
  const QString sentValue = sent->text().trimmed().toUpper();
  const QString receivedValue = received->text().trimmed().toUpper();
  bool freqOk = false;
  const double freqMHz = frequency->text().trimmed().toDouble(&freqOk);
  if (callValue.isEmpty() || sentValue.isEmpty() || receivedValue.isEmpty() ||
      stationCall.isEmpty() || myGrid.isEmpty() || !freqOk ||
      !std::isfinite(freqMHz) || freqMHz <= 0.0 || freqMHz > 100000.0) {
    QMessageBox::warning(this, QStringLiteral("QSO not logged"),
                         QStringLiteral("CALL, QRG, RST-S, RST-R, station callsign and station locator are required. "
                                        "Set MYCALL and MYLOC in Setup."));
    return;
  }
  // ADIF field lengths are UTF-8 byte counts, not QString character counts.
  QByteArray record;
  auto tag = [&record](const char *name, const QString & value) {
    const QByteArray bytes = value.trimmed().toUtf8();
    if (bytes.isEmpty()) { return; }
    record += '<';
    record += name;
    record += ':';
    record += QByteArray::number(bytes.size());
    record += '>';
    record += bytes;
  };
  tag("CALL", callValue);
  tag("FREQ", QString::number(freqMHz, 'f', 6));
  tag("MODE", QStringLiteral("RTTY"));
  tag("QSO_DATE", utcDate);
  tag("TIME_ON", utcTime);
  tag("QSO_DATE_OFF", utcDate);
  tag("TIME_OFF", utcTime);
  tag("RST_SENT", sentValue);
  tag("RST_RCVD", receivedValue);
  tag("STATION_CALLSIGN", stationCall);
  tag("MY_GRIDSQUARE", myGrid);
  tag("NAME", name->text().trimmed().toUpper());
  tag("GRIDSQUARE", grid->text().trimmed().toUpper());
  tag("QTH", qth->text().trimmed().toUpper());
  tag("COMMENT", comment->text().trimmed());
  record += "<EOR>\n";
  QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (documents.isEmpty()) { documents = QDir::homePath() + QStringLiteral("/Documents"); }
  const QString directory = documents + QStringLiteral("/rttyPad");
  const QString path = directory + QStringLiteral("/rttyPad.adi");
  if (!QDir().mkpath(directory)) {
    QMessageBox::critical(this, QStringLiteral("QSO not logged"),
                          QStringLiteral("Cannot create log directory: %1").arg(directory));
    return;
  }
  const QByteArray eol = logLineEnding(path);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
    QMessageBox::critical(this, QStringLiteral("QSO not logged"), file.errorString());
    return;
  }
  QByteArray output;
  if (file.size() == 0) {
    output = "rttyPad ADIF log\n<ADIF_VER:5>3.1.4\n<PROGRAMID:7>rttyPad\n<EOH>\n\n";
  }
  output += record;
  output = withLineEnding(output, eol);
  // A short write or failed flush must not leave a partial ADIF record.
  // Preserve the previous file length so an interrupted append can be
  // rolled back. Never clear the QSO fields on an I/O failure.
  const qint64 previousSize = file.size();
  const qint64 written = file.write(output);
  const bool flushed = written == output.size() && file.flush();
  if (!flushed) {
    const QString error = file.errorString();
    const bool rolledBack = file.resize(previousSize);
    file.close();
    QMessageBox::critical(this, QStringLiteral("QSO not logged"),
                          QStringLiteral("ADIF write failed: %1%2").arg(error,
                              rolledBack ? QString() : QStringLiteral(". Partial record may remain in the log.")));
    return;
  }
  file.close();
  if (file.error() != QFile::NoError) {
    QMessageBox::critical(this, QStringLiteral("QSO not logged"), file.errorString());
    return;
  }
  // Match native rttyTCI: transmit the ADIF record, not the file header,
  // and only after the local append has succeeded. Contest QSOs use a
  // separate log and never pass through this function.
  if (sendUdp) {
    QHostAddress address;
    const bool validHost = address.setAddress(udpHost) &&
                           address.protocol() == QAbstractSocket::IPv4Protocol;
    if (!validHost || udpPort < 1 || udpPort > 65535) {
      log_->appendPlainText(QStringLiteral("LOG UDP: invalid target (check Setup → UDP Log Settings)"));
    } else {
      QUdpSocket socket;
      const QByteArray payload = record.trimmed();
      const qint64 sent = socket.writeDatagram(payload, address, static_cast<quint16>(udpPort));
      if (sent != payload.size()) {
        const QString error = socket.errorString();
        log_->appendPlainText(QStringLiteral("LOG UDP: failed %1:%2 — %3")
                              .arg(udpHost).arg(udpPort).arg(error));
        QMessageBox::warning(this, QStringLiteral("Log UDP send failed"),
                             QStringLiteral("The QSO was saved locally to %1.\n\nUDP error: %2")
                             .arg(path, error));
      } else {
        log_->appendPlainText(QStringLiteral("LOG UDP: sent CALL=%1 to %2:%3")
                              .arg(callValue).arg(udpHost).arg(udpPort));
      }
    }
  }
  qsoCall_->clear();
  qsoName_->clear();
  qsoReceived_->setText(QStringLiteral("599"));
  decodePickTarget_ = DecodePickTarget::Call;
  qsoCall_->setFocus();
  statusBar()->showMessage(QStringLiteral("QSO saved: %1").arg(path), 8000);
}

void MainWindow::editStationSettings() {
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Station settings"));
  auto *layout = new QVBoxLayout(&dialog);
  auto *form = new QFormLayout();
  QSettings settings;
  const struct { const char *label; const char *key; const char *defaultValue; } fields[] = {
    {"Callsign:", "station/callsign", ""},
    {"Name:", "station/name", ""},
    {"QTH:", "station/qth", ""},
    {"Locator:", "station/locator", ""},
    {"RST:", "station/rst", "599"}
  };
  QLineEdit *edits[5] {};
  for (int i = 0; i < 5; ++i) {
    edits[i] = new QLineEdit(&dialog);
    edits[i]->setText(settings.value(QLatin1String(fields[i].key),
                                     QLatin1String(fields[i].defaultValue)).toString());
    normalizeUpperEdit(edits[i]);
    form->addRow(QLatin1String(fields[i].label), edits[i]);
  }
  layout->addLayout(form);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) { return; }
  for (int i = 0; i < 5; ++i) {
    QString value = edits[i]->text().trimmed();
    value = normalizedUpperField(value, true);
    settings.setValue(QLatin1String(fields[i].key), value);
  }
}

void MainWindow::runMacro(int index) {
  const bool tciOk = !tciConnMode_ || tci_.isConnected();
  if (!tciOk || tx_.state() == NativeRTTYTx::State::Draining) { return; }
  QSettings settings;
  const bool contest = contestMode_->isChecked();
  const bool cqww = contestType_->currentText() == QStringLiteral("CQ-WW-RTTY");
  const QString macroKey = contestMacroPrefix(contestMode_,
    contestType_) + QString::number(index) + QStringLiteral("/text");
  QString body = settings.value(macroKey, contest ? defaultContestMacroText(index, cqww,
    contestType_->currentText() == QStringLiteral("DARC-RTTY"), contestType_->currentText() == QStringLiteral("JARL-WW-RTTY")) : defaultMacroText(index)).toString();
  if (body.isEmpty()) { return; }
  // Reject invalid or unsupported tokens before sending rtty_start.
  const QString syntaxError = macroSyntaxError(body);
  if (!syntaxError.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Macro"), syntaxError);
    return;
  }
  // AppKit interprets TX/RX tokens as macro-level flags, independent
  // of their textual position. Native FSK transmits the expanded text
  // as one queue item and drains only when <RX> was requested.
  const QRegularExpression txTag(QStringLiteral("<TX>"), QRegularExpression::CaseInsensitiveOption);
  const QRegularExpression rxTag(QStringLiteral("<RX>"), QRegularExpression::CaseInsensitiveOption);
  const bool start = body.contains(txTag);
  const bool drain = body.contains(rxTag);
  body.remove(txTag);
  body.remove(rxTag);
  // Never send a directed reply without a destination callsign.
  // A missing <PCALL> previously disappeared silently and could result
  // in a misleading on-air response.
  if (body.contains(QStringLiteral("<PCALL>"), Qt::CaseInsensitive) &&
      qsoCall_->text().trimmed().isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Macro"),
                         QStringLiteral("Enter the other station's CALL before sending this macro."));
    qsoCall_->setFocus();
    return;
  }
  if (contest && body.contains(QStringLiteral("<MYDOK>"), Qt::CaseInsensitive)) {
    if (contestType_->currentText() != QStringLiteral("DARC-RTTY")) {
      QMessageBox::warning(this, QStringLiteral("Macro"), QStringLiteral("<MYDOK> is only available for DARC RTTY."));
      return;
    }
    body.replace(QStringLiteral("<MYDOK>"),
                 darcOperatingInDl(settings.value(QStringLiteral("station/callsign")).toString())
                 ? settings.value(QStringLiteral("contest/dok"), QStringLiteral("NM")).toString().trimmed().toUpper()
                 : QStringLiteral("%1").arg(contestSerial_->value(), 3, 10, QLatin1Char('0')),
                 Qt::CaseInsensitive);
  }
  // Resolve contest exchange before general tokens. CQ WW sends CQ zone,
  // SARTG sends a three-digit running serial. Neither is the received EXCH.
  if (contest) {
    const int number = contestSerial_->value();
    if (cqww && (number < 1 || number > 40)) {
      QMessageBox::warning(this, QStringLiteral("CQ WW RTTY"),
                           QStringLiteral("Own CQ zone must be between 1 and 40."));
      return;
    }
    QString exchange = contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") ? jarlExchange(settings)
                       : cqww ? QString::number(number)
                       : QStringLiteral("%1").arg(number, 3, 10, QLatin1Char('0'));
    if (contestType_->currentText() == QStringLiteral("DARC-RTTY") &&
        darcOperatingInDl(settings.value(QStringLiteral("station/callsign")).toString())) {
      exchange = settings.value(QStringLiteral("contest/dok"), QStringLiteral("NM")).toString().trimmed().toUpper();
    }
    body.replace(QStringLiteral("<EXCH>"), exchange, Qt::CaseInsensitive);
    body.replace(QStringLiteral("<MYZONE>"), cqww ? exchange : QString(), Qt::CaseInsensitive);
    // SERIAL is always the running number; EXCH is the on-air exchange.
    // In DARC, German operators send DOK/NM, not the serial.
    body.replace(QStringLiteral("<SERIAL>"), (cqww || contestType_->currentText() == QStringLiteral("JARL-WW-RTTY")) ? QString() :
                 QStringLiteral("%1").arg(number, 3, 10, QLatin1Char('0')), Qt::CaseInsensitive);
  } else if (body.contains(QRegularExpression(QStringLiteral("<(EXCH|MYZONE|SERIAL)>"),
      QRegularExpression::CaseInsensitiveOption))) {
    QMessageBox::warning(this, QStringLiteral("Macro"), QStringLiteral("Contest tokens require Contest Mode."));
    return;
  }
  // AppKit resolves partner and report tokens from the active QSO fields.
  const struct { const char *token; QString value; } qsoFields[] = {
    {"PCALL", qsoCall_->text().trimmed().toUpper()},
    {"PNAME", qsoName_->text().trimmed().toUpper()},
    {"RST", qsoSent_->text().trimmed().toUpper()},
    {"PRST", qsoReceived_->text().trimmed().toUpper()}
  };
  for (const auto &field : qsoFields) {
    const QString token = QStringLiteral("<%1>").arg(QLatin1String(field.token));
    if (field.value.isEmpty()) {
      // A missing partner call/name must not leave doubled spaces.
      body.replace(QRegularExpression(QStringLiteral("\\s*%1\\s*").arg(QRegularExpression::escape(token)),
                                      QRegularExpression::CaseInsensitiveOption), QStringLiteral(" "));
    } else {
      body.replace(token, field.value, Qt::CaseInsensitive);
    }
  }
  body.replace(QRegularExpression(QStringLiteral("<BR>|<CRLF>"), QRegularExpression::CaseInsensitiveOption),
               QStringLiteral("\r\n"));
  const struct { const char *token; const char *key; } fields[] = {
    {"MYCALL", "station/callsign"}, {"MYNAME", "station/name"},
    {"MYQTH", "station/qth"}, {"MYLOC", "station/locator"}
  };
  for (const auto &field : fields) {
    const QString token = QStringLiteral("<%1>").arg(QLatin1String(field.token));
    if (!body.contains(token, Qt::CaseInsensitive)) { continue; }
    const QString value = settings.value(QLatin1String(field.key)).toString().trimmed();
    if (value.isEmpty()) {
      QMessageBox::warning(this, QStringLiteral("Macro"),
                           QStringLiteral("%1 is not configured. Nothing transmitted.").arg(token));
      return;
    }
    body.replace(token, value, Qt::CaseInsensitive);
  }
  // AppKit expands these into ITA2 control bytes. deskHPSDR's native
  // rtty_text protocol chooses shifts itself, so the bytes are stripped
  // by NativeRTTYTx::escapePayload(). Preserve them until that boundary.
  body.replace(QStringLiteral("<LTRS>"), QString(QChar(0x0e)), Qt::CaseInsensitive);
  body.replace(QStringLiteral("<FIGS>"), QString(QChar(0x0f)), Qt::CaseInsensitive);
  if (body.contains(QLatin1Char('<')) || body.contains(QLatin1Char('>'))) {
    QMessageBox::warning(this, QStringLiteral("Macro"),
                         QStringLiteral("Unresolved macro token. Nothing transmitted."));
    return;
  }
  // Do not start RF for a macro containing only ITA2 shift controls.
  QString printable = body;
  printable.remove(QChar(0x0e));
  printable.remove(QChar(0x0f));
  if (printable.trimmed().isEmpty()) {
    if (drain && tx_.state() == NativeRTTYTx::State::Sending) { tx_.drain(); }
    return;
  }
  if (start && !tx_.start()) { return; }
  if (!body.isEmpty()) {
    if (!tx_.send(body)) { return; }
    appendTxTraffic(printable);
  }
  if (drain) { tx_.drain(); }
  updateTxUi();
}

// Match the AppKit pitch/shift convention: DIGL MARK at pitch, DIGU SPACE at pitch.
double MainWindow::rxCenterHz() const {
  return center_->value() + (tciMode_ == QStringLiteral("DIGU") ? -shift_->value() / 2.0 : shift_->value() / 2.0);
}

bool MainWindow::rxReverse() const {
  return reverse_->isChecked() != (tciMode_ == QStringLiteral("DIGU"));
}

void MainWindow::applyTciPitch() {
  if (iqPan_) iqPan_->setTones(tciMode_ == QStringLiteral("DIGU") ? diguOffsetHz_ : diglOffsetHz_,
                                 shift_->value(), tciMode_ == QStringLiteral("DIGU"), reverse_->isChecked());
  const double pitch = tciMode_ == QStringLiteral("DIGU") ? diguOffsetHz_ : diglOffsetHz_;
  if (pitch >= 100.0 && pitch <= 6000.0 && center_->value() != pitch) {
    center_->setValue(pitch);
  }
  updateRxStatus();
}

void MainWindow::updateRxStatus() {
  if (!rxStatus_) { return; }
  const QString qrg = tciFrequencyHz_ > 0.0
                      ? QString::number(tciFrequencyHz_ / 1e6, 'f', 6) + QStringLiteral(" MHz")
                      : QStringLiteral("—");
  const QString level = QSettings().value(QStringLiteral("rx/show_smeter"), false).toBool() && tciRxDbmValid_
                        ? QString::number(tciRxDbm_, 'f', 0) + QStringLiteral(" dBm")
                        : QStringLiteral("— dBm");
  rxStatus_->setText(QStringLiteral("MODE %1  |  QRG %2  |  %3")
                     .arg(tciMode_, qrg, level));
  setWindowTitle(QStringLiteral("rttyPad TCI by DL1BZ — %1 offset %2 Hz — %3")
                 .arg(tciMode_)
                 .arg(center_->value(), 0, 'f', 0)
                 .arg(tci_.isConnected() ? QStringLiteral("Connected (Native RTTY)")
                      : QStringLiteral("Disconnected")));
}

void MainWindow::startStandaloneRx()
{
  if (!standaloneRx_) {
    standaloneRx_ = new StandaloneRTTYRx(this);
    connect(standaloneRx_, &StandaloneRTTYRx::audioError,
            this, [](const QString &e){ qWarning() << "StandaloneRTTYRx:" << e; });
    standaloneRx_->setAudioCallback([this](const float *samples, int frames, double sr) {
      // Called on audio thread — copy and post to main thread
      QByteArray buf(reinterpret_cast<const char *>(samples), frames * (int)sizeof(float));
      QMetaObject::invokeMethod(this, [this, buf, frames, sr] {
        processRxSamples(reinterpret_cast<const float *>(buf.constData()), frames, sr, false);
      }, Qt::QueuedConnection);
    });
  }
  const int devIdx = QSettings().value(QStringLiteral("audio/rxDevice"), -1).toInt();
  const double sr   = QSettings().value(QStringLiteral("audio/rxSampleRate"), 48000.0).toDouble();
  standaloneRx_->setInputDevice(devIdx);
  standaloneRx_->setSampleRate(sr);
  standaloneRx_->start();
}

void MainWindow::stopStandaloneRx()
{
  if (standaloneRx_) standaloneRx_->stop();
}

void MainWindow::openAudioSettingsDialog()
{
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("Audio Device Settings"));
  auto *layout = new QVBoxLayout(&dlg);
  auto *form   = new QFormLayout();

  // RX input
  auto *rxCombo = new QComboBox(&dlg);
  rxCombo->addItem(QStringLiteral("[default]"));
  const auto rxDevices = StandaloneRTTYRx::availableInputDevices();
  for (const QString &d : rxDevices) rxCombo->addItem(d);
  const int savedRx = QSettings().value(QStringLiteral("audio/rxDevice"), -1).toInt();
  rxCombo->setCurrentIndex(savedRx + 1);  // 0 = default = index 0
  form->addRow(QStringLiteral("RX Input:"), rxCombo);

  // TX output
  auto *txCombo = new QComboBox(&dlg);
  txCombo->addItem(QStringLiteral("[default]"));
  const auto txDevices = StandaloneRTTYTx::availableOutputDevices();
  for (const QString &d : txDevices) txCombo->addItem(d);
  const int savedTx = QSettings().value(QStringLiteral("audio/txDevice"), -1).toInt();
  txCombo->setCurrentIndex(savedTx + 1);
  form->addRow(QStringLiteral("TX Output:"), txCombo);

  layout->addLayout(form);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

  if (dlg.exec() != QDialog::Accepted) return;

  const int rxIdx = rxCombo->currentIndex() - 1;  // -1 = default
  const int txIdx = txCombo->currentIndex() - 1;
  QSettings().setValue(QStringLiteral("audio/rxDevice"), rxIdx);
  QSettings().setValue(QStringLiteral("audio/txDevice"), txIdx);

  // Apply TX device
  if (tx_.standaloneEngine()) {
    tx_.standaloneEngine()->setOutputDevice(txIdx);
  }

  // Restart RX with new device
  if (standaloneRx_ && standaloneRx_->isRunning()) {
    stopStandaloneRx();
    startStandaloneRx();
  }
}


void MainWindow::beginRttySession() {
  ++sessionGeneration_;
  originalMode_.clear();
  requestedRttyPitchHz_ = center_->value();
  sessionStarted_ = false;
  snapshotModeReady_ = snapshotDiglReady_ = snapshotDiguReady_ = false;
  sessionChangedMode_ = sessionChangedDigl_ = sessionChangedDigu_ = false;
  sessionRestoring_ = false;
  lastAutoSetupBand_ = 0;
  pendingAutoSetupBand_ = false;
  autoSetupBandChangeSession_ = QSettings().value(QStringLiteral("tci/autoSetupBandChange"), true).toBool();
  // Freeze the preference for this connection; toggling Setup mid-session
  // must not discard a pending restoration or start an incomplete snapshot.
  autoSetupSession_ = QSettings().value(QStringLiteral("tci/autoSetup"), true).toBool();
  if (!autoSetupSession_) { return; }
  // Do not change the SDR until all three original values have been received.
  tci_.sendText(QStringLiteral("modulation:0;"));
  tci_.sendText(QStringLiteral("digl_offset;"));
  tci_.sendText(QStringLiteral("digu_offset;"));
  const quint64 generation = sessionGeneration_;
  auto *bandRetry = new QTimer(this);
  bandRetry->setInterval(200);
  connect(bandRetry, &QTimer::timeout, this, [this, generation, bandRetry] {
    if (generation != sessionGeneration_ || !tci_.isConnected() || sessionRestoring_) {
      bandRetry->stop();
      bandRetry->deleteLater();
      return;
    }
    applyPendingBandAutoSetup();
  });
  bandRetry->start();
  QTimer::singleShot(2000, this, [this, generation] {
    if (generation != sessionGeneration_ || !tci_.isConnected() || sessionRestoring_)
      return;
    if (!snapshotModeReady_ || !snapshotDiglReady_ || !snapshotDiguReady_)
      log_->appendPlainText(QStringLiteral("RTTY session: incomplete TCI snapshot; SDR mode/offset unchanged"));
  });
}

void MainWindow::maybeStartRttySession() {
  if (!autoSetupSession_ || !tci_.isConnected() || sessionRestoring_ || sessionStarted_ ||
      !snapshotModeReady_ || !snapshotDiglReady_ || !snapshotDiguReady_) {
    return;
  }
  sessionStarted_ = true;
  sessionChangedMode_ = originalMode_ != QStringLiteral("DIGL");
  sessionChangedDigl_ = qRound(originalDiglOffsetHz_) != qRound(requestedRttyPitchHz_);
  if (sessionChangedMode_) {
    tci_.sendText(QStringLiteral("modulation:0,DIGL;"));
  }
  if (sessionChangedDigl_) {
    tci_.sendText(QStringLiteral("digl_offset:%1;").arg(qRound(requestedRttyPitchHz_)));
  }
  // The DIGU offset is never modified automatically.
  applyPendingBandAutoSetup();
}

int MainWindow::tciBandForFrequency(double hz) const {
  static const struct { double low, high; int band; } bands[] = {
    {1800000, 2000000, 160}, {3500000, 4000000, 80},
    {5351000, 5367000, 60}, {7000000, 7300000, 40},
    {10100000, 10150000, 30}, {14000000, 14350000, 20},
    {18068000, 18168000, 17}, {21000000, 21450000, 15},
    {24890000, 24990000, 12}, {28000000, 29700000, 10},
    {50000000, 54000000, 6}
  };
  for (const auto &entry : bands) {
    if (hz >= entry.low && hz <= entry.high) return entry.band;
  }
  return 0;
}

void MainWindow::applyPendingBandAutoSetup() {
  if (!pendingAutoSetupBand_ || !autoSetupSession_ || !autoSetupBandChangeSession_ ||
      !sessionStarted_ || sessionRestoring_ || !tci_.isConnected()) return;
  if (tx_.state() == NativeRTTYTx::State::Sending ||
      tx_.state() == NativeRTTYTx::State::Draining) return;
  pendingAutoSetupBand_ = false;
  tci_.sendText(QStringLiteral("modulation:0,DIGL;"));
  tci_.sendText(QStringLiteral("digl_offset:%1;").arg(qRound(requestedRttyPitchHz_)));
  // These settings may differ from the initial snapshot even if the original
  // values already matched DIGL/pitch. Restore must still run on disconnect.
  sessionChangedMode_ = true;
  sessionChangedDigl_ = true;
}

void MainWindow::disconnectRestoringSession() {
  if (!tci_.isConnected()) {
    tci_.disconnectFromHost();
    return;
  }
  if (sessionRestoring_) { return; }
  // Stop an active native RTTY transmission before restoring SDR settings.
  // This also applies when Auto Setup is disabled.
  if (tx_.state() == NativeRTTYTx::State::Sending ||
      tx_.state() == NativeRTTYTx::State::Draining) {
    tx_.stop();
  }
  if (!autoSetupSession_) {
    ++sessionGeneration_;
    tci_.disconnectFromHost();
    return;
  }
  sessionRestoring_ = true;
  const quint64 generation = ++sessionGeneration_;
  if (sessionChangedDigl_ && snapshotDiglReady_) {
    tci_.sendText(QStringLiteral("digl_offset:%1;").arg(qRound(originalDiglOffsetHz_)));
  }
  if (sessionChangedDigu_ && snapshotDiguReady_) {
    tci_.sendText(QStringLiteral("digu_offset:%1;").arg(qRound(originalDiguOffsetHz_)));
  }
  if (sessionChangedMode_ && snapshotModeReady_) {
    tci_.sendText(QStringLiteral("modulation:0,%1;").arg(originalMode_));
  }
  // Allow queued WebSocket messages to reach the server before closing.
  // This is best-effort; a network failure cannot guarantee restoration.
  QTimer::singleShot(400, this, [this, generation] {
    if (generation == sessionGeneration_) tci_.disconnectFromHost();
  });
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (tci_.isConnected()) {
    event->ignore();
    closingAfterRestore_ = true;
    disconnectRestoringSession();
    return;
  }
  QMainWindow::closeEvent(event);
}

void MainWindow::processTciParameters(const QString &message) {
  QString line = message.trimmed();
  if (line.endsWith(QLatin1Char(';'))) { line.chop(1); }
  const qsizetype colon = line.indexOf(QLatin1Char(':'));
  if (colon < 0) { return; }
  const QString command = line.left(colon).trimmed().toLower();
  const QStringList args = line.mid(colon + 1).split(QLatin1Char(','));
  if (args.isEmpty()) { return; }
  bool ok = false;
  if (command == QStringLiteral("rx_filter_band") && args.size() >= 3 &&
      args.at(0).trimmed() == QStringLiteral("0")) {
    bool lowOk = false, highOk = false;
    const double low = args.at(1).trimmed().toDouble(&lowOk);
    const double high = args.at(2).trimmed().toDouble(&highOk);
    if (lowOk && highOk && iqPan_) iqPan_->setFilter(low, high);
  } else if (command == QStringLiteral("modulation") && args.size() >= 2 &&
      args.at(0).trimmed() == QStringLiteral("0")) {
    const QString mode = args.at(1).trimmed().toUpper();
    if (!snapshotModeReady_ && !sessionRestoring_ && !mode.isEmpty()) {
      originalMode_ = mode;
      snapshotModeReady_ = true;
      maybeStartRttySession();
    }
    if ((mode == QStringLiteral("DIGL") || mode == QStringLiteral("DIGU")) && mode != tciMode_) {
      tciMode_ = mode;
      applyTciPitch();
      configureRx();
    }
  } else if (command == QStringLiteral("digl_offset") || command == QStringLiteral("digu_offset")) {
    const double hz = args.at(0).trimmed().toDouble(&ok);
    // Zero is a valid DIGI offset (for example while the SDR is in LSB).
    // Do not confuse the value with whether the snapshot was received.
    if (ok && hz >= 0.0 && hz <= 6000.0) {
      if (command == QStringLiteral("digl_offset")) {
        if (!snapshotDiglReady_ && !sessionRestoring_) {
          originalDiglOffsetHz_ = hz;
          snapshotDiglReady_ = true;
        }
        diglOffsetHz_ = hz;
      } else {
        if (!snapshotDiguReady_ && !sessionRestoring_) {
          originalDiguOffsetHz_ = hz;
          snapshotDiguReady_ = true;
        }
        diguOffsetHz_ = hz;
      }
      maybeStartRttySession();
      if ((command == QStringLiteral("digl_offset") && tciMode_ == QStringLiteral("DIGL")) ||
          (command == QStringLiteral("digu_offset") && tciMode_ == QStringLiteral("DIGU"))) {
        applyTciPitch();
      }
    }
  } else if (command == QStringLiteral("rx_sensors") && args.size() >= 2 &&
             args.at(0).trimmed() == QStringLiteral("0")) {
    const double dbm = args.at(1).trimmed().toDouble(&ok);
    if (ok && dbm > -250.0 && dbm < 100.0) {
      tciRxDbm_ = dbm;
      tciRxDbmValid_ = true;
      updateRxStatus();
    }
  } else if (command == QStringLiteral("vfo") || command == QStringLiteral("dds") ||
             command == QStringLiteral("frequency") || command == QStringLiteral("rx_frequency") ||
             command == QStringLiteral("tx_frequency")) {
    // TCI messages may contain receiver/VFO indices before the frequency.
    for (qsizetype i = args.size(); i-- > 0;) {
      const double hz = args.at(i).trimmed().toDouble(&ok);
      if (ok && hz >= 100000.0 && hz <= 10000000000.0) {
        if (command == QStringLiteral("vfo") || command == QStringLiteral("frequency") ||
            command == QStringLiteral("rx_frequency")) {
          const int band = tciBandForFrequency(hz);
          if (band != 0) {
            if (lastAutoSetupBand_ != 0 && lastAutoSetupBand_ != band &&
                autoSetupSession_ && autoSetupBandChangeSession_) pendingAutoSetupBand_ = true;
            lastAutoSetupBand_ = band;
          }
        }
        tciFrequencyHz_ = hz;
        if (iqPan_) iqPan_->setFrequency(hz);
        applyPendingBandAutoSetup();
        updateRxStatus();
        break;
      }
    }
  }
}

void MainWindow::sendNativeTxParameters() {
  if (!txFill_ || !txStopBits_ || !txUos_ || !txStartCrlf_ || !idleTimeout_) {
    return;
  }
  QSettings settings;
  settings.setValue(QStringLiteral("tx/fill"), txFill_->currentText());
  settings.setValue(QStringLiteral("tx/stopBits"), txStopBits_->currentText());
  settings.setValue(QStringLiteral("tx/uos"), txUos_->isChecked());
  settings.setValue(QStringLiteral("tx/startCrlf"), txStartCrlf_->isChecked());
  settings.setValue(QStringLiteral("tx/idleTimeout"), idleTimeout_->value());
  if (!tci_.isConnected()) {
    return;
  }
  // rtty_* commands are deskHPSDR extensions — do not send them to other servers.
  if (tci_.isDeskHPSDR()) {
    tci_.sendText(QStringLiteral("rtty_baud:%1;").arg(baud_->value(), 0, 'f', 4));
    tci_.sendText(QStringLiteral("rtty_shift:%1;").arg(shift_->value(), 0, 'f', 1));
    tci_.sendText(QStringLiteral("rtty_fill:%1;").arg(txFill_->currentText()));
    tci_.sendText(QStringLiteral("rtty_stop_bits:%1;").arg(txStopBits_->currentText()));
    tci_.sendText(QStringLiteral("rtty_uos:%1;").arg(txUos_->isChecked() ? 1 : 0));
    tci_.sendText(QStringLiteral("rtty_start_crlf:%1;").arg(txStartCrlf_->isChecked() ? 1 : 0));
    tci_.sendText(QStringLiteral("rtty_idle_timeout:%1;").arg(idleTimeout_->value()));
  }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
  if (watched == txInput_ && txOffDraining_ && event->type() == QEvent::KeyPress) {
    return true;
  }
  if (watched == txInput_ && liveTxActive_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    const int code = key->key();
    const bool space = code == Qt::Key_Space;
    const bool enter = code == Qt::Key_Return || code == Qt::Key_Enter;
    QTextCursor cursor = txInput_->textCursor();
    const QString contents = txInput_->toPlainText();
    // Transmitted characters are immutable until TX OFF clears the buffer.
    if (cursor.selectionStart() < liveTxCommitted_ || cursor.position() < liveTxCommitted_) {
      if (code == Qt::Key_Backspace || code == Qt::Key_Delete ||
          (!key->text().isEmpty() && !(key->modifiers() & Qt::ControlModifier))) {
        return true;
      }
    }
    if (space || enter) {
      if (cursor.hasSelection() || cursor.position() != contents.size()) { return true; }
      const QString pending = contents.mid(liveTxCommitted_);
      const QString payload = pending + (enter ? QStringLiteral("\r\n") : QStringLiteral(" "));
      if (!tx_.send(payload)) { return true; }
      appendTxTraffic(payload);
      // Keep the complete visible text, including the delimiter.
      cursor.insertText(enter ? QStringLiteral("\n") : QStringLiteral(" "));
      liveTxCommitted_ = txInput_->toPlainText().size();
      return true;
    }
  }

  if (event->type() == QEvent::FocusIn) {
    if (watched == qsoCall_) { decodePickTarget_ = DecodePickTarget::Call; }
    else if (watched == qsoName_) { decodePickTarget_ = DecodePickTarget::Name; }
  }
  return QMainWindow::eventFilter(watched, event);
}

// Contest records are tab-separated and intentionally independent of ADIF.
// A successful QSaveFile::commit() atomically replaces the complete log.
QString MainWindow::contestLogPath() const {
  QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (documents.isEmpty()) { documents = QDir::homePath() + QStringLiteral("/Documents"); }
  return documents + QStringLiteral("/rttyPad/contest-qsos.tsv");
}

QString MainWindow::contestBand() const {
  const double mhz = tciFrequencyHz_ / 1e6;
  if (contestType_->currentText() == QStringLiteral("DARC-RTTY") &&
      !(mhz >= 3.5 && mhz <= 3.8) && !(mhz >= 7.0 && mhz <= 7.2)) { return QString(); }
  if (contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") && mhz < 3.5) return QString();
  if (mhz >= 1.8 && mhz <= 2.0) { return QStringLiteral("160M"); }
  if (mhz >= 3.5 && mhz <= 4.0) { return QStringLiteral("80M"); }
  if (mhz >= 7.0 && mhz <= 7.3) { return QStringLiteral("40M"); }
  if (mhz >= 14.0 && mhz <= 14.35) { return QStringLiteral("20M"); }
  if (mhz >= 21.0 && mhz <= 21.45) { return QStringLiteral("15M"); }
  if (mhz >= 28.0 && mhz <= 29.7) { return QStringLiteral("10M"); }
  return QString();
}

void MainWindow::updateContestUi() {
  if (!contestMode_) { return; }
  const bool enabled = contestMode_->isChecked();
  contestType_->setVisible(true);
  contestType_->setEnabled(enabled);
  if (auto *fields = findChild<QWidget *>(QStringLiteral("contestFields"))) {
    fields->setVisible(enabled);
  }
  if (auto *button = findChild<QPushButton *>(QStringLiteral("contestExport"))) {
    button->setVisible(true);
    button->setEnabled(enabled);
  }
  dupeLabel_->clear();
  if (!enabled) { contestSerial_->setEnabled(true); return; }
  const bool cqww = contestType_->currentText() == QStringLiteral("CQ-WW-RTTY");
  const bool darc = contestType_->currentText() == QStringLiteral("DARC-RTTY");
  const bool jarl = contestType_->currentText() == QStringLiteral("JARL-WW-RTTY");
  if (auto *label = findChild<QLabel *>(QStringLiteral("contestExchangeLabel"))) label->setText(jarl ? QStringLiteral("AGE-R") : QStringLiteral("EXCH"));
  if (auto *label = findChild<QLabel *>(QStringLiteral("contestOwnLabel"))) label->setText(jarl ? QStringLiteral("AGE") : (cqww ? QStringLiteral("ZONE") : QStringLiteral("NR")));
  contestSerial_->setRange(jarl ? -1 : 1, cqww ? 40 : (jarl ? 99 : 99999));
  const bool dlOperator = darc && darcOperatingInDl(QSettings().value(QStringLiteral("station/callsign")).toString());
  contestSerial_->setEnabled(!dlOperator);
  contestSerial_->setSpecialValueText(jarl ? QStringLiteral("AUTO") : QString());
  contestSerial_->setToolTip(jarl ? QStringLiteral("Own AGE 00–99; AUTO uses DOB (or 00).") : cqww ? QStringLiteral("Own CQ zone (1–40), not a running serial")
                             : (darc ? QStringLiteral("Next DARC short contest serial number (Non-DL only)")
                                : QStringLiteral("Next SARTG serial number")));
  const QString call = qsoCall_->text().trimmed().toUpper();
  const QString band = contestBand();
  bool duplicate = false;
  QFile file(contestLogPath());
  if (!call.isEmpty() && !band.isEmpty() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!file.atEnd()) {
      const QList<QByteArray> fields = file.readLine().trimmed().split('\t');
      if (fields.size() >= 8 && QString::fromUtf8(fields.at(0)) == contestType_->currentText() &&
          QString::fromUtf8(fields.at(3)) == band &&
          QString::fromUtf8(fields.at(4)) == call) { duplicate = true; break; }
    }
  }
  dupeLabel_->setText(duplicate ? QStringLiteral("DUPE") : QStringLiteral(""));
  dupeLabel_->setStyleSheet(duplicate ? QStringLiteral("color: #ff4040; font-weight: bold;") : QString());
}

void MainWindow::logContestQso() {
  const QString call = qsoCall_->text().trimmed().toUpper();
  const QString band = contestBand();
  const QString exchange = contestExchange_->text().trimmed().toUpper();
  const bool darc = contestType_->currentText() == QStringLiteral("DARC-RTTY");
  if (call.isEmpty() || band.isEmpty() || (!darc && exchange.isEmpty()) ||
      qsoSent_->text().trimmed().isEmpty() || qsoReceived_->text().trimmed().isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Contest QSO"),
                         QStringLiteral("CALL, valid contest band, RST and received exchange are required."));
    return;
  }
  // Normalize the received exchange before persisting it.  The native
  // AppKit logger applies the same contest-specific numeric constraints.
  static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
  const bool cqww = contestType_->currentText() == QStringLiteral("CQ-WW-RTTY");
  const bool jarl = contestType_->currentText() == QStringLiteral("JARL-WW-RTTY");
  QString normalizedExchange;
  if (darc) {
    // AppKit: German calls require DOK or NM; DX stations send a
    // numeric serial (empty is logged as 000).
    if (isDarcGermanCall(call)) {
      if (exchange.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("DARC RTTY"),
                             QStringLiteral("German stations require a received DOK or NM."));
        return;
      }
      static const QRegularExpression dok(QStringLiteral("^[A-Z0-9]{1,8}$"));
      if (!dok.match(exchange).hasMatch()) {
        QMessageBox::warning(this, QStringLiteral("DARC RTTY"),
                             QStringLiteral("Received DOK/NM must contain 1–8 letters or digits."));
        return;
      }
      normalizedExchange = exchange;
    } else {
      if (!exchange.isEmpty() && (!digits.match(exchange).hasMatch() || exchange.size() > 3)) {
        QMessageBox::warning(this, QStringLiteral("DARC RTTY"),
                             QStringLiteral("DX exchange must be a numeric serial of at most three digits."));
        return;
      }
      normalizedExchange = QStringLiteral("%1").arg(exchange.isEmpty() ? 0 : exchange.toInt(), 3, 10, QLatin1Char('0'));
    }
  } else {
    if (!digits.match(exchange).hasMatch() || (jarl && exchange.size() > 2)) {
      QMessageBox::warning(this, QStringLiteral("Contest QSO"),
                           jarl ? QStringLiteral("JARL requires a numeric age (00–99).") : cqww ? QStringLiteral("CQ WW requires a numeric CQ zone (01–40).")
                           : QStringLiteral("SARTG requires a numeric serial number."));
      return;
    }
    bool numberOk = false;
    const int receivedNumber = exchange.toInt(&numberOk);
    if (!numberOk || (jarl ? receivedNumber < 0 || receivedNumber > 99 : receivedNumber < 1) || (cqww && receivedNumber > 40)) {
      QMessageBox::warning(this, QStringLiteral("Contest QSO"),
                           jarl ? QStringLiteral("Received age must be between 00 and 99.") : cqww ? QStringLiteral("Received CQ zone must be between 01 and 40.")
                           : QStringLiteral("Received serial number must be positive."));
      return;
    }
    normalizedExchange = QStringLiteral("%1").arg(receivedNumber, (cqww || jarl) ? 2 : 3, 10, QLatin1Char('0'));
  }
  updateContestUi();
  if (!dupeLabel_->text().isEmpty() &&
      QMessageBox::question(this, QStringLiteral("Duplicate QSO"),
                            QStringLiteral("This callsign is already logged on this band. Log again?")) != QMessageBox::Yes) { return; }
  const QString path = contestLogPath();
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    QMessageBox::critical(this, QStringLiteral("Contest QSO"), QStringLiteral("Cannot create contest log directory."));
    return;
  }
  QByteArray previous;
  QFile source(path);
  if (source.exists()) {
    if (!source.open(QIODevice::ReadOnly)) {
      QMessageBox::critical(this, QStringLiteral("Contest QSO"), source.errorString());
      return;
    }
    previous = source.readAll();
    if (source.error() != QFile::NoError) {
      QMessageBox::critical(this, QStringLiteral("Contest QSO"), source.errorString());
      return;
    }
  }
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const QStringList fields = {
    contestType_->currentText(), now.toString(QStringLiteral("yyyy-MM-dd")),
    now.toString(QStringLiteral("HHmm")), band, call,
    QString::number(qRound64(tciFrequencyHz_ / 1000.0)),
    qsoSent_->text().trimmed(), qsoReceived_->text().trimmed(),
    normalizedExchange,
    darc && darcOperatingInDl(QSettings().value(QStringLiteral("station/callsign")).toString())
    ? QSettings().value(QStringLiteral("contest/dok"), QStringLiteral("NM")).toString().trimmed().toUpper()
    : (darc ? QStringLiteral("%1").arg(contestSerial_->value(), 3, 10, QLatin1Char('0'))
    : (contestType_->currentText() == QStringLiteral("JARL-WW-RTTY") ? jarlExchange(QSettings()) : QString::number(contestSerial_->value()))),
    QStringLiteral("DX")
  };
  for (const QString &field : fields) {
    if (field.contains(QLatin1Char('\t')) || field.contains(QLatin1Char('\n')) || field.contains(QLatin1Char('\r'))) {
      QMessageBox::warning(this, QStringLiteral("Contest QSO"), QStringLiteral("Invalid control character in QSO."));
      return;
    }
  }
  if (!previous.isEmpty() && !previous.endsWith('\n')) { previous += '\n'; }
  previous += fields.join(QLatin1Char('\t')).toUtf8() + '\n';
  previous = withLineEnding(previous, QByteArray("\r\n"));
  QSaveFile output(path);
  if (!output.open(QIODevice::WriteOnly) || output.write(previous) != previous.size() || !output.commit()) {
    QMessageBox::critical(this, QStringLiteral("Contest QSO"),
                          QStringLiteral("Contest log could not be saved: %1").arg(output.errorString()));
    return;
  }
  if (contestType_->currentText() == QStringLiteral("SARTG-WW-RTTY") ||
      (darc && !darcOperatingInDl(QSettings().value(QStringLiteral("station/callsign")).toString()))) {
    contestSerial_->setValue(contestSerial_->value() + 1);
  }
  qsoCall_->clear();
  qsoName_->clear();
  contestExchange_->clear();
  qsoReceived_->setText(QStringLiteral("599"));
  decodePickTarget_ = DecodePickTarget::Call;
  qsoCall_->setFocus();
  statusBar()->showMessage(QStringLiteral("Contest QSO saved"), 6000);
  updateContestUi();
}

void MainWindow::showContestLog() {
  const QString selectedContest = contestType_->currentText();
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Contest Log — %1").arg(selectedContest));
  dialog.resize(960, 480);
  auto *layout = new QVBoxLayout(&dialog);
  auto *table = new QTableWidget(&dialog);
  table->setColumnCount(10);
  table->setHorizontalHeaderLabels({QStringLiteral("UTC Date"), QStringLiteral("Time"),
                                    QStringLiteral("Call"), QStringLiteral("Band"), QStringLiteral("kHz"),
                                    QStringLiteral("Sent"), QStringLiteral("Rcv"), QStringLiteral("Exchange"), QStringLiteral("NR"), QStringLiteral("QTH-R")});
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(table);
  auto *scoreLabel = new QLabel(&dialog);
  if (selectedContest == QStringLiteral("DARC-RTTY")) { layout->addWidget(scoreLabel); }
  auto *actions = new QHBoxLayout();
  auto *deleteButton = new QPushButton(QStringLiteral("Delete selected QSO"), &dialog);
  auto *editQsoButton = new QPushButton(QStringLiteral("Edit QSO…"), &dialog);
  auto *editLocationButton = new QPushButton(QStringLiteral("Edit QTH-R…"), &dialog);
  auto *exportButton = new QPushButton(QStringLiteral("Export Cabrillo…"), &dialog);
  auto *closeButton = new QPushButton(QStringLiteral("Close"), &dialog);
  actions->addWidget(deleteButton);
  actions->addWidget(editQsoButton);
  actions->addWidget(editLocationButton);
  actions->addWidget(exportButton);
  actions->addStretch();
  actions->addWidget(closeButton);
  layout->addLayout(actions);
  // Keep the original raw records (including other contests) so deleting a
  // visible QSO never modifies unrelated records or silently normalizes data.
  QList<QByteArray> records;
  auto reload = [&]() -> bool {
    QFile file(contestLogPath());
    records.clear();
    table->setRowCount(0);
    if (!file.exists()) {
    if (selectedContest == QStringLiteral("DARC-RTTY")) {
        scoreLabel->setText(QStringLiteral("Estimated: QSOs 0   Prefix 0   DOK 0   Mult 0   Score 0"));
      }
      return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::critical(&dialog, QStringLiteral("Contest Log"), file.errorString());
      return false;
    }
    while (!file.atEnd()) {
    QByteArray line = file.readLine();
      if (line.isEmpty() && file.error() != QFile::NoError) {
        QMessageBox::critical(&dialog, QStringLiteral("Contest Log"), file.errorString());
        return false;
      }
      const int recordIndex = records.size();
      records.append(line);
      const QList<QByteArray> fields = line.trimmed().split('\t');
      if ((fields.size() != 10 && fields.size() != 11) || QString::fromUtf8(fields.at(0)) != selectedContest) { continue; }
      const int row = table->rowCount();
      table->insertRow(row);
      const int columns[] = {1, 2, 4, 3, 5, 6, 7, 8, 9};
      for (int col = 0; col < 10; ++col) {
        const QString value = col == 9 ? (fields.size() == 11 ? QString::fromUtf8(fields.at(10)) : QStringLiteral("DX"))
                              : QString::fromUtf8(fields.at(columns[col]));
        auto *item = new QTableWidgetItem(value);
        if (col == 0) { item->setData(Qt::UserRole, recordIndex); }
        table->setItem(row, col, item);
      }
    }
    if (selectedContest == QStringLiteral("DARC-RTTY")) {
    const ShortRyScore score = shortRyScore(contestLogPath());
      scoreLabel->setText(QStringLiteral("Estimated: QSOs %1   Prefix %2   DOK %3   Mult %4   Score %5")
                          .arg(score.qsos).arg(score.prefixes).arg(score.doks)
                          .arg(score.prefixes + score.doks)
                          .arg(score.qsos * (score.prefixes + score.doks)));
    }
    if (file.error() != QFile::NoError) {
    QMessageBox::critical(&dialog, QStringLiteral("Contest Log"), file.errorString());
      return false;
    }
    return true;
  };
  if (!reload()) { return; }
  connect(deleteButton, &QPushButton::clicked, &dialog, [&] {
    const int row = table->currentRow();
    if (row < 0 || !table->item(row, 0)) return;
    const int index = table->item(row, 0)->data(Qt::UserRole).toInt();
    if (index < 0 || index >= records.size()) return;
    if (QMessageBox::question(&dialog, QStringLiteral("Delete contest QSO"),
                              QStringLiteral("Permanently delete the selected QSO?")) != QMessageBox::Yes) return;
    QByteArray updated;
    for (int i = 0; i < records.size(); ++i) {
      if (i != index) { updated += records.at(i); }
    }
    updated = withLineEnding(updated, QByteArray("\r\n"));
    QSaveFile output(contestLogPath());
    if (!output.open(QIODevice::WriteOnly) || output.write(updated) != updated.size() || !output.commit()) {
      QMessageBox::critical(&dialog, QStringLiteral("Contest Log"), output.errorString());
      return;
    }
    reload();
    updateContestUi();
  });
  connect(editQsoButton, &QPushButton::clicked, &dialog, [&] {
    const int row = table->currentRow();
    if (row < 0 || !table->item(row, 0)) return;
    const int index = table->item(row, 0)->data(Qt::UserRole).toInt();
    if (index < 0 || index >= records.size()) return;
    QList<QByteArray> fields = records.at(index).trimmed().split('\t');
    if ((fields.size() != 10 && fields.size() != 11) ||
        QString::fromUtf8(fields.at(0)) != selectedContest) return;

    QDialog editor(&dialog);
    editor.setWindowTitle(QStringLiteral("Edit Contest QSO"));
    auto *form = new QFormLayout(&editor);
    auto *date = new QDateEdit(&editor);
    date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    date->setCalendarPopup(true);
    date->setDate(QDate::fromString(QString::fromUtf8(fields.at(1)), QStringLiteral("yyyy-MM-dd")));
    auto *time = new QTimeEdit(&editor);
    time->setDisplayFormat(QStringLiteral("HH:mm"));
    time->setTime(QTime::fromString(QString::fromUtf8(fields.at(2)), QStringLiteral("HHmm")));
    auto *band = new QComboBox(&editor);
    band->addItems({QStringLiteral("160M"), QStringLiteral("80M"), QStringLiteral("40M"),
                    QStringLiteral("20M"), QStringLiteral("15M"), QStringLiteral("10M")});
    band->setCurrentText(QString::fromUtf8(fields.at(3)));
    if (selectedContest == QStringLiteral("DARC-RTTY")) {
      band->removeItem(band->findText(QStringLiteral("160M")));
      for (const QString &unused : {QStringLiteral("20M"), QStringLiteral("15M"), QStringLiteral("10M")})
        band->removeItem(band->findText(unused));
    } else if (selectedContest == QStringLiteral("JARL-WW-RTTY")) {
      band->removeItem(band->findText(QStringLiteral("160M")));
    }
    auto *call = new QLineEdit(QString::fromUtf8(fields.at(4)), &editor);
    auto *sentRst = new QLineEdit(QString::fromUtf8(fields.at(6)), &editor);
    auto *receivedRst = new QLineEdit(QString::fromUtf8(fields.at(7)), &editor);
    auto *exchange = new QLineEdit(QString::fromUtf8(fields.at(8)), &editor);
    auto *sentExchange = new QLineEdit(QString::fromUtf8(fields.at(9)), &editor);
    auto *frequency = new QLineEdit(QString::fromUtf8(fields.at(5)), &editor);
    for (QLineEdit *edit : {call, sentRst, receivedRst, exchange, sentExchange})
    normalizeUpperEdit(edit);
    form->addRow(QStringLiteral("UTC date"), date);
    form->addRow(QStringLiteral("UTC time"), time);
    form->addRow(QStringLiteral("Band"), band);
    form->addRow(QStringLiteral("CALL"), call);
    form->addRow(QStringLiteral("Frequency (kHz)"), frequency);
    form->addRow(QStringLiteral("RST sent"), sentRst);
    form->addRow(QStringLiteral("RST received"), receivedRst);
    form->addRow(QStringLiteral("Exchange received"), exchange);
    form->addRow(selectedContest == QStringLiteral("CQ-WW-RTTY")
                 ? QStringLiteral("Own CQ zone") : (selectedContest == QStringLiteral("DARC-RTTY")
                     ? QStringLiteral("Exchange sent (DOK/NM or serial)") : (selectedContest == QStringLiteral("JARL-WW-RTTY") ? QStringLiteral("Own AGE") : QStringLiteral("Serial sent"))), sentExchange);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &editor, [&] {
      static const QRegularExpression callPattern(QStringLiteral("^[A-Z0-9/]{3,20}$"));
      static const QRegularExpression rstPattern(QStringLiteral("^[0-9]{2,3}$"));
      static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
      const QString newCall = call->text().trimmed().toUpper();
      const QString newSent = sentRst->text().trimmed();
      const QString newReceived = receivedRst->text().trimmed();
      const QString newExchange = exchange->text().trimmed();
      const QString newSentExchange = sentExchange->text().trimmed();
      bool freqOk = false, rxOk = false, txOk = false;
      const int khz = frequency->text().trimmed().toInt(&freqOk);
      const int rx = newExchange.toInt(&rxOk);
      const int tx = newSentExchange.toInt(&txOk);
      const bool cqww = selectedContest == QStringLiteral("CQ-WW-RTTY");
      const bool darc = selectedContest == QStringLiteral("DARC-RTTY");
      const bool jarl = selectedContest == QStringLiteral("JARL-WW-RTTY");
      static const QRegularExpression darcExchange(QStringLiteral("^[A-Z0-9]{1,8}$"));
      const double mhz = khz / 1000.0;
      QString frequencyBand;
      if (mhz >= 1.8 && mhz <= 2.0) frequencyBand = QStringLiteral("160M");
      else if (mhz >= 3.5 && mhz <= 4.0) frequencyBand = QStringLiteral("80M");
      else if (mhz >= 7.0 && mhz <= 7.3) frequencyBand = QStringLiteral("40M");
      else if (mhz >= 14.0 && mhz <= 14.35) frequencyBand = QStringLiteral("20M");
      else if (mhz >= 21.0 && mhz <= 21.45) frequencyBand = QStringLiteral("15M");
      else if (mhz >= 28.0 && mhz <= 29.7) frequencyBand = QStringLiteral("10M");
      const bool darcFrequencyOk = !darc || (mhz >= 3.5 && mhz <= 3.8) || (mhz >= 7.0 && mhz <= 7.2);
      const bool validDateTime = date->date().isValid() && time->time().isValid();
      const bool darcGermanRx = darc && isDarcGermanCall(newCall);
      const bool validRxExchange = darc ? (darcGermanRx
          ? darcExchange.match(newExchange).hasMatch()
          : (newExchange.isEmpty() || (digits.match(newExchange).hasMatch() && newExchange.size() <= 3)))
        : digits.match(newExchange).hasMatch() && rxOk &&
          (jarl ? newExchange.size() <= 2 && rx >= 0 && rx <= 99
                : (cqww ? rx >= 1 && rx <= 40 : rx >= 1));
      const bool validTxExchange = darc ? (darcExchange.match(newSentExchange).hasMatch() &&
          (!digits.match(newSentExchange).hasMatch() || newSentExchange.size() <= 3))
        : digits.match(newSentExchange).hasMatch() && txOk &&
          (jarl ? newSentExchange.size() <= 2 && tx >= 0 && tx <= 99
                : (cqww ? tx >= 1 && tx <= 40 : tx >= 1));
      if (!callPattern.match(newCall).hasMatch() ||
          !rstPattern.match(newSent).hasMatch() || !rstPattern.match(newReceived).hasMatch() ||
          !validRxExchange || !validTxExchange || !freqOk || khz <= 0 ||
          !validDateTime || frequencyBand != band->currentText() || !darcFrequencyOk) {
      QMessageBox::warning(&editor, QStringLiteral("Contest QSO"),
                           QStringLiteral("Check UTC date/time, CALL, band/frequency, RST and contest exchanges."));
        return;
      }
      fields[1] = date->date().toString(QStringLiteral("yyyy-MM-dd")).toUtf8();
      fields[2] = time->time().toString(QStringLiteral("HHmm")).toUtf8();
      fields[3] = band->currentText().toUtf8();
      fields[4] = newCall.toUtf8();
      fields[5] = QByteArray::number(khz);
      fields[6] = newSent.toUtf8();
      fields[7] = newReceived.toUtf8();
      fields[8] = darc ? (darcGermanRx ? newExchange.toUtf8()
                          : QStringLiteral("%1").arg(newExchange.isEmpty() ? 0 : rx, 3, 10, QLatin1Char('0')).toUtf8())
                       : QStringLiteral("%1").arg(rx, (cqww || jarl) ? 2 : 3, 10, QLatin1Char('0')).toUtf8();
      fields[9] = darc ? newSentExchange.toUtf8()
                       : QStringLiteral("%1").arg(tx, jarl ? 2 : (cqww ? 1 : 3), 10, QLatin1Char('0')).toUtf8();
      QByteArray updated;
      for (int i = 0; i < records.size(); ++i)
      updated += i == index ? fields.join('\t') + '\n' : records.at(i);
      updated = withLineEnding(updated, QByteArray("\r\n"));
      QSaveFile output(contestLogPath());
      if (!output.open(QIODevice::WriteOnly) || output.write(updated) != updated.size() || !output.commit()) {
        QMessageBox::critical(&editor, QStringLiteral("Contest Log"), output.errorString());
          return;
        }
      editor.accept();
    });
    if (editor.exec() == QDialog::Accepted) {
      if (reload() && row < table->rowCount()) { table->selectRow(row); }
      updateContestUi();
    }
  });
  editLocationButton->setEnabled(selectedContest == QStringLiteral("CQ-WW-RTTY"));
  connect(editLocationButton, &QPushButton::clicked, &dialog, [&] {
    const int row = table->currentRow();
    if (row < 0 || !table->item(row, 0)) return;
    const int index = table->item(row, 0)->data(Qt::UserRole).toInt();
    if (index < 0 || index >= records.size()) return;
    QList<QByteArray> fields = records.at(index).trimmed().split('\t');
    if ((fields.size() != 10 && fields.size() != 11) ||
        QString::fromUtf8(fields.at(0)) != QStringLiteral("CQ-WW-RTTY")) return;
    const QString oldValue = fields.size() == 11 ? QString::fromUtf8(fields.at(10)) : QStringLiteral("DX");
    bool accepted = false;
    const QString value = QInputDialog::getText(&dialog, QStringLiteral("Received QTH-R"),
      QStringLiteral("State/province for continental US/Canada, otherwise DX:"),
      QLineEdit::Normal, oldValue, &accepted).trimmed().toUpper();
    if (!accepted) return;
    if (!validCQWWQthR(value)) {
      QMessageBox::warning(&dialog, QStringLiteral("QTH-R"),
                           QStringLiteral("Use DX or a valid state/province abbreviation."));
      return;
    }
    if (fields.size() == 10) fields.append(value.toUtf8());
    else { fields[10] = value.toUtf8(); }
    QByteArray updated;
    for (int i = 0; i < records.size(); ++i)
      updated += i == index ? fields.join('\t') + '\n' : records.at(i);
    updated = withLineEnding(updated, QByteArray("\r\n"));
    QSaveFile output(contestLogPath());
    if (!output.open(QIODevice::WriteOnly) || output.write(updated) != updated.size() || !output.commit()) {
      QMessageBox::critical(&dialog, QStringLiteral("Contest Log"), output.errorString());
      return;
    }
    reload();
    if (row < table->rowCount()) table->selectRow(row);
  });
  connect(exportButton, &QPushButton::clicked, &dialog, [this] { exportContestCabrillo(); });
  connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
  dialog.exec();
}

void MainWindow::exportContestCabrillo() {
  const QString station = QSettings().value(QStringLiteral("station/callsign")).toString().trimmed().toUpper();
  if (station.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("Cabrillo"), QStringLiteral("Configure MYCALL first."));
    return;
  }
  QFile source(contestLogPath());
  if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QMessageBox::warning(this, QStringLiteral("Cabrillo"), QStringLiteral("No contest log available."));
    return;
  }
  const QString contest = contestType_->currentText();
  const bool cqww = contest == QStringLiteral("CQ-WW-RTTY");
  const bool darc = contest == QStringLiteral("DARC-RTTY");
  const bool jarl = contest == QStringLiteral("JARL-WW-RTTY");
  QSettings settings;
  QDialog config(this);
  config.setWindowTitle(QStringLiteral("Cabrillo Settings"));
  auto *form = new QFormLayout(&config);
  auto *power = new QComboBox(&config);
  if (darc) {
    power->addItems({QStringLiteral("LOW"), QStringLiteral("QRP")});
  } else {
    power->addItems({QStringLiteral("HIGH"), QStringLiteral("LOW"), QStringLiteral("QRP")});
  }
  const QString savedPower = settings.value(QStringLiteral("cabrillo/power"), QStringLiteral("LOW")).toString();
  power->setCurrentText(darc && savedPower == QStringLiteral("HIGH") ? QStringLiteral("LOW") : savedPower);
  const bool eligibleTrainee = darc && darcOperatingInDl(station) && shortRyTrainee(station);
  auto *trainee = new QCheckBox(QStringLiteral("Trainee (DN1–DN8 or /T)"), &config);
  trainee->setChecked(eligibleTrainee);
  trainee->setEnabled(eligibleTrainee);
  auto *operatorCategory = new QComboBox(&config);
  operatorCategory->addItems({QStringLiteral("SINGLE-OP"), QStringLiteral("CHECKLOG")});
  operatorCategory->setCurrentText(settings.value(QStringLiteral("cabrillo/operator"),
      QStringLiteral("SINGLE-OP")).toString());
  auto *assisted = new QComboBox(&config);
  assisted->addItems({QStringLiteral("NON-ASSISTED"), QStringLiteral("ASSISTED")});
  assisted->setCurrentText(settings.value(QStringLiteral("cabrillo/assisted"),
                                          QStringLiteral("NON-ASSISTED")).toString());
  auto *bandCategory = new QComboBox(&config);
  bandCategory->addItems({QStringLiteral("ALL"), QStringLiteral("80M"), QStringLiteral("40M"),
                          QStringLiteral("20M"), QStringLiteral("15M"), QStringLiteral("10M")});
  bandCategory->setCurrentText(settings.value(QStringLiteral("cabrillo/band"), QStringLiteral("ALL")).toString());
  auto *email = new QLineEdit(settings.value(QStringLiteral("cabrillo/email")).toString(), &config);
  auto *club = new QLineEdit(settings.value(QStringLiteral("cabrillo/club")).toString(), &config);
  auto *soapbox = new QLineEdit(settings.value(QStringLiteral("cabrillo/soapbox")).toString(), &config);
  auto *location = new QLineEdit(settings.value(QStringLiteral("cabrillo/location"), QStringLiteral("DX")).toString(),
                                 &config);
  location->setMaxLength(16);
  form->addRow(QStringLiteral("Operator"), operatorCategory);
  form->addRow(QStringLiteral("Power"), power);
  if (darc) { form->addRow(QStringLiteral("Class"), trainee); }
  if (cqww) {
    form->addRow(QStringLiteral("Location"), location);
    form->addRow(QStringLiteral("Assisted"), assisted);
    form->addRow(QStringLiteral("Band"), bandCategory);
  }
  form->addRow(QStringLiteral("E-mail"), email);
  form->addRow(QStringLiteral("Club"), club);
  form->addRow(QStringLiteral("Soapbox"), soapbox);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &config);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &config, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &config, &QDialog::reject);
  if (config.exec() != QDialog::Accepted) { return; }
  const QString chosenLocation = location->text().trimmed().toUpper();
  static const QRegularExpression validLocation(QStringLiteral("^[A-Z0-9-]{1,16}$"));
  if (cqww && !validLocation.match(chosenLocation).hasMatch()) {
    QMessageBox::warning(this, QStringLiteral("Cabrillo"), QStringLiteral("Invalid LOCATION field."));
    return;
  }
  settings.setValue(QStringLiteral("cabrillo/power"), power->currentText());
  settings.setValue(QStringLiteral("cabrillo/operator"), operatorCategory->currentText());
  settings.setValue(QStringLiteral("cabrillo/location"), chosenLocation);
  settings.setValue(QStringLiteral("cabrillo/assisted"), assisted->currentText());
  settings.setValue(QStringLiteral("cabrillo/band"), bandCategory->currentText());
  settings.setValue(QStringLiteral("cabrillo/email"), email->text().trimmed());
  settings.setValue(QStringLiteral("cabrillo/club"), club->text().trimmed());
  settings.setValue(QStringLiteral("cabrillo/soapbox"), soapbox->text().trimmed());
  QStringList lines;
  lines << QStringLiteral("START-OF-LOG: %1").arg(darc || cqww || jarl ? QStringLiteral("3.0") : QStringLiteral("2.0"))
        << QStringLiteral("CONTEST: %1").arg(jarl ? QStringLiteral("JARL-WW-RTTY") : (cqww ? QStringLiteral("CQ-WW-RTTY") : (darc ? QStringLiteral("ShortRY") :
            QStringLiteral("SARTG-RTTY"))))
        << QStringLiteral("CALLSIGN: %1").arg(station);
  if (cqww) {
    lines << QStringLiteral("LOCATION: %1").arg(chosenLocation)
          << QStringLiteral("CATEGORY-OPERATOR: %1").arg(operatorCategory->currentText())
          << QStringLiteral("CATEGORY-BAND: %1").arg(bandCategory->currentText())
          << QStringLiteral("CATEGORY-POWER: %1").arg(power->currentText())
          << QStringLiteral("CATEGORY-MODE: RTTY")
          << QStringLiteral("CATEGORY-ASSISTED: %1").arg(assisted->currentText())
          << QStringLiteral("CATEGORY-STATION: FIXED");
  } else if (darc || jarl) {
    lines << QStringLiteral("CATEGORY-OPERATOR: SINGLE-OP")
          << QStringLiteral("CATEGORY-BAND: ALL")
          << QStringLiteral("CATEGORY-MODE: RTTY")
          << QStringLiteral("CATEGORY-POWER: %1").arg(power->currentText())
          << QStringLiteral("CATEGORY-ASSISTED: NON-ASSISTED")
          << QStringLiteral("CATEGORY-STATION: FIXED");
    if (darc && trainee->isChecked()) {
      lines << QStringLiteral("CATEGORY-OVERLAY: TRAINEE")
            << QStringLiteral("SOAPBOX: DARC ShortRY trainee class");
    }
  } else {
    // SARTG uses the Cabrillo 2.0 category convention in AppKit.
    lines << QStringLiteral("CATEGORY: %1 ALL %2 RTTY")
          .arg(operatorCategory->currentText(), power->currentText());
  }
  const QString operatorName = settings.value(QStringLiteral("station/name")).toString().trimmed();
  if (!operatorName.isEmpty()) { lines << QStringLiteral("NAME: %1").arg(operatorName); }
  lines << QStringLiteral("OPERATORS: %1").arg(station)
        << QStringLiteral("CREATED-BY: rttyPad");
  if (!email->text().trimmed().isEmpty()) {
    lines << (cqww ? QStringLiteral("EMAIL: %1") : QStringLiteral("SOAPBOX: E-MAIL: %1"))
          .arg(email->text().trimmed());
  }
  if (!jarl && !club->text().trimmed().isEmpty()) { lines << QStringLiteral("CLUB: %1").arg(club->text().trimmed()); }
  for (const QString &entry : soapbox->text().split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
      Qt::SkipEmptyParts)) {
    if (!entry.trimmed().isEmpty()) { lines << QStringLiteral("SOAPBOX: %1").arg(entry.trimmed()); }
  }
  // Review location codes before exporting.  Zones 03-05 may include
  // continental USA/Canada, but the zone alone cannot establish the QTH.
  // Do not silently turn an unrecognized code into a plausible Cabrillo QSO.
  int missingQthR = 0;
  int invalidQthR = 0;
  if (cqww) {
    while (!source.atEnd()) {
      const QList<QByteArray> fields = source.readLine().trimmed().split('\t');
      if ((fields.size() != 10 && fields.size() != 11) ||
          QString::fromUtf8(fields.at(0)) != contest) { continue; }
      const QString qth = fields.size() == 11
                          ? QString::fromUtf8(fields.at(10)).trimmed().toUpper() : QStringLiteral("DX");
      if (!validCQWWQthR(qth)) { ++invalidQthR; }
      else if (qth == QStringLiteral("DX")) {
        bool ok = false;
        const int zone = QString::fromUtf8(fields.at(8)).toInt(&ok);
        if (ok && zone >= 3 && zone <= 5) { ++missingQthR; }
      }
    }
    if (source.error() != QFile::NoError) {
      QMessageBox::critical(this, QStringLiteral("Cabrillo"), source.errorString());
      return;
    }
    if (missingQthR || invalidQthR) {
      const QString warning = QStringLiteral(
                                      "%1 QSO(s) in zones 03-05 still have QTH-R DX; "
                                      "%2 QSO(s) have an invalid QTH-R.\n\n"
                                      "Review the Contest Log before submission. Continue exporting?")
                              .arg(missingQthR).arg(invalidQthR);
      if (QMessageBox::question(this, QStringLiteral("Review CQ WW QTH-R"), warning,
                                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
      }
    }
    if (!source.seek(0)) {
      QMessageBox::critical(this, QStringLiteral("Cabrillo"),
                            QStringLiteral("Cannot rewind contest log for export."));
      return;
    }
  }
  // A single-band entry must not silently contain contacts from other bands.
  // Filter by the stored band (field 3), not by the current VFO frequency.
  const QString requestedBand = cqww ? bandCategory->currentText() : QStringLiteral("ALL");
  int count = 0;
  int excluded = 0;
  int recordNumber = 0;
  static const QRegularExpression cabrilloDate(QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2}$"));
  static const QRegularExpression cabrilloTime(QStringLiteral("^([01][0-9]|2[0-3])[0-5][0-9]$"));
  static const QRegularExpression cabrilloCall(QStringLiteral("^[A-Z0-9/]+$"));
  static const QRegularExpression cabrilloDigits(QStringLiteral("^[0-9]+$"));
  while (!source.atEnd()) {
    ++recordNumber;
    const QList<QByteArray> fields = source.readLine().trimmed().split('\t');
    if (fields.isEmpty() || QString::fromUtf8(fields.at(0)) != contest) { continue; }
    if (fields.size() != 10 && fields.size() != 11) {
      QMessageBox::critical(this, QStringLiteral("Cabrillo"),
                            QStringLiteral("Malformed contest log record on line %1 (%2 fields). Export aborted.")
                            .arg(recordNumber).arg(fields.size()));
      return;
    }
    if (requestedBand != QStringLiteral("ALL") &&
        QString::fromUtf8(fields.at(3)).toUpper() != requestedBand) {
      ++excluded;
      continue;
    }
    const QString rawFrequency = QString::fromUtf8(fields.at(5));
    const QString rawDate = QString::fromUtf8(fields.at(1));
    const QString rawTime = QString::fromUtf8(fields.at(2));
    const QString rawCall = QString::fromUtf8(fields.at(4)).trimmed().toUpper();
    const QString rawSent = QString::fromUtf8(fields.at(9));
    const QString rawReceived = QString::fromUtf8(fields.at(8));
    bool frequencyOk = false;
    const int khz = rawFrequency.toInt(&frequencyOk);
    bool sentOk = false, receivedOk = false;
    const int sentValue = rawSent.toInt(&sentOk);
    const int receivedValue = rawReceived.toInt(&receivedOk);
    if (!frequencyOk || khz <= 0 ||
        !cabrilloDate.match(rawDate).hasMatch() ||
        !QDate::fromString(rawDate, QStringLiteral("yyyy-MM-dd")).isValid() ||
        !cabrilloTime.match(rawTime).hasMatch() ||
        !cabrilloCall.match(rawCall).hasMatch() ||
        (!darc && !cabrilloDigits.match(rawSent).hasMatch()) ||
        (!darc && !cabrilloDigits.match(rawReceived).hasMatch()) ||
        (!darc && (!sentOk || !receivedOk || (jarl ? sentValue < 0 || receivedValue < 0 : sentValue <= 0 || receivedValue <= 0))) ||
        (darc && (rawSent.isEmpty() || rawReceived.isEmpty())) ||
        ((cqww && (sentValue > 40 || receivedValue > 40)) || (jarl && (sentValue > 99 || receivedValue > 99))) ||
        (cqww && !validCQWWQthR(QString::fromUtf8(fields.size() == 11 ? fields.at(10) :
            QByteArray("DX")).trimmed().toUpper()))) {
      QMessageBox::critical(this, QStringLiteral("Cabrillo"),
                            QStringLiteral("Invalid QSO data on contest log line %1. Correct the QSO before exporting.")
                            .arg(recordNumber));
      return;
    }
    if (darc) {
      const QString band = QString::fromUtf8(fields.at(3)).trimmed().toUpper();
      const QString received = rawReceived.trimmed().toUpper();
      const QString sent = rawSent.trimmed().toUpper();
      static const QRegularExpression dok(QStringLiteral("^[A-Z0-9]{1,8}$"));
      static const QRegularExpression serial(QStringLiteral("^[0-9]{3}$"));
      const bool rxValid = isDarcGermanCall(rawCall)
                           ? (dok.match(received).hasMatch() && !serial.match(received).hasMatch())
                           : serial.match(received).hasMatch();
      const bool txValid = darcOperatingInDl(station)
                           ? (dok.match(sent).hasMatch() && !serial.match(sent).hasMatch())
                           : serial.match(sent).hasMatch();
      if ((band != QStringLiteral("80M") && band != QStringLiteral("40M")) || !rxValid || !txValid) {
        QMessageBox::critical(this, QStringLiteral("Cabrillo"),
                              QStringLiteral("Invalid ShortRY band or DL/DX exchange on line %1. Correct the QSO before exporting.")
                              .arg(recordNumber));
        return;
      }
    }
    const QString frequency = QString::fromUtf8(fields.at(5)).rightJustified(5, QLatin1Char(' '));
    const QString date = QString::fromUtf8(fields.at(1));
    const QString time = QString::fromUtf8(fields.at(2)).leftJustified(4, QLatin1Char(' '));
    const QString call = QString::fromUtf8(fields.at(4)).leftJustified(13, QLatin1Char(' '));
    const QString sentRst = QString::fromUtf8(fields.at(6));
    const QString receivedRst = QString::fromUtf8(fields.at(7));
    const QString receivedExchange = QString::fromUtf8(fields.at(8));
    const QString sentExchange = QString::fromUtf8(fields.at(9));
    const QString receivedLocation = fields.size() == 11 ? QString::fromUtf8(fields.at(10)) : QStringLiteral("DX");
    if (cqww) {
      // CQ WW RTTY: sent RST, zone, DX; received RST, zone, location.
      // LOCATION: DX in the header is not transmitted as an exchange.
      lines << QStringLiteral("QSO: %1 RY %2 %3 %4 %5 %6 DX %7 %8 %9 %10")
            .arg(frequency, date, time, station.leftJustified(13, QLatin1Char(' ')),
                 sentRst, sentExchange.rightJustified(2, QLatin1Char('0')),
                 call, receivedRst, receivedExchange.rightJustified(2, QLatin1Char('0')), receivedLocation);
    } else {
      lines << QStringLiteral("QSO: %1 RY %2 %3 %4 %5 %6 %7 %8 %9")
            .arg(frequency, date, time, station.leftJustified(13, QLatin1Char(' ')),
                 sentRst, (darc ? sentExchange.leftJustified(6, QLatin1Char(' '))
                           : sentExchange.rightJustified(6, QLatin1Char(' '))),
                 call, receivedRst, (darc ? receivedExchange.leftJustified(6, QLatin1Char(' '))
                                     : receivedExchange.rightJustified(6, QLatin1Char(' '))));
    }
    ++count;
  }
  if (source.error() != QFile::NoError) {
    QMessageBox::critical(this, QStringLiteral("Cabrillo"), source.errorString());
    return;
  }
  if (!count) {
    QMessageBox::warning(this, QStringLiteral("Cabrillo"),
                         excluded ? QStringLiteral("No QSOs on the selected category band.")
                         : QStringLiteral("No QSOs for the selected contest."));
    return;
  }
  lines << QStringLiteral("END-OF-LOG:");
  const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Cabrillo"),
    station + QStringLiteral(".cbr"), QStringLiteral("Cabrillo (*.cbr)"));
  if (path.isEmpty()) { return; }
  QSaveFile output(path);
  const QByteArray bytes = lines.join(QStringLiteral("\r\n")).toUtf8() + "\r\n";
  if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
    QMessageBox::critical(this, QStringLiteral("Cabrillo"), output.errorString());
  }
}

// ---- PTT / Serial settings dialog ----------------------------------------
void MainWindow::openPttSettingsDialog()
{
  QSettings settings;
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Serial / PTT Settings"));
  auto *form = new QFormLayout(&dialog);

  auto *hostEdit = new QLineEdit(
    settings.value(QStringLiteral("tx/rigctlHost"), QStringLiteral("127.0.0.1")).toString(), &dialog);
  auto *portSpin = new QSpinBox(&dialog);
  portSpin->setRange(1, 65535);
  portSpin->setValue(settings.value(QStringLiteral("tx/rigctlPort"), 4532).toInt());

  form->addRow(QStringLiteral("rigctld host"), hostEdit);
  form->addRow(QStringLiteral("rigctld port"), portSpin);

  auto *note = new QLabel(
    QStringLiteral(
      "rigctld is part of Hamlib. Start externally with:\n"
      "  rigctld -m <model> -r <comport> -t 4532\n\n"
      "All Hamlib-supported rigs work via rigctld.\n"
      "Download Hamlib: https://hamlib.github.io/"),
    &dialog);
  note->setWordWrap(true);
  form->addRow(note);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);

  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    settings.setValue(QStringLiteral("tx/rigctlHost"), hostEdit->text());
    settings.setValue(QStringLiteral("tx/rigctlPort"), portSpin->value());
    tx_.setRigctlHost(hostEdit->text());
    tx_.setRigctlPort(portSpin->value());
    dialog.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog.exec();
}

// ---- Settings TCI dialog --------------------------------------------------
void MainWindow::openTciSettingsDialog()
{
  QSettings settings;
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Settings TCI"));
  auto *form = new QFormLayout(&dialog);

  // Auto setup toggle
  auto *autoSetup = new QCheckBox(QStringLiteral("Auto setup TCI on connect"), &dialog);
  autoSetup->setChecked(settings.value(QStringLiteral("tci/autoSetup"), true).toBool());
  autoSetup->setToolTip(QStringLiteral(
    "Set SDR to DIGL mode and correct pitch on connect. Restore original settings on disconnect."));
  form->addRow(autoSetup);

  // Auto setup on band change
  auto *bandChange = new QCheckBox(QStringLiteral("Auto setup on band change"), &dialog);
  bandChange->setChecked(settings.value(QStringLiteral("tci/autoSetupBandChange"), true).toBool());
  bandChange->setEnabled(autoSetup->isChecked());
  connect(autoSetup, &QCheckBox::toggled, bandChange, &QWidget::setEnabled);
  form->addRow(bandChange);

  form->addRow(new QLabel(QStringLiteral(""), &dialog)); // spacer

  // Dialect — controls which init commands are sent and whether IQ stream is used.
  // Stored as int (TCIClient::Dialect cast).
  auto *dialectCombo = new QComboBox(&dialog);

  // Entries: human label, data = int(TCIClient::Dialect)
  struct DialectEntry { TCIClient::Dialect value; const char *label; };
  static const DialectEntry kDialects[] = {
    { TCIClient::Dialect::Auto,      "Auto-detect (device: header)" },
    { TCIClient::Dialect::Generic,   "Generic TCI  —  ExpertSDR2/3, Zeus, NereusSDR, Kainos" },
    { TCIClient::Dialect::DeskHPSDR, "deskHPSDR / Thetis  —  rtty_enable:1 op verbinding" },
    { TCIClient::Dialect::BoltSDR,   "BoltSDR / Rigflow  —  geen IQ stream, poort 40001" },
    { TCIClient::Dialect::SDROxide,  "SDROxide  —  geen IQ stream, TCI client+server" },
  };
  for (const auto &e : kDialects) {
    dialectCombo->addItem(QLatin1String(e.label), static_cast<int>(e.value));
  }
  dialectCombo->setToolTip(QStringLiteral(
    "Kies het dialect dat overeenkomt met jouw SDR software.\n"
    "Auto-detect leest de 'device:' melding bij verbinding en kiest automatisch.\n"
    "Generic TCI werkt voor de meeste apps; kies alleen een specifiek dialect\n"
    "als de standaard niet werkt."));

  const int savedDialect = settings.value(QStringLiteral("tci/dialect"),
                                          static_cast<int>(TCIClient::Dialect::Auto)).toInt();
  for (int i = 0; i < dialectCombo->count(); ++i) {
    if (dialectCombo->itemData(i).toInt() == savedDialect) {
      dialectCombo->setCurrentIndex(i);
      break;
    }
  }
  form->addRow(QStringLiteral("TCI dialect"), dialectCombo);

  // Active dialect info label (shows what is currently resolved when Auto)
  auto *activeDialectLabel = new QLabel(&dialog);
  activeDialectLabel->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
  {
    const TCIClient::Dialect active = tci_.activeDialect();
    const bool isAuto = (tci_.dialect() == TCIClient::Dialect::Auto);
    if (isAuto) {
      activeDialectLabel->setText(QStringLiteral("Huidig: %1 (auto-detect)")
        .arg(TCIClient::dialectName(active)));
    } else {
      activeDialectLabel->setText(QStringLiteral("Huidig: %1")
        .arg(TCIClient::dialectName(active)));
    }
  }
  form->addRow(QStringLiteral(""), activeDialectLabel);

  // Sample rate
  auto *srCombo = new QComboBox(&dialog);
  srCombo->addItem(QStringLiteral("48 kHz"),  48000);
  srCombo->addItem(QStringLiteral("96 kHz"),  96000);
  srCombo->addItem(QStringLiteral("192 kHz"), 192000);
  const int savedSr = settings.value(QStringLiteral("tci/sampleRate"), 48000).toInt();
  for (int i = 0; i < srCombo->count(); ++i) {
    if (srCombo->itemData(i).toInt() == savedSr) {
      srCombo->setCurrentIndex(i);
      break;
    }
  }
  form->addRow(QStringLiteral("I/Q sample rate"), srCombo);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);

  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    settings.setValue(QStringLiteral("tci/autoSetup"),          autoSetup->isChecked());
    settings.setValue(QStringLiteral("tci/autoSetupBandChange"), bandChange->isChecked());
    const int dialectInt = dialectCombo->currentData().toInt();
    settings.setValue(QStringLiteral("tci/dialect"),   dialectInt);
    settings.setValue(QStringLiteral("tci/sampleRate"), srCombo->currentData().toInt());
    applyTciDialect(static_cast<TCIClient::Dialect>(dialectInt));
    dialog.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog.exec();
}

// ---- Apply TCI dialect — sets dialect on TCIClient and PTT suffix ----------
void MainWindow::applyTciDialect(TCIClient::Dialect dialect)
{
  tci_.setDialect(dialect);

  // PTT suffix: SDROxide requires ",tci" on trx:0,true/false to route audio
  // to the TX path. All other dialects use plain trx:0,true/false.
  const QString suffix = (dialect == TCIClient::Dialect::SDROxide)
                         ? QStringLiteral(",tci") : QString();
  tx_.setTciPttSuffix(suffix);

  qDebug() << "TCI dialect set:" << TCIClient::dialectName(dialect)
           << "PTT suffix:" << (suffix.isEmpty() ? "(none)" : suffix);
}

// ---- Verbindingsmodus: TCI of Audio (geluidskaart) -----------------------
// tciMode = true  → TCI WebSocket: IQ stream RX, binary TX frames, TCI PTT
// tciMode = false → geluidskaart in/uit, serieel/rigctld PTT, geen WebSocket
void MainWindow::applyConnectionMode(bool tciMode)
{
  tciConnMode_ = tciMode;

  // IQ-pan widget alleen zichtbaar in TCI-modus
  if (iqPan_) iqPan_->setVisible(tciMode);

  if (tciMode) {
    // Naar TCI: stop geluidskaart RX, TCI TX frames aan
    stopStandaloneRx();
    if (tx_.standaloneEngine())
      tx_.standaloneEngine()->setTciTxAudioEnabled(true);
  } else {
    // Naar Audio: verbreek TCI, start geluidskaart RX, TCI TX frames uit
    if (tci_.isConnected()) tci_.disconnectFromHost();
    if (tx_.standaloneEngine())
      tx_.standaloneEngine()->setTciTxAudioEnabled(false);
    startStandaloneRx();
  }
}

// ---- Wire StandaloneRTTYTx::txAudioBlock → TCIClient::sendTxAudioFrame ---
// Called once during construction, after both tx_ and tci_ exist.
void MainWindow::connectTciTxAudio()
{
  auto *eng = tx_.standaloneEngine();
  if (!eng) return;
  // QueuedConnection: txAudioBlock is emitted from the miniaudio audio thread;
  // sendTxAudioFrame must run on the main thread (QWebSocket is not thread-safe).
  connect(eng, &StandaloneRTTYTx::txAudioBlock,
          this, [this](const QByteArray &block, quint32 sr) {
    const auto *samples = reinterpret_cast<const float *>(block.constData());
    const quint32 count = quint32(block.size()) / sizeof(float);
    tci_.sendTxAudioFrame(samples, count, sr);
  }, Qt::QueuedConnection);
}
