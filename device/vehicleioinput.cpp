#include "vehicleioinput.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QTimer>

#include <array>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr int kScanIntervalMs = 2000;
constexpr const char kInputDeviceName[] = "quarkpi-vehicle-io";

bool keyBitIsSet(const unsigned long *bits, unsigned int code)
{
    constexpr unsigned int bitsPerLong = sizeof(unsigned long) * 8U;

    const unsigned int word = code / bitsPerLong;
    const unsigned int bit = code % bitsPerLong;

    return (bits[word] & (1UL << bit)) != 0;
}

} // namespace

VehicleIoInput::VehicleIoInput(QObject *parent)
    : QObject(parent)
    , m_scanTimer(new QTimer(this))
{
    m_scanTimer->setInterval(kScanIntervalMs);

    connect(m_scanTimer,
            &QTimer::timeout,
            this,
            &VehicleIoInput::tryAutoDetect);
}

VehicleIoInput::~VehicleIoInput()
{
    stop();
}

bool VehicleIoInput::start(const QString &devicePath)
{
    stop();

    m_autoScan = devicePath.isEmpty();

    if (!devicePath.isEmpty())
        return openDevice(devicePath);

    tryAutoDetect();

    if (m_fd < 0)
        m_scanTimer->start();

    return m_fd >= 0;
}

void VehicleIoInput::stop()
{
    m_autoScan = false;
    m_scanTimer->stop();
    closeDevice(false);
}

QString VehicleIoInput::devicePath() const
{
    return m_devicePath;
}

bool VehicleIoInput::isConnected() const
{
    return m_fd >= 0;
}

bool VehicleIoInput::reverseActive() const
{
    return m_reverseActive;
}

bool VehicleIoInput::accActive() const
{
    return m_accActive;
}

bool VehicleIoInput::buttonActive() const
{
    return m_buttonActive;
}

void VehicleIoInput::tryAutoDetect()
{
    if (!m_autoScan || m_fd >= 0)
        return;

    const QString path = findVehicleInputDevice();

    if (path.isEmpty())
        return;

    if (openDevice(path))
        m_scanTimer->stop();
}

QString VehicleIoInput::findVehicleInputDevice() const
{
    const QDir inputClassDir(QStringLiteral("/sys/class/input"));

    const QStringList eventEntries = inputClassDir.entryList(
        QStringList() << QStringLiteral("event*"),
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name);

    for (const QString &entry : eventEntries) {
        QFile nameFile(inputClassDir.filePath(
            entry + QStringLiteral("/device/name")));

        if (!nameFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        const QString name =
            QString::fromUtf8(nameFile.readAll()).trimmed();

        if (name != QLatin1String(kInputDeviceName))
            continue;

        const QString devicePath =
            QStringLiteral("/dev/input/") + entry;

        if (QFileInfo::exists(devicePath))
            return devicePath;
    }

    return QString();
}

bool VehicleIoInput::openDevice(const QString &devicePath)
{
    const QByteArray pathBytes = QFile::encodeName(devicePath);

    const int fd = ::open(pathBytes.constData(),
                          O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0) {
        emit errorOccurred(
            tr("无法打开 %1：%2")
                .arg(devicePath,
                     QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }

    m_fd = fd;
    m_devicePath = devicePath;
    m_notifier = new QSocketNotifier(
        m_fd,
        QSocketNotifier::Read,
        this);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_notifier,
            &QSocketNotifier::activated,
            this,
            [this](QSocketDescriptor,
                   QSocketNotifier::Type) {
                readAvailableEvents();
            });
#else
    connect(m_notifier,
            SIGNAL(activated(int)),
            this,
            SLOT(readAvailableEvents()));
#endif

    readInitialKeyState();
    emit deviceConnected(m_devicePath);

    return true;
}

void VehicleIoInput::closeDevice(bool notifyDisconnect)
{
    const QString oldPath = m_devicePath;

    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    m_devicePath.clear();

    setReverseActive(false);
    setAccActive(false);
    setButtonActive(false);

    if (notifyDisconnect && !oldPath.isEmpty())
        emit deviceDisconnected(oldPath);

    if (notifyDisconnect && m_autoScan)
        m_scanTimer->start();
}

void VehicleIoInput::readInitialKeyState()
{
    constexpr unsigned int bitsPerLong =
        sizeof(unsigned long) * 8U;
    constexpr unsigned int wordCount =
        (KEY_MAX / bitsPerLong) + 1U;

    std::array<unsigned long, wordCount> keyBits {};

    if (::ioctl(m_fd,
                EVIOCGKEY(sizeof(keyBits)),
                keyBits.data()) < 0) {
        return;
    }

    setReverseActive(
        keyBitIsSet(keyBits.data(), KEY_PROG2));
    setAccActive(
        keyBitIsSet(keyBits.data(), KEY_POWER2));
    setButtonActive(
        keyBitIsSet(keyBits.data(), KEY_PROG1));
}

void VehicleIoInput::readAvailableEvents()
{
    if (m_fd < 0)
        return;

    while (true) {
        std::array<input_event, 16> events {};

        const ssize_t bytesRead = ::read(
            m_fd,
            events.data(),
            sizeof(events));

        if (bytesRead > 0) {
            const std::size_t eventCount =
                static_cast<std::size_t>(bytesRead) /
                sizeof(input_event);

            for (std::size_t i = 0; i < eventCount; ++i) {
                const input_event &event = events[i];

                if (event.type != EV_KEY)
                    continue;

                handleKeyEvent(event.code, event.value);
            }

            continue;
        }

        if (bytesRead == 0) {
            closeDevice(true);
            return;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        const QString message =
            tr("读取 %1 失败：%2")
                .arg(m_devicePath,
                     QString::fromLocal8Bit(std::strerror(errno)));

        emit errorOccurred(message);
        closeDevice(true);
        return;
    }
}

void VehicleIoInput::handleKeyEvent(unsigned short code, int value)
{
    if (value != 0 && value != 1)
        return;

    const bool active = value == 1;

    switch (code) {
    case KEY_PROG2:
        setReverseActive(active);
        break;

    case KEY_POWER2:
        setAccActive(active);
        break;

    case KEY_PROG1:
        setButtonActive(active);
        break;

    default:
        break;
    }
}

void VehicleIoInput::setReverseActive(bool active)
{
    if (m_reverseActive == active)
        return;

    m_reverseActive = active;
    emit reverseChanged(active);
}

void VehicleIoInput::setAccActive(bool active)
{
    if (m_accActive == active)
        return;

    m_accActive = active;
    emit accChanged(active);
}

void VehicleIoInput::setButtonActive(bool active)
{
    if (m_buttonActive == active)
        return;

    m_buttonActive = active;
    emit buttonChanged(active);
}
