/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.cpp
 * @brief Cross-platform Qt 6 viewer and USB console for P2000M VID2VGA.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <utility>

#ifdef _WIN32
#include <cwchar>
#include <vector>
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>
#else
#include "adapter_serial_port.h"
#endif

#include <QApplication>
#include <QAction>
#include <QActionGroup>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "metric_graph_widget.h"
#include "p2000m_packbits.h"
#include "p2000m_phosphor_noise.h"
#include "phosphor_afterglow.h"
#include "signal_loss_screen.h"

namespace {

#ifdef _WIN32
/** Raspberry Pi USB vendor identifier used to shortlist Pico CDC devices. */
constexpr quint16 kPicoVendorId = 0x2e8a;
#endif
/** Bytes in the version-one binary screen-record header. */
constexpr int kFrameHeaderSize = 48;
/** Bytes in one unpacked 640 x 288 one-bit source framebuffer. */
constexpr int kFramePayloadSize = 640 * 288 / 8;
/** PackBits upper bound used to reject corrupt payload-size fields. */
constexpr int kMaximumPackBitsSize =
    kFramePayloadSize +
    (kFramePayloadSize + P2000M_PACKBITS_MAX_RUN - 1) /
        P2000M_PACKBITS_MAX_RUN;
/** Reconstructed source width in pixels. */
constexpr int kSourceWidth = 640;
/** Reconstructed source height in lines. */
constexpr int kSourceHeight = 288;
/** Active line count reproduced by the VGA monitor view. */
constexpr int kVgaHeight = 480;
/** Match the firmware's missing-source-frame watchdog interval. */
constexpr qint64 kSignalLossTimeoutMs = 100;
/** Fixed black bezel surrounding the live monitor viewport. */
constexpr int kMonitorBezelWidth = 12;
/** Corner radius of the live monitor's outer bezel. */
constexpr int kMonitorBezelRadius = 14;
/** Subtle rounding retained on the inner screen aperture. */
constexpr int kMonitorScreenRadius = 5;
/** Optional black margin added around captured screenshots and recordings. */
constexpr int kCaptureBorderWidth = 12;
/** Nominal P2000M source-frame rate represented by sequence numbers. */
constexpr double kNominalSourceFrameRate = 50.094;
/** Default brightness half-life for the optional phosphor effect. */
constexpr int kDefaultAfterglowHalfLifeMs = 120;
/** Shortest configurable phosphor brightness half-life. */
constexpr int kMinimumAfterglowHalfLifeMs = 10;
/** Longest configurable phosphor brightness half-life. */
constexpr int kMaximumAfterglowHalfLifeMs = 1000;
/** Green associated with classic Matrix-style monochrome terminals. */
constexpr quint32 kMatrixGreen = 0x00ff41u;
/** Warm amber used by many late-1970s and early-1980s monochrome displays. */
constexpr quint32 kRetroAmber = 0xffb000u;
/** Slightly warm white which avoids the harshness of full RGB white. */
constexpr quint32 kWarmOffWhite = 0xf2f0e6u;

/** Add the optional fixed-width black margin used by exported captures. */
QImage addCaptureBorder(const QImage &frame) {
    QImage bordered(frame.width() + 2 * kCaptureBorderWidth,
                    frame.height() + 2 * kCaptureBorderWidth,
                    QImage::Format_RGB32);
    bordered.fill(Qt::black);
    QPainter painter(&bordered);
    painter.drawImage(kCaptureBorderWidth, kCaptureBorderWidth, frame);
    return bordered;
}

/** Build the reflected CRC-32 lookup table used for frame validation. */
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

/** Load an unaligned little-endian 16-bit protocol field. */
quint16 loadU16(const char *data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return static_cast<quint16>(bytes[0]) |
           (static_cast<quint16>(bytes[1]) << 8u);
}

/** Load an unaligned little-endian 32-bit protocol field. */
quint32 loadU32(const char *data) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    return static_cast<quint32>(bytes[0]) |
           (static_cast<quint32>(bytes[1]) << 8u) |
           (static_cast<quint32>(bytes[2]) << 16u) |
           (static_cast<quint32>(bytes[3]) << 24u);
}

/** Calculate the standard reflected CRC-32 used by screen records. */
quint32 crc32(const QByteArray &data) {
    quint32 crc = 0xffffffffu;
    for (const unsigned char byte : data) {
        crc = (crc >> 8u) ^ kCrc32Table[(crc ^ byte) & 0xffu];
    }
    return ~crc;
}

/** Display and capture settings exchanged through the firmware console. */
struct DeviceSettings {
    /** Foreground color in 0xRRGGBB form. */
    quint32 foreground = 0xffffffu;
    /** Background color in 0xRRGGBB form. */
    quint32 background = 0x000000u;
    /** Border color in 0xRRGGBB form. */
    quint32 border = 0xff00ffu;
    /** Whether the one-pixel screen border is enabled. */
    bool borderEnabled = false;
    /** Whether the border uses the two-on, two-off dotted pattern. */
    bool borderDotted = false;
    /** Whether 288 source lines are stretched over all 480 VGA lines. */
    bool stretch = false;
    /** Density of firmware/viewer foreground phosphor grain. */
    int noiseLevel = P2000M_PHOSPHOR_NOISE_OFF;
    /** Whether the connected firmware reports the v0.4 noise setting. */
    bool noiseSupported = false;
    /** Manual capture phase adjustment in 63 MHz sampling ticks. */
    int phaseTrim = 0;
    /** Firmware-reported persistence state. */
    QString storage = QStringLiteral("default");
};

/** Format a 0xRRGGBB value as the console's uppercase #RRGGBB notation. */
QString colorText(quint32 rgb) {
    return QStringLiteral("#%1").arg(rgb & 0x00ffffffu, 6, 16,
                                     QLatin1Char('0')).toUpper();
}

/**
 * Parse the machine-readable DISPLAY record returned by the firmware.
 *
 * @param consoleText Complete or partial console response containing DISPLAY.
 * @param settings Destination updated only after all fields validate.
 * @return true when a valid settings record was found; false otherwise.
 */
bool parseDeviceSettings(const QByteArray &consoleText,
                         DeviceSettings *settings) {
    static const QRegularExpression expression(QStringLiteral(
        R"(DISPLAY foreground=#([0-9a-fA-F]{6}) background=#([0-9a-fA-F]{6}) border=(on|off) border_color=#([0-9a-fA-F]{6}) border_style=(solid|dotted) scale=(fit-5:3|native-1:1)(?: noise=(off|low|medium|high))? phase_trim=(-?[0-9]+) storage=([a-zA-Z]+))"));
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
    const int phase = match.captured(8).toInt(&phaseOk);
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
    const QString noise = match.captured(7);
    settings->noiseSupported = !noise.isEmpty();
    if (noise == QStringLiteral("low")) {
        settings->noiseLevel = P2000M_PHOSPHOR_NOISE_LOW;
    } else if (noise == QStringLiteral("medium")) {
        settings->noiseLevel = P2000M_PHOSPHOR_NOISE_MEDIUM;
    } else if (noise == QStringLiteral("high")) {
        settings->noiseLevel = P2000M_PHOSPHOR_NOISE_HIGH;
    } else {
        settings->noiseLevel = P2000M_PHOSPHOR_NOISE_OFF;
    }
    settings->phaseTrim = phase;
    settings->storage = match.captured(9);
    return true;
}

#ifdef _WIN32

/**
 * Enumerate present Windows COM ports belonging to Raspberry Pi Pico devices.
 *
 * Firmware identity is deliberately verified later over the console because
 * the USB vendor identifier alone cannot distinguish this adapter.
 *
 * @return Stable, case-insensitively sorted COM-port names.
 */
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

/** Synchronous, nonblocking-read wrapper around the Windows COM-port API. */
class WindowsSerialPort {
public:
    /** Close the native handle before the wrapper is destroyed. */
    ~WindowsSerialPort() { close(); }

    /**
     * Open and configure a USB CDC COM port for binary traffic.
     *
     * @param portName Windows port name such as COM3.
     * @return true when the handle and communication parameters are ready.
     */
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

    /** Close the port and clear its user-facing name. */
    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            EscapeCommFunction(handle_, CLRDTR);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        name_.clear();
    }

    /** Return whether a valid Windows port handle is owned. */
    bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    /** Return the COM-port name associated with the current handle. */
    QString name() const { return name_; }

    /** Write an entire command, retrying until every byte is accepted. */
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

    /**
     * Drain currently queued input without waiting for future data.
     *
     * @param ok Receives false when a Windows communication call fails.
     * @return All bytes available during this bounded drain operation.
     */
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
    /** Owned Windows file handle, or INVALID_HANDLE_VALUE while closed. */
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    /** User-facing COM-port name corresponding to handle_. */
    QString name_;
};

#endif

/** Color-selection button that previews and returns one 24-bit RGB value. */
class ColorButton final : public QPushButton {
public:
    /** Construct the button with an initial 0xRRGGBB color. */
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

    /** Return the selected color in 0xRRGGBB form. */
    quint32 color() const { return rgb_; }

    /** Update the selected color, label, and contrast-aware button style. */
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
    /** Selected color in 0xRRGGBB form. */
    quint32 rgb_ = 0;
};

/** Modal editor for display geometry, colors, and capture phase. */
class ConfigurationDialog final : public QDialog {
public:
    /** Distinguishes runtime-only application from apply-and-save. */
    enum Result {
        Apply = QDialog::Accepted,
        ApplyAndSave = 2,
    };

    /** Populate controls from the adapter's current settings. */
    ConfigurationDialog(const DeviceSettings &settings,
                        QWidget *parent = nullptr)
        : QDialog(parent), noiseSupported_(settings.noiseSupported) {
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
        foregroundPreset_ = new QComboBox(this);
        foregroundPreset_->addItem(QStringLiteral("Custom color"), 0u);
        foregroundPreset_->addItem(
            QStringLiteral("Matrix green (%1)").arg(colorText(kMatrixGreen)),
            kMatrixGreen);
        foregroundPreset_->addItem(
            QStringLiteral("Retro amber (%1)").arg(colorText(kRetroAmber)),
            kRetroAmber);
        foregroundPreset_->addItem(
            QStringLiteral("Warm off-white (%1)")
                .arg(colorText(kWarmOffWhite)),
            kWarmOffWhite);
        syncForegroundPreset();
        connect(foregroundPreset_, &QComboBox::currentIndexChanged,
                this, [this](int index) {
                    if (index > 0) {
                        foreground_->setColor(
                            foregroundPreset_->currentData().toUInt());
                    }
                });
        // ColorButton's own click handler runs first and opens the picker.
        connect(foreground_, &QPushButton::clicked,
                this, [this] { syncForegroundPreset(); });
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
        noise_ = new QComboBox(this);
        noise_->addItem(QStringLiteral("Off"),
                        P2000M_PHOSPHOR_NOISE_OFF);
        noise_->addItem(QStringLiteral("Low (subtle)"),
                        P2000M_PHOSPHOR_NOISE_LOW);
        noise_->addItem(QStringLiteral("Medium"),
                        P2000M_PHOSPHOR_NOISE_MEDIUM);
        noise_->addItem(QStringLiteral("High"),
                        P2000M_PHOSPHOR_NOISE_HIGH);
        noise_->setCurrentIndex(settings.noiseLevel);
        noise_->setEnabled(noiseSupported_);
        noise_->setToolTip(noiseSupported_
                               ? QStringLiteral(
                                     "Randomly dims foreground pixels by one "
                                     "RGB444 DAC step")
                               : QStringLiteral(
                                     "Requires adapter firmware v0.4.0 or newer"));
        phaseTrim_ = new QSpinBox(this);
        phaseTrim_->setRange(-4, 4);
        phaseTrim_->setValue(settings.phaseTrim);
        phaseTrim_->setSuffix(QStringLiteral(" tick(s)"));
        auto *storage = new QLabel(settings.storage, this);

        form->addRow(QStringLiteral("Foreground preset:"),
                     foregroundPreset_);
        form->addRow(QStringLiteral("Foreground color:"), foreground_);
        form->addRow(QStringLiteral("Background:"), background_);
        form->addRow(QStringLiteral("Border:"), borderEnabled_);
        form->addRow(QStringLiteral("Border color:"), borderColor_);
        form->addRow(QStringLiteral("Border style:"), borderStyle_);
        form->addRow(QStringLiteral("Vertical scaling:"), scaling_);
        form->addRow(QStringLiteral("Phosphor grain:"), noise_);
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

    /** Return a complete settings snapshot assembled from the controls. */
    DeviceSettings settings() const {
        DeviceSettings result;
        result.foreground = foreground_->color();
        result.background = background_->color();
        result.border = borderColor_->color();
        result.borderEnabled = borderEnabled_->isChecked();
        result.borderDotted = borderStyle_->currentData().toBool();
        result.stretch = scaling_->currentData().toBool();
        result.noiseLevel = noise_->currentData().toInt();
        result.noiseSupported = noiseSupported_;
        result.phaseTrim = phaseTrim_->value();
        return result;
    }

private:
    /** Restore every editable control from one settings snapshot. */
    void loadSettings(const DeviceSettings &settings) {
        foreground_->setColor(settings.foreground);
        syncForegroundPreset();
        background_->setColor(settings.background);
        borderColor_->setColor(settings.border);
        borderEnabled_->setChecked(settings.borderEnabled);
        borderStyle_->setCurrentIndex(settings.borderDotted ? 1 : 0);
        scaling_->setCurrentIndex(settings.stretch ? 1 : 0);
        noise_->setCurrentIndex(settings.noiseLevel);
        phaseTrim_->setValue(settings.phaseTrim);
    }

    /** Select the named preset matching the color button, or Custom color. */
    void syncForegroundPreset() {
        const quint32 foreground = foreground_->color();
        int matchingIndex = 0;
        for (int index = 1; index < foregroundPreset_->count(); ++index) {
            if (foregroundPreset_->itemData(index).toUInt() == foreground) {
                matchingIndex = index;
                break;
            }
        }
        foregroundPreset_->setCurrentIndex(matchingIndex);
    }

    /** Foreground color selector. */
    ColorButton *foreground_ = nullptr;
    /** Named foreground-color shortcuts, plus the custom-color fallback. */
    QComboBox *foregroundPreset_ = nullptr;
    /** Background color selector. */
    ColorButton *background_ = nullptr;
    /** Border color selector. */
    ColorButton *borderColor_ = nullptr;
    /** Border enable control. */
    QCheckBox *borderEnabled_ = nullptr;
    /** Solid/dotted border selector. */
    QComboBox *borderStyle_ = nullptr;
    /** Native/fit vertical scaling selector. */
    QComboBox *scaling_ = nullptr;
    /** Foreground phosphor-grain density selector. */
    QComboBox *noise_ = nullptr;
    /** Manual capture phase adjustment. */
    QSpinBox *phaseTrim_ = nullptr;
    /** Whether the connected firmware accepts the noise command. */
    bool noiseSupported_ = false;
};

/** Paints the reconstructed VGA frame and records actual presentation cost. */
class ScreenWidget final : public QWidget {
public:
    /** Construct an expanding widget including the fixed monitor bezel. */
    explicit ScreenWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(kSourceWidth + 2 * kMonitorBezelWidth,
                       kVgaHeight + 2 * kMonitorBezelWidth);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    /** Publish a new complete frame and schedule a coalescible repaint. */
    void setFrame(QImage frame, quint32 sourceFrameStep = 1) {
        sourceFrame_ = std::move(frame);
        if (afterglowEnabled_ && afterglowHistoryValid_) {
            const double elapsedMilliseconds =
                1000.0 * std::max(sourceFrameStep, 1u) /
                kNominalSourceFrameRate;
            const double retention = std::exp2(
                -elapsedMilliseconds / afterglowHalfLifeMs_);
            frame_ = p2000m::applyPhosphorAfterglow(
                sourceFrame_, frame_, retention);
        } else {
            frame_ = sourceFrame_;
        }
        afterglowHistoryValid_ = afterglowEnabled_;
        ++frameSerial_;
        update();
    }

    /** Enable or disable temporal phosphor persistence. */
    void setAfterglowEnabled(bool enabled) {
        if (afterglowEnabled_ == enabled) {
            return;
        }
        afterglowEnabled_ = enabled;
        resetAfterglow();
    }

    /** Set the time in which an unrefreshed trace loses half its brightness. */
    void setAfterglowHalfLife(int milliseconds) {
        afterglowHalfLifeMs_ = std::clamp(
            milliseconds, kMinimumAfterglowHalfLifeMs,
            kMaximumAfterglowHalfLifeMs);
    }

    /** Drop temporal history without discarding the latest source image. */
    void resetAfterglow() {
        frame_ = sourceFrame_;
        afterglowHistoryValid_ = false;
        ++frameSerial_;
        update();
    }

    /** Select smooth interpolation or nearest-neighbor scaling. */
    void setSmoothScaling(bool enabled) {
        smoothScaling_ = enabled;
        update();
    }

    /** Enable or disable whole-number pixel scaling. */
    void setIntegerScaling(bool enabled) {
        integerScaling_ = enabled;
        update();
    }

    /** Return whether a decoded image is available. */
    bool hasFrame() const { return !sourceFrame_.isNull(); }

    /** Save the unscaled reconstructed VGA image with an optional margin. */
    bool saveFrame(const QString &filename, bool addBorder) const {
        if (sourceFrame_.isNull()) {
            return false;
        }
        return (addBorder ? addCaptureBorder(sourceFrame_) : sourceFrame_)
            .save(filename);
    }

    /** Return the smoothed rate of unique frames actually painted. */
    double paintFps() const { return smoothedPaintFps_; }
    /** Return the smoothed wall time of a unique-frame paint operation. */
    double paintMilliseconds() const { return smoothedPaintMilliseconds_; }

protected:
    /** Paint one aspect-correct image and update presentation telemetry. */
    void paintEvent(QPaintEvent *) override {
        QElapsedTimer duration;
        duration.start();
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());

        const QSize availableSize(
            std::max(0, width() - 2 * kMonitorBezelWidth),
            std::max(0, height() - 2 * kMonitorBezelWidth));
        const QSize frameSize = frame_.isNull()
                                    ? QSize(kSourceWidth, kVgaHeight)
                                    : frame_.size();

        QSize targetSize;
        if (integerScaling_) {
            const int factor = std::min(
                availableSize.width() / frameSize.width(),
                availableSize.height() / frameSize.height());
            targetSize = factor >= 1
                             ? frameSize * factor
                             : frameSize.scaled(availableSize,
                                                Qt::KeepAspectRatio);
        } else {
            targetSize = frameSize.scaled(availableSize,
                                          Qt::KeepAspectRatio);
        }
        const QRect target(
            QPoint((width() - targetSize.width()) / 2,
                   (height() - targetSize.height()) / 2),
            targetSize);

        const QRect bezel = target.adjusted(
            -kMonitorBezelWidth, -kMonitorBezelWidth,
            kMonitorBezelWidth, kMonitorBezelWidth);
        QPainterPath bezelPath;
        bezelPath.addRoundedRect(QRectF(bezel), kMonitorBezelRadius,
                                 kMonitorBezelRadius);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillPath(bezelPath, Qt::black);

        QPainterPath screenPath;
        screenPath.addRoundedRect(QRectF(target), kMonitorScreenRadius,
                                  kMonitorScreenRadius);
        painter.save();
        painter.setClipPath(screenPath);
        painter.fillRect(target, Qt::black);
        if (frame_.isNull()) {
            painter.setPen(QColor(180, 180, 180));
            painter.drawText(target, Qt::AlignCenter,
                             QStringLiteral("Waiting for P2000M video"));
            painter.restore();
            return;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              smoothScaling_ && !integerScaling_);
        painter.drawImage(target, frame_);
        painter.restore();

        if (paintedSerial_ != frameSerial_) {
            paintedSerial_ = frameSerial_;
            const double milliseconds =
                static_cast<double>(duration.nsecsElapsed()) / 1000000.0;
            smoothedPaintMilliseconds_ =
                smoothedPaintMilliseconds_ == 0.0
                    ? milliseconds
                    : smoothedPaintMilliseconds_ * 0.85 +
                          milliseconds * 0.15;
            if (paintClock_.isValid()) {
                const qint64 elapsed = paintClock_.restart();
                if (elapsed > 0) {
                    const double instantaneous = 1000.0 / elapsed;
                    smoothedPaintFps_ = smoothedPaintFps_ == 0.0
                                            ? instantaneous
                                            : smoothedPaintFps_ * 0.85 +
                                                  instantaneous * 0.15;
                }
            } else {
                paintClock_.start();
            }
        }
    }

private:
    /** Most recent unfiltered reconstructed VGA frame. */
    QImage sourceFrame_;
    /** Most recent complete 640 x 480 image. */
    QImage frame_;
    /** Whether temporal phosphor persistence is applied to new frames. */
    bool afterglowEnabled_ = false;
    /** Brightness half-life used by the temporal persistence filter. */
    int afterglowHalfLifeMs_ = kDefaultAfterglowHalfLifeMs;
    /** Whether frame_ contains history eligible for the next blend. */
    bool afterglowHistoryValid_ = false;
    /** Whether noninteger scaling uses smooth interpolation. */
    bool smoothScaling_ = true;
    /** Whether image dimensions are constrained to integer multiples. */
    bool integerScaling_ = false;
    /** Serial assigned to each image accepted by setFrame(). */
    quint64 frameSerial_ = 0;
    /** Serial of the most recent image included in paint telemetry. */
    quint64 paintedSerial_ = 0;
    /** Interval timer between uniquely painted frames. */
    QElapsedTimer paintClock_;
    /** Exponentially smoothed unique-frame presentation rate. */
    double smoothedPaintFps_ = 0.0;
    /** Exponentially smoothed unique-frame paint duration. */
    double smoothedPaintMilliseconds_ = 0.0;
};

/** Coordinates discovery, protocol state, configuration, and presentation. */
class MainWindow final : public QMainWindow {
public:
    /** Build the complete interface and start automatic adapter discovery. */
    MainWindow() {
        setWindowTitle(QStringLiteral("P2000M VID2VGA Viewer v%1")
                           .arg(QStringLiteral(P2000M_VIEWER_VERSION)));
        setWindowIcon(QIcon(QStringLiteral(
            ":/icons/p2000m-vid2vga-viewer.png")));
        resize(1160, 760);

        const QSettings settings;
        afterglowEnabled_ = settings.value(
            QStringLiteral("view/phosphorAfterglowEnabled"), false).toBool();
        afterglowHalfLifeMs_ = std::clamp(
            settings.value(QStringLiteral("view/phosphorAfterglowHalfLifeMs"),
                           kDefaultAfterglowHalfLifeMs).toInt(),
            kMinimumAfterglowHalfLifeMs, kMaximumAfterglowHalfLifeMs);
        createMenus();

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        auto *contentLayout = new QHBoxLayout;
        contentLayout->setContentsMargins(0, 0, 0, 0);
        screen_ = new ScreenWidget(central);
        screen_->setAfterglowHalfLife(afterglowHalfLifeMs_);
        screen_->setAfterglowEnabled(afterglowEnabled_);
        contentLayout->addWidget(screen_, 1);
        statisticsPanel_ = createStatisticsPanel(central);
        contentLayout->addWidget(statisticsPanel_);
        layout->addLayout(contentLayout, 1);

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
        connect(
            &ffmpegProcess_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                handleRecordingFinished(exitCode, exitStatus);
            });
        pollTimer_.setTimerType(Qt::PreciseTimer);
        pollTimer_.start(1);
        QTimer::singleShot(0, this, [this] { beginDiscovery(); });
    }

protected:
    /** Restore console mode before allowing the native window to close. */
    void closeEvent(QCloseEvent *event) override {
        closing_ = true;
        if (recording_) {
            stopRecording();
        }
        if (recordingStopping_ &&
            !ffmpegProcess_.waitForFinished(5000)) {
            ffmpegProcess_.terminate();
            if (!ffmpegProcess_.waitForFinished(1000)) {
                ffmpegProcess_.kill();
                ffmpegProcess_.waitForFinished(1000);
            }
        }
        if (serial_.isOpen()) {
            serial_.write(QByteArrayLiteral("console\r\n"));
            serial_.close();
        }
        event->accept();
    }

private:
    /** Mutually exclusive phases of discovery and serial protocol handling. */
    enum class State {
        Disconnected,
        Probing,
        AwaitingScreenMode,
        Streaming,
        AwaitingConsole,
        QueryingSettings,
        Configuring,
        SwitchingEncoding,
        Disconnecting,
    };

    /** Build the scrollable right-hand collection of live metric graphs. */
    QWidget *createStatisticsPanel(QWidget *parent) {
        auto *scrollArea = new QScrollArea(parent);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setMinimumWidth(300);
        scrollArea->setMaximumWidth(330);

        auto *container = new QWidget(scrollArea);
        auto *statisticsLayout = new QVBoxLayout(container);
        statisticsLayout->setContentsMargins(6, 0, 2, 0);
        statisticsLayout->setSpacing(2);

        auto *heading = new QLabel(QStringLiteral("Live performance"),
                                   container);
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        headingFont.setPointSizeF(headingFont.pointSizeF() + 1.0);
        heading->setFont(headingFont);
        statisticsLayout->addWidget(heading);

        auto *explanation = new QLabel(
            QStringLiteral("Approximately 60 seconds of history"),
            container);
        explanation->setStyleSheet(
            QStringLiteral("color: palette(mid); padding-bottom: 3px;"));
        statisticsLayout->addWidget(explanation);

        const auto addGraph = [&](MetricGraphWidget **destination,
                                  const QString &title,
                                  const QColor &color) {
            *destination = new MetricGraphWidget(title, color, container);
            statisticsLayout->addWidget(*destination);
        };
        addGraph(&usbGraph_, QStringLiteral("USB throughput"),
                 QColor(0, 137, 168));
        addGraph(&frameRateGraph_, QStringLiteral("Received frames"),
                 QColor(0, 120, 215));
        addGraph(&sourceStepGraph_, QStringLiteral("Source sequence step"),
                 QColor(108, 74, 182));
        addGraph(&payloadGraph_, QStringLiteral("Payload size"),
                 QColor(0, 153, 102));
        addGraph(&renderGraph_, QStringLiteral("Viewer rendering"),
                 QColor(214, 127, 0));
        addGraph(&unpackGraph_, QStringLiteral("PackBits unpacking"),
                 QColor(174, 89, 0));
        addGraph(&paintRateGraph_, QStringLiteral("Painted frames"),
                 QColor(42, 126, 66));
        addGraph(&paintTimeGraph_, QStringLiteral("Paint duration"),
                 QColor(87, 139, 46));
        addGraph(&firmwarePrepareGraph_,
                 QStringLiteral("Firmware preparation"),
                 QColor(190, 44, 74));
        addGraph(&firmwareEncodeGraph_, QStringLiteral("Firmware encoding"),
                 QColor(220, 70, 116));
        addGraph(&crcGraph_, QStringLiteral("CRC error rate"),
                 QColor(200, 35, 35));
        updateEncodingGraphVisibility();
        statisticsLayout->addStretch(1);

        scrollArea->setWidget(container);
        return scrollArea;
    }

    /** Create all persistent actions, menus, shortcuts, and connections. */
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
        startRecordingAction_ = fileMenu->addAction(
            QStringLiteral("Start &Recording…"));
        startRecordingAction_->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+R")));
        startRecordingAction_->setEnabled(false);
        stopRecordingAction_ = fileMenu->addAction(
            QStringLiteral("S&top Recording"));
        stopRecordingAction_->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+Shift+S")));
        stopRecordingAction_->setEnabled(false);
        fileMenu->addSeparator();
        auto *exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
        exitAction->setShortcut(QKeySequence::Quit);

        auto *deviceMenu = menuBar()->addMenu(QStringLiteral("&Adapter"));
        configureAction_ = deviceMenu->addAction(
            QStringLiteral("&Configure Adapter…"));
        configureAction_->setShortcut(
            QKeySequence(QStringLiteral("Ctrl+,")));
        configureAction_->setEnabled(false);
        auto *encodingMenu = deviceMenu->addMenu(
            QStringLiteral("Stream &Encoding"));
        auto *encodingGroup = new QActionGroup(this);
        auto *rawEncodingAction = encodingMenu->addAction(
            QStringLiteral("&Raw (recommended)"));
        auto *packBitsEncodingAction = encodingMenu->addAction(
            QStringLiteral("&PackBits (experimental)"));
        rawEncodingAction->setCheckable(true);
        packBitsEncodingAction->setCheckable(true);
        rawEncodingAction->setChecked(true);
        encodingGroup->addAction(rawEncodingAction);
        encodingGroup->addAction(packBitsEncodingAction);
        encodingGroup->setExclusive(true);

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
        auto *afterglowMenu = viewMenu->addMenu(
            QStringLiteral("CRT Phosphor &Afterglow"));
        afterglowEnabledAction_ = afterglowMenu->addAction(
            QStringLiteral("&Enabled"));
        afterglowEnabledAction_->setCheckable(true);
        afterglowEnabledAction_->setChecked(afterglowEnabled_);
        afterglowDecayAction_ = afterglowMenu->addAction(QString());
        updateAfterglowDecayActionText();
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
        connect(startRecordingAction_, &QAction::triggered, this, [this] {
            startRecording();
        });
        connect(stopRecordingAction_, &QAction::triggered, this, [this] {
            stopRecording();
        });
        connect(configureAction_, &QAction::triggered, this, [this] {
            beginConfiguration();
        });
        connect(rawEncodingAction, &QAction::triggered, this, [this] {
            setStreamEncoding(false);
        });
        connect(packBitsEncodingAction, &QAction::triggered, this, [this] {
            setStreamEncoding(true);
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
        connect(afterglowEnabledAction_, &QAction::toggled,
                this, [this](bool enabled) {
                    setAfterglowEnabled(enabled);
                });
        connect(afterglowDecayAction_, &QAction::triggered, this, [this] {
            configureAfterglowDecay();
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
            QMessageBox about(this);
            about.setWindowTitle(
                QStringLiteral("About P2000M VID2VGA Viewer"));
            about.setWindowIcon(QApplication::windowIcon());
            about.setIconPixmap(
                QApplication::windowIcon().pixmap(QSize(96, 96)));
            about.setTextFormat(Qt::RichText);
            about.setText(
                QStringLiteral(
                    "<h3>P2000M VID2VGA Viewer v%1</h3>"
                    "<p>A live Qt 6 monitor and configuration utility for "
                    "the Raspberry Pi Pico 2 P2000M video adapter.</p>"
                    "<p>Copyright © 2026 Ivo Filot<br>"
                    "Licensed under GNU GPL v3 or later.</p>"
                    "<p>Packaged distributions include FFmpeg %2 and x264 "
                    "under their GNU GPL licenses. Exact source archives, "
                    "license texts, checksums, and build configuration are "
                    "provided in the <code>licenses/ffmpeg</code> directory."
                    "</p>")
                    .arg(QStringLiteral(P2000M_VIEWER_VERSION),
                         QStringLiteral(P2000M_FFMPEG_VERSION)));
            about.exec();
        });
    }

    /** Toggle the presentation-only CRT persistence effect and persist it. */
    void setAfterglowEnabled(bool enabled) {
        afterglowEnabled_ = enabled;
        if (screen_ != nullptr) {
            screen_->setAfterglowEnabled(enabled);
        }
        QSettings().setValue(
            QStringLiteral("view/phosphorAfterglowEnabled"), enabled);
    }

    /** Ask for a brightness half-life and apply it to subsequent frames. */
    void configureAfterglowDecay() {
        bool accepted = false;
        const int milliseconds = QInputDialog::getInt(
            this, QStringLiteral("CRT Phosphor Afterglow"),
            QStringLiteral("Brightness half-life (milliseconds):"),
            afterglowHalfLifeMs_, kMinimumAfterglowHalfLifeMs,
            kMaximumAfterglowHalfLifeMs, 10, &accepted);
        if (!accepted) {
            return;
        }
        afterglowHalfLifeMs_ = milliseconds;
        screen_->setAfterglowHalfLife(milliseconds);
        QSettings().setValue(
            QStringLiteral("view/phosphorAfterglowHalfLifeMs"), milliseconds);
        updateAfterglowDecayActionText();
    }

    /** Include the active half-life in the tunable menu item. */
    void updateAfterglowDecayActionText() {
        afterglowDecayAction_->setText(
            QStringLiteral("Decay &Half-life… (%1 ms)")
                .arg(afterglowHalfLifeMs_));
        afterglowDecayAction_->setStatusTip(QStringLiteral(
            "Time for an unrefreshed phosphor trace to lose half its brightness"));
    }

    /** Show a save dialog containing the shared capture-border option. */
    QString selectCaptureFile(const QString &title,
                              const QString &suggestedFilename,
                              const QString &nameFilter,
                              const QString &defaultSuffix,
                              bool *addBorder) {
        QFileDialog dialog(this, title, suggestedFilename, nameFilter);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setFileMode(QFileDialog::AnyFile);
        dialog.setDefaultSuffix(defaultSuffix);
        // Native file pickers cannot host application-specific controls.
        dialog.setOption(QFileDialog::DontUseNativeDialog, true);

        QList<QUrl> sidebarUrls;
        const std::array locations = {
            QStandardPaths::HomeLocation,
            QStandardPaths::DownloadLocation,
            QStandardPaths::DesktopLocation,
            QStandardPaths::DocumentsLocation,
        };
        for (const auto location : locations) {
            for (const QString &path :
                 QStandardPaths::standardLocations(location)) {
                const QUrl url = QUrl::fromLocalFile(path);
                if (!path.isEmpty() && !sidebarUrls.contains(url)) {
                    sidebarUrls.append(url);
                }
            }
        }
        dialog.setSidebarUrls(sidebarUrls);

        auto *borderCheckBox = new QCheckBox(
            QStringLiteral("Add 12 px black border"), &dialog);
        borderCheckBox->setChecked(
            QSettings().value(QStringLiteral("capture/addBorder"), false)
                .toBool());
        dialog.layout()->addWidget(borderCheckBox);

        if (dialog.exec() != QDialog::Accepted ||
            dialog.selectedFiles().isEmpty()) {
            return {};
        }
        *addBorder = borderCheckBox->isChecked();
        QSettings().setValue(QStringLiteral("capture/addBorder"), *addBorder);
        return dialog.selectedFiles().constFirst();
    }

    /** Ask for a filename and save the latest unscaled framebuffer. */
    void saveScreenshot() {
        if (!screen_->hasFrame()) {
            return;
        }
        bool addBorder = false;
        const QString filename = selectCaptureFile(
            QStringLiteral("Save framebuffer"),
            QStringLiteral("p2000m-screen.png"),
            QStringLiteral("PNG image (*.png);;Bitmap image (*.bmp)"),
            QStringLiteral("png"), &addBorder);
        if (!filename.isEmpty() &&
            !screen_->saveFrame(filename, addBorder)) {
            QMessageBox::warning(
                this, QStringLiteral("Save screenshot"),
                QStringLiteral("The screenshot could not be written."));
        }
    }

    /** Locate the private packaged FFmpeg, then development fallbacks. */
    QString findFfmpegExecutable() const {
        const QDir applicationDirectory(
            QCoreApplication::applicationDirPath());
        QStringList candidates;

#ifdef _WIN32
        candidates.append(applicationDirectory.filePath(
            QStringLiteral("tools/ffmpeg.exe")));
#elif defined(Q_OS_MACOS)
        candidates.append(applicationDirectory.filePath(
            QStringLiteral("../Resources/tools/ffmpeg")));
#else
        candidates.append(applicationDirectory.filePath(
            QStringLiteral("../libexec/p2000m-vid2vga-viewer/ffmpeg")));
#endif

        // These fallbacks keep unpackaged developer builds convenient. A
        // release package always supplies and selects the private tool first.
        candidates.append({
            applicationDirectory.filePath(QStringLiteral("ffmpeg.exe")),
            applicationDirectory.filePath(QStringLiteral("ffmpeg")),
            QStandardPaths::findExecutable(QStringLiteral("ffmpeg.exe")),
            QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
        });

#ifdef _WIN32
        // Explorer-launched applications often do not inherit the MSYS2
        // shell PATH, so also check its active prefix and default locations.
        const auto appendPrefix = [&candidates](const QString &prefix) {
            if (!prefix.isEmpty()) {
                candidates.append(
                    QDir(prefix).filePath(QStringLiteral("bin/ffmpeg.exe")));
            }
        };
        appendPrefix(QString::fromLocal8Bit(qgetenv("MSYSTEM_PREFIX")));
        appendPrefix(QString::fromLocal8Bit(qgetenv("MINGW_PREFIX")));
        candidates.append({
            QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
            QStringLiteral("C:/msys64/mingw64/bin/ffmpeg.exe"),
        });
#elif defined(Q_OS_MACOS)
        // Finder does not normally inherit Homebrew's shell PATH.
        candidates.append({
            QStringLiteral("/opt/homebrew/bin/ffmpeg"),
            QStringLiteral("/usr/local/bin/ffmpeg"),
        });
#else
        candidates.append(QStringLiteral("/usr/bin/ffmpeg"));
#endif
        for (const QString &candidate : candidates) {
            if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
                return QDir::toNativeSeparators(candidate);
            }
        }
        return {};
    }

    /** Ask for an MP4 path and start an FFmpeg raw-video encoder process. */
    void startRecording() {
        if (recording_ || recordingStopping_ ||
            state_ != State::Streaming || !screen_->hasFrame()) {
            return;
        }

        const QString ffmpeg = findFfmpegExecutable();
        if (ffmpeg.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("FFmpeg not found"),
                QStringLiteral(
                    "The packaged FFmpeg recording runtime is missing or "
                    "damaged. Reinstall the viewer from an official package. "
                    "An unpackaged development build may instead use an "
                    "FFmpeg installation on PATH."));
            return;
        }

        bool addBorder = false;
        QString filename = selectCaptureFile(
            QStringLiteral("Record P2000M screen"),
            QDir::home().filePath(QStringLiteral("p2000m-recording.mp4")),
            QStringLiteral("MPEG-4 video (*.mp4)"),
            QStringLiteral("mp4"), &addBorder);
        if (filename.isEmpty()) {
            return;
        }
        if (!filename.endsWith(QStringLiteral(".mp4"),
                               Qt::CaseInsensitive)) {
            filename += QStringLiteral(".mp4");
        }

        const int recordingWidth =
            kSourceWidth + (addBorder ? 2 * kCaptureBorderWidth : 0);
        const int recordingHeight =
            kVgaHeight + (addBorder ? 2 * kCaptureBorderWidth : 0);

        ffmpegProcess_.setProgram(ffmpeg);
        ffmpegProcess_.setArguments({
            QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-nostdin"), QStringLiteral("-y"),
            QStringLiteral("-f"), QStringLiteral("rawvideo"),
            QStringLiteral("-pixel_format"), QStringLiteral("bgra"),
            QStringLiteral("-video_size"),
            QStringLiteral("%1x%2").arg(recordingWidth).arg(recordingHeight),
            QStringLiteral("-framerate"), QStringLiteral("25.047"),
            QStringLiteral("-i"), QStringLiteral("pipe:0"),
            QStringLiteral("-an"),
            QStringLiteral("-c:v"), QStringLiteral("libx264"),
            QStringLiteral("-preset"), QStringLiteral("veryfast"),
            QStringLiteral("-crf"), QStringLiteral("18"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-movflags"), QStringLiteral("+faststart"),
            QDir::toNativeSeparators(filename),
        });
        ffmpegProcess_.setProcessChannelMode(QProcess::SeparateChannels);
        ffmpegProcess_.start(QIODevice::WriteOnly);
        if (!ffmpegProcess_.waitForStarted(3000)) {
            QMessageBox::critical(
                this, QStringLiteral("Unable to start FFmpeg"),
                QStringLiteral("Could not start %1:\n%2")
                    .arg(ffmpeg, ffmpegProcess_.errorString()));
            return;
        }

        recordingFile_ = filename;
        recordingBorder_ = addBorder;
        recordedFrames_ = 0;
        recordingDroppedFrames_ = 0;
        recordingWriteFailed_ = false;
        recording_ = true;
        recordingStopping_ = false;
        recordingClock_.restart();
        startRecordingAction_->setEnabled(false);
        stopRecordingAction_->setEnabled(true);
        statusBar()->showMessage(
            QStringLiteral("Recording to %1").arg(QFileInfo(filename).fileName()));
    }

    /** Close FFmpeg input and allow it to finalize the MP4 container. */
    void stopRecording() {
        if (!recording_) {
            return;
        }
        recording_ = false;
        recordingStopping_ = true;
        stopRecordingAction_->setEnabled(false);
        ffmpegProcess_.closeWriteChannel();
        statusBar()->showMessage(QStringLiteral("Finalizing recording…"));
    }

    /** Feed one complete RGB32 VGA frame to FFmpeg without blocking the UI. */
    void recordFrame(const QImage &frame) {
        if (!recording_ ||
            ffmpegProcess_.state() != QProcess::Running) {
            return;
        }

        const qint64 sourceFrameBytes =
            static_cast<qint64>(kSourceWidth) * kVgaHeight * 4;
        if (frame.format() != QImage::Format_RGB32 ||
            frame.width() != kSourceWidth || frame.height() != kVgaHeight ||
            frame.bytesPerLine() != kSourceWidth * 4 ||
            frame.sizeInBytes() != sourceFrameBytes) {
            recordingWriteFailed_ = true;
            stopRecording();
            return;
        }

        const QImage outputFrame =
            recordingBorder_ ? addCaptureBorder(frame) : frame;
        const qint64 frameBytes = outputFrame.sizeInBytes();

        // Bounded buffering keeps a slow encoder from consuming unbounded RAM.
        // Dropping a whole frame is safe because raw-video frame boundaries
        // remain intact; FFmpeg simply produces a shorter constant-rate video.
        if (ffmpegProcess_.bytesToWrite() > frameBytes * 4) {
            ++recordingDroppedFrames_;
            return;
        }
        const qint64 written = ffmpegProcess_.write(
            reinterpret_cast<const char *>(outputFrame.constBits()),
            frameBytes);
        if (written != frameBytes) {
            recordingWriteFailed_ = true;
            stopRecording();
            return;
        }
        ++recordedFrames_;
    }

    /** Handle normal completion and report FFmpeg diagnostics on failure. */
    void handleRecordingFinished(int exitCode,
                                 QProcess::ExitStatus exitStatus) {
        const bool wasRecording = recording_ || recordingStopping_;
        const QString diagnostics = QString::fromLocal8Bit(
            ffmpegProcess_.readAllStandardError()).trimmed();
        const bool succeeded = wasRecording && !recordingWriteFailed_ &&
            exitStatus == QProcess::NormalExit && exitCode == 0;

        recording_ = false;
        recordingStopping_ = false;
        stopRecordingAction_->setEnabled(false);
        startRecordingAction_->setEnabled(
            state_ == State::Streaming && screen_->hasFrame());
        if (!wasRecording || closing_) {
            return;
        }
        if (succeeded) {
            statusBar()->showMessage(
                QStringLiteral("Recording saved: %1")
                    .arg(QDir::toNativeSeparators(recordingFile_)),
                8000);
        } else {
            QMessageBox::critical(
                this, QStringLiteral("Recording failed"),
                diagnostics.isEmpty()
                    ? QStringLiteral("FFmpeg did not complete the recording.")
                    : QStringLiteral("FFmpeg reported:\n%1")
                          .arg(diagnostics));
        }
    }

    /** Toggle the frameless presentation view while retaining stream state. */
    void setPresentationMode(bool enabled) {
        menuBar()->setVisible(!enabled);
        statusBar()->setVisible(!enabled);
        controlsWidget_->setVisible(!enabled);
        statisticsPanel_->setVisible(!enabled);
        if (enabled) {
            showFullScreen();
        } else {
            showNormal();
        }
    }

    /** Start a fresh enumeration pass when the state machine is idle. */
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
        firmwareVersion_.clear();
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

    /** Open and identify the next Pico CDC candidate. */
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

    /** Schedule a bounded-delay discovery retry when auto-reconnect is active. */
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

    /** Leave binary streaming and request the current console settings. */
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

    /** Report a configuration error and return to the selected screen mode. */
    void configurationFailed(const QString &message) {
        state_ = State::Configuring;
        receiveBuffer_.clear();
        QMessageBox::warning(this, QStringLiteral("Adapter configuration"),
                             message);
        resumeScreenMode();
    }

    /** Present the configuration dialog and dispatch its selected result. */
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

    /** Translate a settings delta into console commands and resume streaming. */
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
        if (current.noiseSupported) {
            static const char *const noiseCommands[] = {
                "noise off\r\n",
                "noise low\r\n",
                "noise medium\r\n",
                "noise high\r\n",
            };
            const int level = std::clamp(
                requested.noiseLevel,
                static_cast<int>(P2000M_PHOSPHOR_NOISE_OFF),
                static_cast<int>(P2000M_PHOSPHOR_NOISE_HIGH));
            commands += noiseCommands[level];
        }

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
        commands += screenModeCommand();

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

    /** Re-enter the selected binary screen mode after console interaction. */
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
        if (!serial_.write(screenModeCommand())) {
            handleConnectionLoss();
        }
    }

    /** Return the console command for the currently selected encoding. */
    QByteArray screenModeCommand() const {
        return packBitsRequested_
                   ? QByteArrayLiteral("screen packbits\r\n")
                   : QByteArrayLiteral("screen raw\r\n");
    }

    /** Switch a live stream between raw and opportunistic PackBits records. */
    void setStreamEncoding(bool packBits) {
        if (packBitsRequested_ == packBits) {
            return;
        }
        packBitsRequested_ = packBits;
        updateEncodingGraphVisibility();
        if (state_ != State::Streaming || !serial_.isOpen()) {
            statusBar()->showMessage(
                packBits
                    ? QStringLiteral(
                          "PackBits will be used at the next connection")
                    : QStringLiteral(
                          "Raw streaming will be used at the next connection"));
            return;
        }

        receiveBuffer_.clear();
        state_ = State::SwitchingEncoding;
        stateClock_.restart();
        statusBar()->showMessage(
            packBits ? QStringLiteral("Switching to PackBits streaming…")
                     : QStringLiteral("Switching to raw streaming…"));
        if (!serial_.write(QByteArrayLiteral("console\r\n"))) {
            handleConnectionLoss();
        }
    }

    /** Poll Windows serial input and advance the connection state machine. */
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
        recordReceivedBytes(incoming.size());
        receiveBuffer_.append(incoming);

        if (state_ == State::Probing) {
            if (receiveBuffer_.contains("P2000M VID2VGA firmware")) {
                const QRegularExpression versionPattern(QStringLiteral(
                    "P2000M VID2VGA firmware (v[0-9]+(?:\\.[0-9]+){2})"));
                const QRegularExpressionMatch versionMatch =
                    versionPattern.match(QString::fromLatin1(receiveBuffer_));
                if (versionMatch.hasMatch()) {
                    firmwareVersion_ = versionMatch.captured(1);
                }
                receiveBuffer_.clear();
                state_ = State::AwaitingScreenMode;
                stateClock_.restart();
                serial_.write(screenModeCommand());
            } else if (stateClock_.elapsed() > 3000) {
                tryNextCandidate();
            }
            return;
        }

        if (state_ == State::AwaitingScreenMode) {
            const qsizetype announcementOffset = receiveBuffer_.indexOf(
                QByteArrayLiteral("SCREEN mode=binary version=1"));
            if (announcementOffset >= 0) {
                const qsizetype lineEnd =
                    receiveBuffer_.indexOf('\n', announcementOffset);
                if (lineEnd < 0) {
                    return;
                }
                const QByteArray announcement = receiveBuffer_.mid(
                    announcementOffset, lineEnd - announcementOffset);
                continuousScreenMode_ =
                    announcement.contains("flow=continuous");
                // Continuous firmware may append the first binary record to
                // the same Windows read as its text announcement. Preserve it
                // instead of throwing away the start of that frame.
                receiveBuffer_.remove(0, lineEnd + 1);
                state_ = State::Streaming;
                if (!resumingScreenMode_) {
                    frameCount_ = 0;
                    crcErrors_ = 0;
                    crcErrorTimes_.clear();
                    captureClock_.restart();
                }
                smoothedFps_ = 0.0;
                smoothedPayloadBytes_ = 0.0;
                smoothedRxBytesPerSecond_ = 0.0;
                rxWindowBytes_ = 0;
                rxClock_.restart();
                smoothedRenderMilliseconds_ = 0.0;
                smoothedDecompressionMilliseconds_ = 0.0;
                smoothedSequenceStep_ = 0.0;
                lastSequenceValid_ = false;
                firmwareTimingAvailable_ = false;
                smoothedFirmwarePrepareUs_ = 0.0;
                smoothedFirmwareEncodeUs_ = 0.0;
                frameClock_.invalidate();
                streamFrameClock_.start();
                signalLossDisplayed_ = false;
                screen_->resetAfterglow();
                clearStatisticsGraphs();
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
                processFrames();
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

        if (state_ == State::SwitchingEncoding) {
            if (receiveBuffer_.contains("Command mode.") &&
                receiveBuffer_.contains("vid2vga> ")) {
                receiveBuffer_.clear();
                state_ = State::AwaitingScreenMode;
                resumingScreenMode_ = true;
                stateClock_.restart();
                if (!serial_.write(screenModeCommand())) {
                    handleConnectionLoss();
                }
            } else if (stateClock_.elapsed() > 2000) {
                handleConnectionLoss();
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
            updateSignalPresence();
        }
    }

    /** Replace a frozen source frame after the firmware's watchdog interval. */
    void updateSignalPresence() {
        if (signalLossDisplayed_ || !streamFrameClock_.isValid() ||
            streamFrameClock_.elapsed() <= kSignalLossTimeoutMs) {
            return;
        }

        signalLossDisplayed_ = true;
        screen_->resetAfterglow();
        const QString version = firmwareVersion_.isEmpty()
                                    ? QStringLiteral("v%1").arg(
                                          QStringLiteral(P2000M_VIEWER_VERSION))
                                    : firmwareVersion_;
        screen_->setFrame(p2000m::renderSignalLossScreen(version));
        screenshotAction_->setEnabled(true);
        statusBar()->showMessage(
            QStringLiteral("%1  •  SIGNAL LOST  •  waiting for HSYNC + VSYNC")
                .arg(serial_.name()));
    }

    /** Parse, validate, render, and account for every complete queued record. */
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
            const quint32 timingDiagnostics = loadU32(header + 12);
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

            const bool validPayload =
                (type == 1 && payloadSize == kFramePayloadSize) ||
                (type == 2 && payloadSize > 0 &&
                 payloadSize <= kMaximumPackBitsSize);
            const bool validHeader =
                version == 1 && validPayload && (flags & 0x3u) == 0x3u &&
                width == kSourceWidth && height == kSourceHeight &&
                stride == kSourceWidth / 8 &&
                headerSize == kFrameHeaderSize;
            if (!validHeader) {
                receiveBuffer_.remove(0, 1);
                continue;
            }
            const qsizetype recordSize =
                static_cast<qsizetype>(headerSize) + payloadSize;
            if (receiveBuffer_.size() < recordSize) {
                return;
            }

            const QByteArray encodedPayload = receiveBuffer_.mid(
                headerSize, static_cast<qsizetype>(payloadSize));
            receiveBuffer_.remove(0, recordSize);
            QByteArray payload;
            if (type == 1) {
                payload = encodedPayload;
            } else {
                QElapsedTimer decompressionTimer;
                decompressionTimer.start();
                payload.resize(kFramePayloadSize);
                if (!p2000m_packbits_decode(
                        reinterpret_cast<const uint8_t *>(
                            encodedPayload.constData()),
                        static_cast<size_t>(encodedPayload.size()),
                        reinterpret_cast<uint8_t *>(payload.data()),
                        static_cast<size_t>(payload.size()))) {
                    recordCrcError();
                    statusBar()->showMessage(
                        QStringLiteral(
                            "Discarded frame %1: invalid PackBits data")
                            .arg(sequence));
                    requestFrame();
                    continue;
                }
                const double milliseconds =
                    static_cast<double>(decompressionTimer.nsecsElapsed()) /
                    1000000.0;
                smoothedDecompressionMilliseconds_ =
                    smoothedDecompressionMilliseconds_ == 0.0
                        ? milliseconds
                        : smoothedDecompressionMilliseconds_ * 0.85 +
                              milliseconds * 0.15;
            }
            if (crc32(payload) != expectedCrc) {
                recordCrcError();
                statusBar()->showMessage(
                    QStringLiteral("Discarded frame %1: CRC mismatch")
                        .arg(sequence));
                requestFrame();
                continue;
            }

            streamFrameClock_.restart();
            if (signalLossDisplayed_) {
                signalLossDisplayed_ = false;
                screen_->resetAfterglow();
            }

            // Grant the next frame before doing GUI-side pixel expansion so
            // USB and rendering overlap instead of adding their latencies.
            requestFrame();
            QElapsedTimer renderTimer;
            renderTimer.start();
            QImage rendered = renderFrame(payload, foreground, background,
                                          border, style, sequence);
            const quint32 sourceFrameStep = lastSequenceValid_
                                                ? std::max(
                                                      sequence - lastSequence_,
                                                      1u)
                                                : 1u;
            const double renderMilliseconds =
                static_cast<double>(renderTimer.nsecsElapsed()) / 1000000.0;
            smoothedRenderMilliseconds_ =
                smoothedRenderMilliseconds_ == 0.0
                    ? renderMilliseconds
                    : smoothedRenderMilliseconds_ * 0.85 +
                          renderMilliseconds * 0.15;
            recordFrame(rendered);
            screen_->setFrame(std::move(rendered), sourceFrameStep);
            screenshotAction_->setEnabled(true);
            startRecordingAction_->setEnabled(!recording_ &&
                                               !recordingStopping_);
            ++frameCount_;
            if (lastSequenceValid_) {
                smoothedSequenceStep_ = smoothedSequenceStep_ == 0.0
                                            ? sourceFrameStep
                                            : smoothedSequenceStep_ * 0.85 +
                                                  sourceFrameStep * 0.15;
            }
            lastSequence_ = sequence;
            lastSequenceValid_ = true;
            if ((flags & 0x4u) != 0u) {
                const double prepareUs = timingDiagnostics & 0xffffu;
                const double encodeUs = timingDiagnostics >> 16u;
                smoothedFirmwarePrepareUs_ =
                    smoothedFirmwarePrepareUs_ == 0.0
                        ? prepareUs
                        : smoothedFirmwarePrepareUs_ * 0.85 +
                              prepareUs * 0.15;
                smoothedFirmwareEncodeUs_ =
                    smoothedFirmwareEncodeUs_ == 0.0
                        ? encodeUs
                        : smoothedFirmwareEncodeUs_ * 0.85 +
                              encodeUs * 0.15;
                firmwareTimingAvailable_ = true;
            }
            smoothedPayloadBytes_ = smoothedPayloadBytes_ == 0.0
                                        ? payloadSize
                                        : smoothedPayloadBytes_ * 0.85 +
                                              payloadSize * 0.15;
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
            const double savedPercent = std::max(
                0.0, 100.0 *
                         (1.0 - smoothedPayloadBytes_ / kFramePayloadSize));
            updateStatisticsGraphs(type, savedPercent);
            updateStreamingStatus(sequence);
        }
    }

    /** Clear every graph when beginning a newly negotiated stream. */
    void clearStatisticsGraphs() {
        const std::array<MetricGraphWidget *, 11> graphs = {
            usbGraph_, frameRateGraph_, sourceStepGraph_, payloadGraph_,
            renderGraph_, unpackGraph_, paintRateGraph_, paintTimeGraph_,
            firmwarePrepareGraph_, firmwareEncodeGraph_, crcGraph_,
        };
        for (MetricGraphWidget *graph : graphs) {
            graph->clear();
        }
    }

    /** Show compression metrics only while PackBits mode is selected. */
    void updateEncodingGraphVisibility() {
        payloadGraph_->setVisible(packBitsRequested_);
        unpackGraph_->setVisible(packBitsRequested_);
        firmwareEncodeGraph_->setVisible(packBitsRequested_);
    }

    /** Record one integrity failure for total and rolling-rate metrics. */
    void recordCrcError() {
        ++crcErrors_;
        if (!captureClock_.isValid()) {
            captureClock_.start();
        }
        crcErrorTimes_.push_back(captureClock_.elapsed());
    }

    /** Return the CRC-error rate over at most the latest 60 seconds. */
    double crcErrorsPerMinute() {
        if (!captureClock_.isValid()) {
            return 0.0;
        }
        const qint64 now = captureClock_.elapsed();
        constexpr qint64 windowMilliseconds = 60000;
        while (!crcErrorTimes_.empty() &&
               crcErrorTimes_.front() <= now - windowMilliseconds) {
            crcErrorTimes_.pop_front();
        }
        const qint64 observedMilliseconds =
            std::clamp<qint64>(now, 1000, windowMilliseconds);
        return static_cast<double>(crcErrorTimes_.size()) *
               windowMilliseconds / observedMilliseconds;
    }

    /** Publish all smoothed transport, viewer, and firmware measurements. */
    void updateStatisticsGraphs(quint8 payloadType, double savedPercent) {
        updateEncodingGraphVisibility();
        usbGraph_->setSample(
            smoothedRxBytesPerSecond_ / 1024.0,
            QStringLiteral("%1 KiB/s")
                .arg(smoothedRxBytesPerSecond_ / 1024.0, 0, 'f', 1));
        frameRateGraph_->setSample(
            smoothedFps_,
            QStringLiteral("%1 FPS").arg(smoothedFps_, 0, 'f', 1));
        sourceStepGraph_->setSample(
            smoothedSequenceStep_,
            lastSequenceValid_ && frameCount_ > 1
                ? QStringLiteral("%1 source frames")
                      .arg(smoothedSequenceStep_, 0, 'f', 2)
                : QStringLiteral("Waiting for next frame"),
            lastSequenceValid_ && frameCount_ > 1);
        if (packBitsRequested_) {
            payloadGraph_->setSample(
                smoothedPayloadBytes_ / 1024.0,
                QStringLiteral("%1 KiB · %2% saved")
                    .arg(smoothedPayloadBytes_ / 1024.0, 0, 'f', 1)
                    .arg(savedPercent, 0, 'f', 0));
        }
        renderGraph_->setSample(
            smoothedRenderMilliseconds_,
            QStringLiteral("%1 ms")
                .arg(smoothedRenderMilliseconds_, 0, 'f', 2));
        if (packBitsRequested_) {
            unpackGraph_->setSample(
                payloadType == 2 ? smoothedDecompressionMilliseconds_ : 0.0,
                payloadType == 2
                    ? QStringLiteral("%1 ms")
                          .arg(smoothedDecompressionMilliseconds_, 0, 'f', 2)
                    : QStringLiteral("Raw fallback · not required"));
        }
        paintRateGraph_->setSample(
            screen_->paintFps(),
            QStringLiteral("%1 FPS").arg(screen_->paintFps(), 0, 'f', 1));
        paintTimeGraph_->setSample(
            screen_->paintMilliseconds(),
            QStringLiteral("%1 ms")
                .arg(screen_->paintMilliseconds(), 0, 'f', 2));
        firmwarePrepareGraph_->setSample(
            smoothedFirmwarePrepareUs_ / 1000.0,
            firmwareTimingAvailable_
                ? QStringLiteral("%1 ms")
                      .arg(smoothedFirmwarePrepareUs_ / 1000.0, 0, 'f', 2)
                : QStringLiteral("Not provided by firmware"),
            firmwareTimingAvailable_);
        if (packBitsRequested_) {
            firmwareEncodeGraph_->setSample(
                smoothedFirmwareEncodeUs_ / 1000.0,
                firmwareTimingAvailable_
                    ? QStringLiteral("%1 ms")
                          .arg(smoothedFirmwareEncodeUs_ / 1000.0, 0, 'f', 2)
                    : QStringLiteral("Not provided by firmware"),
                firmwareTimingAvailable_);
        }
        const double errorsPerMinute = crcErrorsPerMinute();
        crcGraph_->setSample(
            errorsPerMinute,
            QStringLiteral("%1 errors/min")
                .arg(errorsPerMinute, 0, 'f', 2));
    }

    /** Show only connection, source-frame, and recording state in the bar. */
    void updateStreamingStatus(quint32 sequence) {
        QString message = QStringLiteral("%1  •  frame %2  •  CRC errors %3")
                              .arg(serial_.name())
                              .arg(sequence)
                              .arg(crcErrors_);
        if (recording_) {
            const qint64 elapsedSeconds = recordingClock_.elapsed() / 1000;
            message += QStringLiteral("  •  recording %1:%2 · %3 frames")
                           .arg(elapsedSeconds / 60, 2, 10,
                                QLatin1Char('0'))
                           .arg(elapsedSeconds % 60, 2, 10,
                                QLatin1Char('0'))
                           .arg(recordedFrames_);
            if (recordingDroppedFrames_ != 0) {
                message += QStringLiteral(" · %1 dropped")
                               .arg(recordingDroppedFrames_);
            }
        } else if (recordingStopping_) {
            message += QStringLiteral("  •  finalizing recording");
        }
        statusBar()->showMessage(message);
    }

    /** Add one Windows read to the smoothed USB-throughput measurement. */
    void recordReceivedBytes(qsizetype count) {
        if (count <= 0) {
            return;
        }
        if (!rxClock_.isValid()) {
            rxClock_.start();
        }
        rxWindowBytes_ += static_cast<quint64>(count);
        const qint64 elapsed = rxClock_.elapsed();
        if (elapsed < 500) {
            return;
        }
        const double instantaneous =
            static_cast<double>(rxWindowBytes_) * 1000.0 / elapsed;
        smoothedRxBytesPerSecond_ = smoothedRxBytesPerSecond_ == 0.0
                                        ? instantaneous
                                        : smoothedRxBytesPerSecond_ * 0.7 +
                                              instantaneous * 0.3;
        rxWindowBytes_ = 0;
        rxClock_.restart();
    }

    /** Reconstruct the exact 640 x 480 VGA view from a packed source frame. */
    QImage renderFrame(const QByteArray &payload, quint32 foreground,
                       quint32 background, quint32 border,
                       quint32 style, quint32 sequence) const {
        const QRgb foregroundRgb = 0xff000000u | (foreground & 0x00ffffffu);
        const QRgb backgroundRgb = 0xff000000u | (background & 0x00ffffffu);
        const QRgb borderRgb = 0xff000000u | (border & 0x00ffffffu);
        const bool borderEnabled = (style & 0x1u) != 0u;
        const bool borderDotted = (style & 0x2u) != 0u;
        const bool stretch = (style & 0x4u) != 0u;
        const uint8_t noiseLevel = (style >> 3u) & 0x3u;
        const uint16_t foregroundRgb444 =
            static_cast<uint16_t>((foreground >> 20u) & 0x0fu) |
            static_cast<uint16_t>(((foreground >> 12u) & 0x0fu) << 4u) |
            static_cast<uint16_t>(((foreground >> 4u) & 0x0fu) << 8u);
        const uint16_t dimmedRgb444 =
            p2000m_phosphor_noise_dim_rgb444(foregroundRgb444);
        const QRgb dimmedForegroundRgb = qRgb(
            (dimmedRgb444 & 0x0fu) * 17u,
            ((dimmedRgb444 >> 4u) & 0x0fu) * 17u,
            ((dimmedRgb444 >> 8u) & 0x0fu) * 17u);

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
            uint32_t noiseState = p2000m_phosphor_noise_seed(
                sequence, static_cast<unsigned>(visibleY));
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

                if (noiseLevel != P2000M_PHOSPHOR_NOISE_OFF &&
                    dimmedRgb444 != foregroundRgb444) {
                    const quint32 selected = word &
                        p2000m_phosphor_noise_mask(noiseLevel, &noiseState);
                    for (unsigned bit = 0u; bit < 32u; ++bit) {
                        if ((selected & (0x80000000u >> bit)) != 0u) {
                            pixels[bit] = dimmedForegroundRgb;
                        }
                    }
                }
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

    /** Grant one frame only when connected to legacy credit-based firmware. */
    void requestFrame() {
        if (state_ == State::Streaming && !continuousScreenMode_ &&
            !serial_.write(QByteArrayLiteral("frame\r\n"))) {
            handleConnectionLoss();
        }
    }

    /** Return the adapter to console mode and close the COM port. */
    void disconnectFromAdapter(bool userRequested) {
        autoReconnect_ = !userRequested;
        streamFrameClock_.invalidate();
        signalLossDisplayed_ = false;
        if (recording_) {
            stopRecording();
        }
        startRecordingAction_->setEnabled(false);
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

    /** Reset connection controls after an I/O failure and schedule discovery. */
    void handleConnectionLoss() {
        streamFrameClock_.invalidate();
        signalLossDisplayed_ = false;
        if (recording_) {
            stopRecording();
        }
        startRecordingAction_->setEnabled(false);
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

    /** Central VGA presentation widget. */
    ScreenWidget *screen_ = nullptr;
    /** Bottom-row controls hidden in full-screen presentation mode. */
    QWidget *controlsWidget_ = nullptr;
    /** Scrollable collection of live performance-history graphs. */
    QWidget *statisticsPanel_ = nullptr;
    /** Windows-side USB receive throughput graph. */
    MetricGraphWidget *usbGraph_ = nullptr;
    /** Validated frame-arrival rate graph. */
    MetricGraphWidget *frameRateGraph_ = nullptr;
    /** Captured source-frame sequence increment graph. */
    MetricGraphWidget *sourceStepGraph_ = nullptr;
    /** Encoded frame payload-size graph. */
    MetricGraphWidget *payloadGraph_ = nullptr;
    /** Framebuffer-to-QImage conversion-duration graph. */
    MetricGraphWidget *renderGraph_ = nullptr;
    /** Optional PackBits expansion-duration graph. */
    MetricGraphWidget *unpackGraph_ = nullptr;
    /** Unique-frame presentation-rate graph. */
    MetricGraphWidget *paintRateGraph_ = nullptr;
    /** Screen-widget painting-duration graph. */
    MetricGraphWidget *paintTimeGraph_ = nullptr;
    /** Firmware frame-preparation-duration graph. */
    MetricGraphWidget *firmwarePrepareGraph_ = nullptr;
    /** Firmware PackBits-duration graph. */
    MetricGraphWidget *firmwareEncodeGraph_ = nullptr;
    /** Accumulated invalid-record count graph. */
    MetricGraphWidget *crcGraph_ = nullptr;
    /** Human-readable connection state. */
    QLabel *connectionLabel_ = nullptr;
    /** Manual discovery button. */
    QPushButton *connectButton_ = nullptr;
    /** User-requested disconnect button. */
    QPushButton *disconnectButton_ = nullptr;
    /** Menu equivalent of connectButton_. */
    QAction *connectAction_ = nullptr;
    /** Menu equivalent of disconnectButton_. */
    QAction *disconnectAction_ = nullptr;
    /** Opens the firmware-backed settings dialog. */
    QAction *configureAction_ = nullptr;
    /** Saves the latest reconstructed frame. */
    QAction *screenshotAction_ = nullptr;
    /** Starts an FFmpeg-backed MP4 recording. */
    QAction *startRecordingAction_ = nullptr;
    /** Stops and finalizes the current recording. */
    QAction *stopRecordingAction_ = nullptr;
    /** Tracks and toggles presentation mode. */
    QAction *fullScreenAction_ = nullptr;
    /** Toggles temporal CRT phosphor persistence. */
    QAction *afterglowEnabledAction_ = nullptr;
    /** Opens the tunable afterglow half-life control. */
    QAction *afterglowDecayAction_ = nullptr;
    /** One-millisecond serial polling timer. */
    QTimer pollTimer_;
    /** Timeout clock for the active protocol state. */
    QElapsedTimer stateClock_;
    /** Interval clock for validated frame arrivals. */
    QElapsedTimer frameClock_;
    /** Time since the latest valid source frame or screen-mode announcement. */
    QElapsedTimer streamFrameClock_;
    /** Accumulation clock for Windows-received byte throughput. */
    QElapsedTimer rxClock_;
    /** Elapsed capture time used for the rolling CRC-error rate. */
    QElapsedTimer captureClock_;
    /** External encoder process receiving raw VGA frames. */
    QProcess ffmpegProcess_;
    /** Elapsed wall clock displayed while recording. */
    QElapsedTimer recordingClock_;
    /** Destination selected for the current or most recent recording. */
    QString recordingFile_;
    /** Native serial-port owner. */
#ifdef _WIN32
    WindowsSerialPort serial_;
#else
    AdapterSerialPort serial_;
#endif
    /** Pico CDC candidates found during the current discovery pass. */
    QStringList candidates_;
    /** Candidates which another application prevented us from opening. */
    QStringList unavailableCandidates_;
    /** Next element of candidates_ to probe. */
    qsizetype candidateIndex_ = 0;
    /** Number of ports successfully opened during the current pass. */
    int probedCandidateCount_ = 0;
    /** Mixed text/binary receive accumulator consumed by the active state. */
    QByteArray receiveBuffer_;
    /** Semantic firmware version parsed during adapter identification. */
    QString firmwareVersion_;
    /** Current connection and protocol phase. */
    State state_ = State::Disconnected;
    /** Whether loss or failed discovery should schedule another pass. */
    bool autoReconnect_ = true;
    /** Prevents more than one queued rediscovery callback. */
    bool rediscoveryScheduled_ = false;
    /** Distinguishes stream resumption from initial connection accounting. */
    bool resumingScreenMode_ = false;
    /** Whether firmware sends continuously without FRAME credits. */
    bool continuousScreenMode_ = false;
    /** User-selected PackBits preference for this and future connections. */
    bool packBitsRequested_ = false;
    /** Whether lastSequence_ contains a validated source sequence. */
    bool lastSequenceValid_ = false;
    /** Whether protocol flag bit 2 supplied firmware timing fields. */
    bool firmwareTimingAvailable_ = false;
    /** Whether the firmware-matching signal-loss card is currently visible. */
    bool signalLossDisplayed_ = false;
    /** Whether new reconstructed frames should be sent to FFmpeg. */
    bool recording_ = false;
    /** Whether the active recording includes a 12 px black margin. */
    bool recordingBorder_ = false;
    /** Whether viewer-side CRT phosphor persistence is enabled. */
    bool afterglowEnabled_ = false;
    /** Persisted brightness half-life for CRT phosphor persistence. */
    int afterglowHalfLifeMs_ = kDefaultAfterglowHalfLifeMs;
    /** Whether FFmpeg is flushing and finalizing its output container. */
    bool recordingStopping_ = false;
    /** Whether frame delivery failed before FFmpeg exited. */
    bool recordingWriteFailed_ = false;
    /** Suppresses recording dialogs while the main window is closing. */
    bool closing_ = false;
    /** Smoothed validated-record arrival rate. */
    double smoothedFps_ = 0.0;
    /** Smoothed transmitted payload bytes per complete frame. */
    double smoothedPayloadBytes_ = 0.0;
    /** Smoothed Windows-received bytes per second. */
    double smoothedRxBytesPerSecond_ = 0.0;
    /** Smoothed packed-frame-to-QImage conversion duration. */
    double smoothedRenderMilliseconds_ = 0.0;
    /** Smoothed PackBits expansion duration. */
    double smoothedDecompressionMilliseconds_ = 0.0;
    /** Smoothed difference between consecutive source sequence numbers. */
    double smoothedSequenceStep_ = 0.0;
    /** Smoothed firmware header-preparation time in microseconds. */
    double smoothedFirmwarePrepareUs_ = 0.0;
    /** Smoothed firmware PackBits encoding time in microseconds. */
    double smoothedFirmwareEncodeUs_ = 0.0;
    /** Bytes accumulated in the current throughput window. */
    quint64 rxWindowBytes_ = 0;
    /** Source sequence of the most recently validated frame. */
    quint32 lastSequence_ = 0;
    /** Number of validated frames received during this connection history. */
    quint64 frameCount_ = 0;
    /** Number of structurally invalid or CRC-mismatched records. */
    quint64 crcErrors_ = 0;
    /** Capture-relative timestamps of CRC errors in the rolling window. */
    std::deque<qint64> crcErrorTimes_;
    /** Complete VGA frames accepted by FFmpeg during this recording. */
    quint64 recordedFrames_ = 0;
    /** Frames skipped to keep the encoder's pending buffer bounded. */
    quint64 recordingDroppedFrames_ = 0;
};

}  // namespace

/** Initialize Qt application metadata and run the viewer event loop. */
int main(int argc, char *argv[]) {
#ifdef _WIN32
    QApplication::setStyle(QStringLiteral("windowsvista"));
#endif
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("P2000M VID2VGA Viewer"));
    QApplication::setApplicationVersion(
        QStringLiteral(P2000M_VIEWER_VERSION));
    QApplication::setOrganizationName(QStringLiteral("P2000M VID2VGA"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(
        ":/icons/p2000m-vid2vga-viewer.png")));

    MainWindow window;
    window.show();
    return application.exec();
}
