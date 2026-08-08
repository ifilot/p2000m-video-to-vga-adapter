/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

#include <windows.h>
#include <devguid.h>
#include <setupapi.h>

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr quint16 kPicoVendorId = 0x2e8a;
constexpr int kFrameHeaderSize = 48;
constexpr int kFramePayloadSize = 640 * 288 / 8;
constexpr int kSourceWidth = 640;
constexpr int kSourceHeight = 288;
constexpr int kVgaHeight = 480;

constexpr std::array<quint32, 256> makeCrc32Table() {
    std::array<quint32, 256> table = {};
    for (quint32 value = 0; value < table.size(); ++value) {
        quint32 remainder = value;
        for (unsigned bit = 0; bit < 8; ++bit) {
            remainder = (remainder >> 1u) ^
                        ((remainder & 1u) != 0u ? 0xedb88320u : 0u);
        }
        table[value] = remainder;
    }
    return table;
}

constexpr auto kCrc32Table = makeCrc32Table();

quint16 loadU16(const char *data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return static_cast<quint16>(bytes[0]) |
           (static_cast<quint16>(bytes[1]) << 8u);
}

quint32 loadU32(const char *data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return static_cast<quint32>(bytes[0]) |
           (static_cast<quint32>(bytes[1]) << 8u) |
           (static_cast<quint32>(bytes[2]) << 16u) |
           (static_cast<quint32>(bytes[3]) << 24u);
}

quint32 crc32(const QByteArray &data) {
    quint32 crc = 0xffffffffu;
    for (const unsigned char byte : data) {
        crc = (crc >> 8u) ^ kCrc32Table[(crc ^ byte) & 0xffu];
    }
    return ~crc;
}

struct DeviceSettings {
    quint32 foreground = 0xffffffu;
    quint32 background = 0x000000u;
    quint32 border = 0xff00ffu;
    bool borderEnabled = false;
    bool borderDotted = false;
    bool stretch = false;
    int phaseTrim = 0;
    QString storage = QStringLiteral("default");
};

QString colorText(quint32 rgb) {
    return QStringLiteral("#%1").arg(rgb & 0x00ffffffu, 6, 16,
                                     QLatin1Char('0')).toUpper();
}

bool parseDeviceSettings(const QByteArray &consoleText,
                         DeviceSettings *settings) {
    static const QRegularExpression expression(QStringLiteral(
        R"(DISPLAY foreground=#([0-9a-fA-F]{6}) background=#([0-9a-fA-F]{6}) border=(on|off) border_color=#([0-9a-fA-F]{6}) border_style=(solid|dotted) scale=(fit-5:3|native-1:1) phase_trim=(-?[0-9]+) storage=([a-zA-Z]+))"));
    const QRegularExpressionMatch match = expression.match(
        QString::fromLatin1(consoleText));
    if (!match.hasMatch()) {
        return false;
    }

    bool foregroundOk = false;
    bool backgroundOk = false;
    bool borderOk = false;
    bool phaseOk = false;
    const quint32 foreground = match.captured(1).toUInt(&foregroundOk, 16);
    const quint32 background = match.captured(2).toUInt(&backgroundOk, 16);
    const quint32 border = match.captured(4).toUInt(&borderOk, 16);
    const int phase = match.captured(7).toInt(&phaseOk);
    if (!foregroundOk || !backgroundOk || !borderOk || !phaseOk ||
        phase < -4 || phase > 4) {
        return false;
    }

    settings->foreground = foreground;
    settings->background = background;
    settings->borderEnabled = match.captured(3) == QStringLiteral("on");
    settings->border = border;
    settings->borderDotted = match.captured(5) == QStringLiteral("dotted");
    settings->stretch = match.captured(6) == QStringLiteral("fit-5:3");
    settings->phaseTrim = phase;
    settings->storage = match.captured(8);
    return true;
}

QStringList picoSerialPorts() {
    QStringList ports;
    HDEVINFO deviceInfo = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (deviceInfo == INVALID_HANDLE_VALUE) {
        return ports;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device = {};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(deviceInfo, index, &device)) {
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceRegistryPropertyW(
            deviceInfo, &device, SPDRP_HARDWAREID, nullptr, nullptr, 0,
            &required);
        if (required == 0) {
            continue;
        }

        std::vector<BYTE> storage(required + sizeof(wchar_t), 0);
        if (!SetupDiGetDeviceRegistryPropertyW(
                deviceInfo, &device, SPDRP_HARDWAREID, nullptr,
                storage.data(), required, nullptr)) {
            continue;
        }

        bool isPico = false;
        const auto *hardwareId =
            reinterpret_cast<const wchar_t *>(storage.data());
        while (*hardwareId != L'\0') {
            const QString id = QString::fromWCharArray(hardwareId).toUpper();
            if (id.contains(QStringLiteral("VID_%1").arg(
                    kPicoVendorId, 4, 16, QLatin1Char('0')).toUpper())) {
                isPico = true;
                break;
            }
            hardwareId += std::wcslen(hardwareId) + 1;
        }
        if (!isPico) {
            continue;
        }

        HKEY deviceKey = SetupDiOpenDevRegKey(
            deviceInfo, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (deviceKey == INVALID_HANDLE_VALUE) {
            continue;
        }

        std::array<wchar_t, 64> portName = {};
        DWORD type = 0;
        DWORD bytes = static_cast<DWORD>(portName.size() * sizeof(wchar_t));
        const LSTATUS result = RegQueryValueExW(
            deviceKey, L"PortName", nullptr, &type,
            reinterpret_cast<LPBYTE>(portName.data()), &bytes);
        RegCloseKey(deviceKey);
        if (result == ERROR_SUCCESS && type == REG_SZ) {
            const QString name = QString::fromWCharArray(portName.data());
            if (name.startsWith(QStringLiteral("COM"),
                                Qt::CaseInsensitive)) {
                ports.append(name);
            }
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);
    ports.removeDuplicates();
    ports.sort(Qt::CaseInsensitive);
    return ports;
}

class WindowsSerialPort {
public:
    ~WindowsSerialPort() { close(); }

    bool open(const QString &portName) {
        close();
        const QString path = QStringLiteral("\\\\.\\") + portName;
        handle_ = CreateFileW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            return false;
        }

        SetupComm(handle_, 1024 * 1024, 4096);
        DCB state = {};
        state.DCBlength = sizeof(state);
        if (!GetCommState(handle_, &state)) {
            close();
            return false;
        }
        state.BaudRate = CBR_115200;
        state.ByteSize = 8;
        state.Parity = NOPARITY;
        state.StopBits = ONESTOPBIT;
        state.fBinary = TRUE;
        state.fParity = FALSE;
        state.fOutxCtsFlow = FALSE;
        state.fOutxDsrFlow = FALSE;
        state.fDtrControl = DTR_CONTROL_ENABLE;
        state.fOutX = FALSE;
        state.fInX = FALSE;
        state.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(handle_, &state)) {
            close();
            return false;
        }

        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.WriteTotalTimeoutConstant = 500;
        if (!SetCommTimeouts(handle_, &timeouts)) {
            close();
            return false;
        }
        PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        EscapeCommFunction(handle_, SETDTR);
        name_ = portName;
        return true;
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            EscapeCommFunction(handle_, CLRDTR);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        name_.clear();
    }

    bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    QString name() const { return name_; }

    bool write(const QByteArray &data) {
        if (!isOpen()) {
            return false;
        }
        qsizetype offset = 0;
        while (offset < data.size()) {
            DWORD written = 0;
            if (!WriteFile(handle_, data.constData() + offset,
                           static_cast<DWORD>(data.size() - offset),
                           &written, nullptr)) {
                return false;
            }
            if (written == 0) {
                return false;
            }
            offset += static_cast<qsizetype>(written);
        }
        return true;
    }

    QByteArray readAvailable(bool *ok) {
        QByteArray result;
        *ok = isOpen();
        if (!*ok) {
            return result;
        }

        std::array<char, 16384> buffer = {};
        for (unsigned pass = 0; pass < 16; ++pass) {
            DWORD errors = 0;
            COMSTAT status = {};
            if (!ClearCommError(handle_, &errors, &status)) {
                *ok = false;
                break;
            }
            if (status.cbInQue == 0) {
                break;
            }
            const DWORD requested = std::min<DWORD>(
                status.cbInQue, static_cast<DWORD>(buffer.size()));
            DWORD received = 0;
            if (!ReadFile(handle_, buffer.data(), requested, &received,
                          nullptr)) {
                *ok = false;
                break;
            }
            if (received == 0) {
                break;
            }
            result.append(buffer.data(), static_cast<qsizetype>(received));
        }
        return result;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    QString name_;
};

class ColorButton final : public QPushButton {
public:
    explicit ColorButton(quint32 rgb, QWidget *parent = nullptr)
        : QPushButton(parent) {
        setMinimumWidth(140);
        setColor(rgb);
        connect(this, &QPushButton::clicked, this, [this] {
            const QColor selected = QColorDialog::getColor(
                QColor::fromRgb(rgb_), this, QStringLiteral("Select color"));
            if (selected.isValid()) {
                setColor(selected.rgb());
            }
        });
    }

    quint32 color() const { return rgb_; }

    void setColor(quint32 rgb) {
        rgb_ = rgb & 0x00ffffffu;
        const int red = static_cast<int>((rgb_ >> 16u) & 0xffu);
        const int green = static_cast<int>((rgb_ >> 8u) & 0xffu);
        const int blue = static_cast<int>(rgb_ & 0xffu);
        const bool useDarkText = red * 299 + green * 587 + blue * 114 >
                                 150000;
        setText(colorText(rgb_));
        setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; color: %2; "
            "border: 1px solid palette(mid); padding: 5px 12px; }")
                          .arg(colorText(rgb_),
                               useDarkText ? QStringLiteral("black")
                                           : QStringLiteral("white")));
    }

private:
    quint32 rgb_ = 0;
};

class ConfigurationDialog final : public QDialog {
public:
    enum Result {
        Apply = QDialog::Accepted,
        ApplyAndSave = 2,
    };

    ConfigurationDialog(const DeviceSettings &settings,
                        QWidget *parent = nullptr)
        : QDialog(parent) {
        setWindowTitle(QStringLiteral("Configure P2000M Adapter"));
        setMinimumWidth(430);

        auto *layout = new QVBoxLayout(this);
        auto *explanation = new QLabel(
            QStringLiteral("Changes are applied to both the VGA output and "
                           "this viewer. Save to make them persistent."),
            this);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);

        auto *form = new QFormLayout;
        foreground_ = new ColorButton(settings.foreground, this);
        background_ = new ColorButton(settings.background, this);
        borderColor_ = new ColorButton(settings.border, this);
        borderEnabled_ = new QCheckBox(QStringLiteral("Show border"), this);
        borderEnabled_->setChecked(settings.borderEnabled);
        borderStyle_ = new QComboBox(this);
        borderStyle_->addItem(QStringLiteral("Solid"), false);
        borderStyle_->addItem(QStringLiteral("Dotted"), true);
        borderStyle_->setCurrentIndex(settings.borderDotted ? 1 : 0);
        scaling_ = new QComboBox(this);
        scaling_->addItem(QStringLiteral("Native 1:1 (centered)"), false);
        scaling_->addItem(QStringLiteral("Fit 5:3 (full height)"), true);
        scaling_->setCurrentIndex(settings.stretch ? 1 : 0);
        phaseTrim_ = new QSpinBox(this);
        phaseTrim_->setRange(-4, 4);
        phaseTrim_->setValue(settings.phaseTrim);
        phaseTrim_->setSuffix(QStringLiteral(" tick(s)"));
        auto *storage = new QLabel(settings.storage, this);

        form->addRow(QStringLiteral("Foreground:"), foreground_);
        form->addRow(QStringLiteral("Background:"), background_);
        form->addRow(QStringLiteral("Border:"), borderEnabled_);
        form->addRow(QStringLiteral("Border color:"), borderColor_);
        form->addRow(QStringLiteral("Border style:"), borderStyle_);
        form->addRow(QStringLiteral("Vertical scaling:"), scaling_);
        form->addRow(QStringLiteral("Sampling phase:"), phaseTrim_);
        form->addRow(QStringLiteral("Current storage state:"), storage);
        layout->addLayout(form);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto *defaults = buttons->addButton(
            QStringLiteral("Defaults"), QDialogButtonBox::ResetRole);
        auto *apply = buttons->addButton(
            QStringLiteral("Apply"), QDialogButtonBox::AcceptRole);
        auto *save = buttons->addButton(
            QStringLiteral("Apply && Save"), QDialogButtonBox::AcceptRole);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::rejected,
                this, &QDialog::reject);
        connect(defaults, &QPushButton::clicked, this, [this] {
            loadSettings(DeviceSettings{});
        });
        connect(apply, &QPushButton::clicked, this, [this] {
            done(Apply);
        });
        connect(save, &QPushButton::clicked, this, [this] {
            done(ApplyAndSave);
        });
    }

    DeviceSettings settings() const {
        DeviceSettings result;
        result.foreground = foreground_->color();
        result.background = background_->color();
        result.border = borderColor_->color();
        result.borderEnabled = borderEnabled_->isChecked();
        result.borderDotted = borderStyle_->currentData().toBool();
        result.stretch = scaling_->currentData().toBool();
        result.phaseTrim = phaseTrim_->value();
        return result;
    }

private:
    void loadSettings(const DeviceSettings &settings) {
        foreground_->setColor(settings.foreground);
        background_->setColor(settings.background);
        borderColor_->setColor(settings.border);
        borderEnabled_->setChecked(settings.borderEnabled);
        borderStyle_->setCurrentIndex(settings.borderDotted ? 1 : 0);
        scaling_->setCurrentIndex(settings.stretch ? 1 : 0);
        phaseTrim_->setValue(settings.phaseTrim);
    }

    ColorButton *foreground_ = nullptr;
    ColorButton *background_ = nullptr;
    ColorButton *borderColor_ = nullptr;
    QCheckBox *borderEnabled_ = nullptr;
    QComboBox *borderStyle_ = nullptr;
    QComboBox *scaling_ = nullptr;
    QSpinBox *phaseTrim_ = nullptr;
};

class ScreenWidget final : public QWidget {
public:
    explicit ScreenWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(640, 480);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setFrame(QImage frame) {
        frame_ = std::move(frame);
        update();
    }

    void setSmoothScaling(bool enabled) {
        smoothScaling_ = enabled;
        update();
    }

    void setIntegerScaling(bool enabled) {
        integerScaling_ = enabled;
        update();
    }

    bool hasFrame() const { return !frame_.isNull(); }

    bool saveFrame(const QString &filename) const {
        return !frame_.isNull() && frame_.save(filename);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (frame_.isNull()) {
            painter.setPen(QColor(180, 180, 180));
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("Waiting for P2000M video"));
            return;
        }

        QSize targetSize;
        if (integerScaling_) {
            const int factor = std::min(width() / frame_.width(),
                                        height() / frame_.height());
            targetSize = factor >= 1
                             ? frame_.size() * factor
                             : frame_.size().scaled(size(),
                                                    Qt::KeepAspectRatio);
        } else {
            targetSize = frame_.size().scaled(size(), Qt::KeepAspectRatio);
        }
        const QRect target(
            QPoint((width() - targetSize.width()) / 2,
                   (height() - targetSize.height()) / 2),
            targetSize);
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              smoothScaling_ && !integerScaling_);
        painter.drawImage(target, frame_);
    }

private:
    QImage frame_;
    bool smoothScaling_ = true;
    bool integerScaling_ = false;
};

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(QStringLiteral("P2000M VID2VGA Viewer"));
        resize(880, 720);
        createMenus();

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        screen_ = new ScreenWidget(central);
        layout->addWidget(screen_, 1);

        controlsWidget_ = new QWidget(central);
        auto *controls = new QHBoxLayout(controlsWidget_);
        controls->setContentsMargins(0, 0, 0, 0);
        connectionLabel_ = new QLabel(QStringLiteral("Not connected"),
                                      controlsWidget_);
        connectButton_ = new QPushButton(QStringLiteral("Connect"),
                                         controlsWidget_);
        disconnectButton_ =
            new QPushButton(QStringLiteral("Disconnect"), controlsWidget_);
        disconnectButton_->setEnabled(false);
        controls->addWidget(connectionLabel_, 1);
        controls->addWidget(connectButton_);
        controls->addWidget(disconnectButton_);
        layout->addWidget(controlsWidget_);
        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("Searching for the adapter…"));

        connect(connectButton_, &QPushButton::clicked, this, [this] {
            autoReconnect_ = true;
            beginDiscovery();
        });
        connect(disconnectButton_, &QPushButton::clicked, this, [this] {
            disconnectFromAdapter(true);
        });
        connect(&pollTimer_, &QTimer::timeout, this,
                [this] { serviceConnection(); });
        pollTimer_.setTimerType(Qt::PreciseTimer);
        pollTimer_.start(1);
        QTimer::singleShot(0, this, [this] { beginDiscovery(); });
    }

protected:
    void closeEvent(QCloseEvent *event) override {
        if (serial_.isOpen()) {
            serial_.write(QByteArrayLiteral("console\r\n"));
            serial_.close();
        }
        event->accept();
    }

private:
    enum class State {
        Disconnected,
        Probing,
        AwaitingScreenMode,
        Streaming,
        AwaitingConsole,
        QueryingSettings,
        Configuring,
        Disconnecting,
    };

    void createMenus() {
        auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
        connectAction_ = fileMenu->addAction(QStringLiteral("&Connect"));
        connectAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
        disconnectAction_ = fileMenu->addAction(QStringLiteral("&Disconnect"));
        disconnectAction_->setEnabled(false);
        fileMenu->addSeparator();
        screenshotAction_ = fileMenu->addAction(
            QStringLiteral("Save &Screenshot…"));
        screenshotAction_->setShortcut(QKeySequence::Save);
        screenshotAction_->setEnabled(false);
        fileMenu->addSeparator();
        auto *exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
        exitAction->setShortcut(QKeySequence::Quit);

        auto *deviceMenu = menuBar()->addMenu(QStringLiteral("&Adapter"));
        configureAction_ = deviceMenu->addAction(
            QStringLiteral("&Configure Adapter…"));
        configureAction_->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+,")));
        configureAction_->setEnabled(false);

        auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
        auto *filterMenu = viewMenu->addMenu(
            QStringLiteral("Scaling &Filter"));
        auto *filterGroup = new QActionGroup(this);
        auto *smoothAction = filterMenu->addAction(
            QStringLiteral("&Smooth (anti-aliased)"));
        auto *sharpAction = filterMenu->addAction(
            QStringLiteral("&Sharp pixels (nearest-neighbor)"));
        smoothAction->setCheckable(true);
        sharpAction->setCheckable(true);
        smoothAction->setChecked(true);
        filterGroup->addAction(smoothAction);
        filterGroup->addAction(sharpAction);
        filterGroup->setExclusive(true);
        auto *integerAction = viewMenu->addAction(
            QStringLiteral("Pixel-perfect &Integer Scaling"));
        integerAction->setCheckable(true);
        viewMenu->addSeparator();
        fullScreenAction_ = viewMenu->addAction(QStringLiteral("&Full Screen"));
        fullScreenAction_->setCheckable(true);
        fullScreenAction_->setShortcut(QKeySequence(Qt::Key_F11));
        auto *leaveFullScreenAction = new QAction(this);
        leaveFullScreenAction->setShortcut(QKeySequence(Qt::Key_Escape));
        addAction(leaveFullScreenAction);

        auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
        auto *aboutAction = helpMenu->addAction(
            QStringLiteral("&About P2000M VID2VGA Viewer"));

        connect(connectAction_, &QAction::triggered, this, [this] {
            autoReconnect_ = true;
            beginDiscovery();
        });
        connect(disconnectAction_, &QAction::triggered, this, [this] {
            disconnectFromAdapter(true);
        });
        connect(screenshotAction_, &QAction::triggered, this, [this] {
            saveScreenshot();
        });
        connect(configureAction_, &QAction::triggered, this, [this] {
            beginConfiguration();
        });
        connect(smoothAction, &QAction::triggered, this, [this] {
            screen_->setSmoothScaling(true);
        });
        connect(sharpAction, &QAction::triggered, this, [this] {
            screen_->setSmoothScaling(false);
        });
        connect(integerAction, &QAction::toggled, this, [this](bool enabled) {
            screen_->setIntegerScaling(enabled);
        });
        connect(fullScreenAction_, &QAction::toggled,
                this, [this](bool enabled) { setPresentationMode(enabled); });
        connect(leaveFullScreenAction, &QAction::triggered, this, [this] {
            if (fullScreenAction_->isChecked()) {
                fullScreenAction_->setChecked(false);
            }
        });
        connect(exitAction, &QAction::triggered, this, &QWidget::close);
        connect(aboutAction, &QAction::triggered, this, [this] {
            QMessageBox::about(
                this, QStringLiteral("About P2000M VID2VGA Viewer"),
                QStringLiteral(
                    "<h3>P2000M VID2VGA Viewer %1</h3>"
                    "<p>A live Qt 6 monitor and configuration utility for "
                    "the Raspberry Pi Pico 2 P2000M video adapter.</p>"
                    "<p>Copyright © 2026 Ivo Filot<br>"
                    "Licensed under GNU GPL v3 or later.</p>")
                    .arg(QStringLiteral(P2000M_VIEWER_VERSION)));
        });
    }

    void saveScreenshot() {
        if (!screen_->hasFrame()) {
            return;
        }
        const QString filename = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save framebuffer"),
            QStringLiteral("p2000m-screen.png"),
            QStringLiteral("PNG image (*.png);;Bitmap image (*.bmp)"));
        if (!filename.isEmpty() && !screen_->saveFrame(filename)) {
            QMessageBox::warning(
                this, QStringLiteral("Save screenshot"),
                QStringLiteral("The screenshot could not be written."));
        }
    }

    void setPresentationMode(bool enabled) {
        menuBar()->setVisible(!enabled);
        statusBar()->setVisible(!enabled);
        controlsWidget_->setVisible(!enabled);
        if (enabled) {
            showFullScreen();
        } else {
            showNormal();
        }
    }

    void beginDiscovery() {
        if (state_ != State::Disconnected) {
            return;
        }
        connectButton_->setEnabled(false);
        connectAction_->setEnabled(false);
        candidates_ = picoSerialPorts();
        candidateIndex_ = 0;
        unavailableCandidates_.clear();
        probedCandidateCount_ = 0;
        if (candidates_.isEmpty()) {
            connectButton_->setEnabled(true);
            connectAction_->setEnabled(true);
            statusBar()->showMessage(
                QStringLiteral("No Raspberry Pi Pico CDC port found"));
            scheduleRediscovery();
            return;
        }
        tryNextCandidate();
    }

    void tryNextCandidate() {
        serial_.close();
        receiveBuffer_.clear();
        while (candidateIndex_ < candidates_.size()) {
            const QString candidate = candidates_.at(candidateIndex_++);
            if (!serial_.open(candidate)) {
                unavailableCandidates_.append(candidate);
                continue;
            }
            ++probedCandidateCount_;
            state_ = State::Probing;
            resumingScreenMode_ = false;
            stateClock_.restart();
            connectionLabel_->setText(
                QStringLiteral("Checking %1…").arg(candidate));
            statusBar()->showMessage(
                QStringLiteral("Verifying P2000M firmware on %1").arg(candidate));
            // Recover deterministically even when a previous viewer exited
            // before the firmware observed DTR dropping and it remained in
            // binary screen mode. CONSOLE is harmless (an unknown command) in
            // an already active console, after which VERSION identifies us.
            serial_.write(
                QByteArrayLiteral("\r\nconsole\r\nversion\r\n"));
            return;
        }

        state_ = State::Disconnected;
        connectButton_->setEnabled(true);
        connectAction_->setEnabled(true);
        if (probedCandidateCount_ == 0 &&
            !unavailableCandidates_.isEmpty()) {
            connectionLabel_->setText(QStringLiteral("Pico port unavailable"));
            statusBar()->showMessage(
                QStringLiteral("%1 could not be opened; close any serial "
                               "terminal using the port")
                    .arg(unavailableCandidates_.join(QStringLiteral(", "))));
        } else {
            connectionLabel_->setText(QStringLiteral("Adapter not found"));
            statusBar()->showMessage(QStringLiteral(
                "Pico ports were opened, but none identified as VID2VGA firmware"));
        }
        scheduleRediscovery();
    }

    void scheduleRediscovery() {
        if (autoReconnect_ && !rediscoveryScheduled_) {
            rediscoveryScheduled_ = true;
            QTimer::singleShot(1500, this, [this] {
                rediscoveryScheduled_ = false;
                if (autoReconnect_ && state_ == State::Disconnected) {
                    beginDiscovery();
                }
            });
        }
    }

    void beginConfiguration() {
        if (state_ != State::Streaming || !serial_.isOpen()) {
            return;
        }

        state_ = State::AwaitingConsole;
        configureAction_->setEnabled(false);
        receiveBuffer_.clear();
        stateClock_.restart();
        statusBar()->showMessage(
            QStringLiteral("Switching the adapter to configuration mode…"));
        if (!serial_.write(QByteArrayLiteral("console\r\n"))) {
            handleConnectionLoss();
        }
    }

    void configurationFailed(const QString &message) {
        state_ = State::Configuring;
        receiveBuffer_.clear();
        QMessageBox::warning(this, QStringLiteral("Adapter configuration"),
                             message);
        resumeScreenMode();
    }

    void showConfigurationDialog(const DeviceSettings &current) {
        state_ = State::Configuring;
        ConfigurationDialog dialog(current, this);
        const int result = dialog.exec();
        if (!serial_.isOpen()) {
            return;
        }

        if (result == ConfigurationDialog::Apply ||
            result == ConfigurationDialog::ApplyAndSave) {
            applyDeviceSettings(
                current, dialog.settings(),
                result == ConfigurationDialog::ApplyAndSave);
        } else {
            resumeScreenMode();
        }
    }

    void applyDeviceSettings(const DeviceSettings &current,
                             const DeviceSettings &requested,
                             bool save) {
        QByteArray commands;
        commands += "fg " + colorText(requested.foreground).mid(1).toLatin1() +
                    "\r\n";
        commands += "bg " + colorText(requested.background).mid(1).toLatin1() +
                    "\r\n";
        commands += requested.borderEnabled ? "border on\r\n"
                                             : "border off\r\n";
        commands += "border-color " +
                    colorText(requested.border).mid(1).toLatin1() + "\r\n";
        commands += requested.borderDotted ? "border-style dotted\r\n"
                                           : "border-style solid\r\n";
        commands += requested.stretch ? "scale fit\r\n"
                                      : "scale native\r\n";

        int phase = current.phaseTrim;
        while (phase < requested.phaseTrim) {
            commands += "phase +\r\n";
            ++phase;
        }
        while (phase > requested.phaseTrim) {
            commands += "phase -\r\n";
            --phase;
        }
        if (save) {
            commands += "save\r\n";
        }
        commands += "screen\r\n";

        receiveBuffer_.clear();
        state_ = State::AwaitingScreenMode;
        resumingScreenMode_ = true;
        stateClock_.restart();
        statusBar()->showMessage(
            save ? QStringLiteral("Applying and saving adapter settings…")
                 : QStringLiteral("Applying adapter settings…"));
        if (!serial_.write(commands)) {
            handleConnectionLoss();
        }
    }

    void resumeScreenMode() {
        if (!serial_.isOpen()) {
            if (state_ != State::Disconnected) {
                handleConnectionLoss();
            }
            return;
        }
        receiveBuffer_.clear();
        state_ = State::AwaitingScreenMode;
        resumingScreenMode_ = true;
        stateClock_.restart();
        statusBar()->showMessage(
            QStringLiteral("Returning to live screen mode…"));
        if (!serial_.write(QByteArrayLiteral("screen\r\n"))) {
            handleConnectionLoss();
        }
    }

    void serviceConnection() {
        if (!serial_.isOpen()) {
            return;
        }

        bool ok = true;
        const QByteArray incoming = serial_.readAvailable(&ok);
        if (!ok) {
            handleConnectionLoss();
            return;
        }
        receiveBuffer_.append(incoming);

        if (state_ == State::Probing) {
            if (receiveBuffer_.contains("P2000M VID2VGA firmware")) {
                receiveBuffer_.clear();
                state_ = State::AwaitingScreenMode;
                stateClock_.restart();
                serial_.write(QByteArrayLiteral("screen\r\n"));
            } else if (stateClock_.elapsed() > 3000) {
                tryNextCandidate();
            }
            return;
        }

        if (state_ == State::AwaitingScreenMode) {
            if (receiveBuffer_.contains("SCREEN mode=binary version=1")) {
                receiveBuffer_.clear();
                state_ = State::Streaming;
                if (!resumingScreenMode_) {
                    frameCount_ = 0;
                    crcErrors_ = 0;
                }
                smoothedFps_ = 0.0;
                frameClock_.invalidate();
                connectButton_->setEnabled(false);
                connectAction_->setEnabled(false);
                disconnectButton_->setEnabled(true);
                disconnectAction_->setEnabled(true);
                configureAction_->setEnabled(true);
                connectionLabel_->setText(
                    QStringLiteral("Connected to %1").arg(serial_.name()));
                statusBar()->showMessage(
                    QStringLiteral("Receiving 640×288 frames from %1")
                        .arg(serial_.name()));
                resumingScreenMode_ = false;
                requestFrame();
            } else if (stateClock_.elapsed() >
                       (resumingScreenMode_ ? 5000 : 1200)) {
                if (resumingScreenMode_) {
                    handleConnectionLoss();
                } else {
                    tryNextCandidate();
                }
            }
            return;
        }

        if (state_ == State::AwaitingConsole) {
            if (receiveBuffer_.contains("Command mode.") &&
                receiveBuffer_.contains("vid2vga> ")) {
                receiveBuffer_.clear();
                state_ = State::QueryingSettings;
                stateClock_.restart();
                if (!serial_.write(QByteArrayLiteral("settings\r\n"))) {
                    handleConnectionLoss();
                }
            } else if (stateClock_.elapsed() > 2000) {
                configurationFailed(QStringLiteral(
                    "The adapter did not return to console mode."));
            }
            return;
        }

        if (state_ == State::QueryingSettings) {
            if (receiveBuffer_.contains("vid2vga> ")) {
                DeviceSettings settings;
                if (!parseDeviceSettings(receiveBuffer_, &settings)) {
                    configurationFailed(QStringLiteral(
                        "The adapter returned an unrecognized settings record."));
                    return;
                }
                receiveBuffer_.clear();
                showConfigurationDialog(settings);
            } else if (stateClock_.elapsed() > 2000) {
                configurationFailed(QStringLiteral(
                    "Timed out while reading the adapter settings."));
            }
            return;
        }

        if (state_ == State::Streaming) {
            processFrames();
        }
    }

    void processFrames() {
        static const QByteArray magic("P2VF", 4);
        while (true) {
            const qsizetype magicOffset = receiveBuffer_.indexOf(magic);
            if (magicOffset < 0) {
                if (receiveBuffer_.size() > 3) {
                    receiveBuffer_.remove(0, receiveBuffer_.size() - 3);
                }
                return;
            }
            if (magicOffset > 0) {
                receiveBuffer_.remove(0, magicOffset);
            }
            if (receiveBuffer_.size() < kFrameHeaderSize) {
                return;
            }

            const char *header = receiveBuffer_.constData();
            const quint8 version = static_cast<quint8>(header[4]);
            const quint8 type = static_cast<quint8>(header[5]);
            const quint16 flags = loadU16(header + 6);
            const quint32 sequence = loadU32(header + 8);
            const quint16 width = loadU16(header + 16);
            const quint16 height = loadU16(header + 18);
            const quint16 stride = loadU16(header + 20);
            const quint16 headerSize = loadU16(header + 22);
            const quint32 payloadSize = loadU32(header + 24);
            const quint32 expectedCrc = loadU32(header + 28);
            const quint32 foreground = loadU32(header + 32);
            const quint32 background = loadU32(header + 36);
            const quint32 border = loadU32(header + 40);
            const quint32 style = loadU32(header + 44);

            const bool validHeader =
                version == 1 && type == 1 && (flags & 0x3u) == 0x3u &&
                width == kSourceWidth && height == kSourceHeight &&
                stride == kSourceWidth / 8 &&
                headerSize == kFrameHeaderSize &&
                payloadSize == kFramePayloadSize;
            if (!validHeader) {
                receiveBuffer_.remove(0, 1);
                continue;
            }
            const qsizetype recordSize =
                static_cast<qsizetype>(headerSize) + payloadSize;
            if (receiveBuffer_.size() < recordSize) {
                return;
            }

            const QByteArray payload = receiveBuffer_.mid(
                headerSize, static_cast<qsizetype>(payloadSize));
            receiveBuffer_.remove(0, recordSize);
            if (crc32(payload) != expectedCrc) {
                ++crcErrors_;
                statusBar()->showMessage(
                    QStringLiteral("Discarded frame %1: CRC mismatch")
                        .arg(sequence));
                requestFrame();
                continue;
            }

            // Grant the next frame before doing GUI-side pixel expansion so
            // USB and rendering overlap instead of adding their latencies.
            requestFrame();
            screen_->setFrame(renderFrame(payload, foreground, background,
                                          border, style));
            screenshotAction_->setEnabled(true);
            ++frameCount_;
            if (frameClock_.isValid()) {
                const qint64 elapsed = frameClock_.restart();
                if (elapsed > 0) {
                    const double instantaneous = 1000.0 / elapsed;
                    smoothedFps_ = smoothedFps_ == 0.0
                                       ? instantaneous
                                       : smoothedFps_ * 0.85 +
                                             instantaneous * 0.15;
                }
            } else {
                frameClock_.start();
            }
            statusBar()->showMessage(
                QStringLiteral("%1  •  frame %2  •  %3 fps  •  CRC errors %4")
                    .arg(serial_.name())
                    .arg(sequence)
                    .arg(smoothedFps_, 0, 'f', 1)
                    .arg(crcErrors_));
        }
    }

    QImage renderFrame(const QByteArray &payload, quint32 foreground,
                       quint32 background, quint32 border,
                       quint32 style) const {
        const QRgb foregroundRgb = 0xff000000u | (foreground & 0x00ffffffu);
        const QRgb backgroundRgb = 0xff000000u | (background & 0x00ffffffu);
        const QRgb borderRgb = 0xff000000u | (border & 0x00ffffffu);
        const bool borderEnabled = (style & 0x1u) != 0u;
        const bool borderDotted = (style & 0x2u) != 0u;
        const bool stretch = (style & 0x4u) != 0u;

        QImage image(kSourceWidth, kVgaHeight, QImage::Format_RGB32);
        image.fill(backgroundRgb);
        const auto *bytes = reinterpret_cast<const unsigned char *>(
            payload.constData());

        std::array<std::array<QRgb, 8>, 256> pixelLookup = {};
        for (unsigned value = 0; value < pixelLookup.size(); ++value) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                pixelLookup[value][bit] =
                    (value & (1u << (7u - bit))) != 0u
                        ? foregroundRgb
                        : backgroundRgb;
            }
        }

        for (int vgaY = 0; vgaY < kVgaHeight; ++vgaY) {
            int sourceY = 0;
            int visibleY = 0;
            int visibleHeight = 0;
            if (stretch) {
                sourceY = (vgaY * 3 + 1) / 5;
                visibleY = vgaY;
                visibleHeight = kVgaHeight;
            } else {
                if (vgaY < 96 || vgaY >= 96 + kSourceHeight) {
                    continue;
                }
                sourceY = vgaY - 96;
                visibleY = sourceY;
                visibleHeight = kSourceHeight;
            }

            QRgb *destination =
                reinterpret_cast<QRgb *>(image.scanLine(vgaY));
            for (int wordIndex = 0; wordIndex < kSourceWidth / 32;
                 ++wordIndex) {
                const int offset = sourceY * (kSourceWidth / 8) +
                                   wordIndex * 4;
                const quint32 word =
                    static_cast<quint32>(bytes[offset]) |
                    (static_cast<quint32>(bytes[offset + 1]) << 8u) |
                    (static_cast<quint32>(bytes[offset + 2]) << 16u) |
                    (static_cast<quint32>(bytes[offset + 3]) << 24u);
                QRgb *pixels = destination + wordIndex * 32;
                std::memcpy(pixels, pixelLookup[(word >> 24u) & 0xffu].data(),
                            8u * sizeof(QRgb));
                std::memcpy(pixels + 8,
                            pixelLookup[(word >> 16u) & 0xffu].data(),
                            8u * sizeof(QRgb));
                std::memcpy(pixels + 16,
                            pixelLookup[(word >> 8u) & 0xffu].data(),
                            8u * sizeof(QRgb));
                std::memcpy(pixels + 24, pixelLookup[word & 0xffu].data(),
                            8u * sizeof(QRgb));
            }

            if (!borderEnabled) {
                continue;
            }
            const bool horizontal =
                visibleY == 0 || visibleY + 1 == visibleHeight;
            if (horizontal) {
                for (int x = 0; x < kSourceWidth; ++x) {
                    if (!borderDotted || (x & 3) < 2) {
                        destination[x] = borderRgb;
                    }
                }
            }
            if (!borderDotted || horizontal || (visibleY & 3) < 2) {
                destination[0] = borderRgb;
                destination[kSourceWidth - 1] = borderRgb;
            }
        }
        return image;
    }

    void requestFrame() {
        if (state_ == State::Streaming &&
            !serial_.write(QByteArrayLiteral("frame\r\n"))) {
            handleConnectionLoss();
        }
    }

    void disconnectFromAdapter(bool userRequested) {
        autoReconnect_ = !userRequested;
        if (serial_.isOpen()) {
            state_ = State::Disconnecting;
            serial_.write(QByteArrayLiteral("console\r\n"));
            QTimer::singleShot(100, this, [this] {
                serial_.close();
                state_ = State::Disconnected;
                connectButton_->setEnabled(true);
                connectAction_->setEnabled(true);
                disconnectButton_->setEnabled(false);
                disconnectAction_->setEnabled(false);
                configureAction_->setEnabled(false);
                connectionLabel_->setText(QStringLiteral("Not connected"));
                statusBar()->showMessage(QStringLiteral("Disconnected"));
                scheduleRediscovery();
            });
        }
    }

    void handleConnectionLoss() {
        serial_.close();
        state_ = State::Disconnected;
        connectButton_->setEnabled(true);
        connectAction_->setEnabled(true);
        disconnectButton_->setEnabled(false);
        disconnectAction_->setEnabled(false);
        configureAction_->setEnabled(false);
        connectionLabel_->setText(QStringLiteral("Connection lost"));
        statusBar()->showMessage(
            QStringLiteral("Adapter disconnected; searching again…"));
        scheduleRediscovery();
    }

    ScreenWidget *screen_ = nullptr;
    QWidget *controlsWidget_ = nullptr;
    QLabel *connectionLabel_ = nullptr;
    QPushButton *connectButton_ = nullptr;
    QPushButton *disconnectButton_ = nullptr;
    QAction *connectAction_ = nullptr;
    QAction *disconnectAction_ = nullptr;
    QAction *configureAction_ = nullptr;
    QAction *screenshotAction_ = nullptr;
    QAction *fullScreenAction_ = nullptr;
    QTimer pollTimer_;
    QElapsedTimer stateClock_;
    QElapsedTimer frameClock_;
    WindowsSerialPort serial_;
    QStringList candidates_;
    QStringList unavailableCandidates_;
    qsizetype candidateIndex_ = 0;
    int probedCandidateCount_ = 0;
    QByteArray receiveBuffer_;
    State state_ = State::Disconnected;
    bool autoReconnect_ = true;
    bool rediscoveryScheduled_ = false;
    bool resumingScreenMode_ = false;
    double smoothedFps_ = 0.0;
    quint64 frameCount_ = 0;
    quint64 crcErrors_ = 0;
};

}  // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("P2000M VID2VGA Viewer"));
    QApplication::setOrganizationName(QStringLiteral("P2000M VID2VGA"));

    MainWindow window;
    window.show();
    return application.exec();
}
