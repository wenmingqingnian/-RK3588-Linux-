#include "gpsdevice.h"

#include "logmanager.h"

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace vehicledevice {

namespace {

[[maybe_unused]] const char *GPS_MODULE = "gps";

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

#ifdef __linux__

std::string errnoMessage(const std::string &prefix)
{
    return prefix + " (" + std::string(std::strerror(errno)) + ")";
}

// Map the config baudrate integer to a termios speed_t constant. Returns
// B0 (0) when the value is not in the allowed list — the caller rejects this.
speed_t baudrateToSpeed(int baudrate)
{
    switch (baudrate) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 500000: return B500000;
    default:     return B0;
    }
}

// Apply termios configuration for 8N1 (or configured data bits / parity /
// stop bits) at the configured baud rate. Raw mode, no flow control, no echo,
// no special character processing. Returns false on tcgetattr/tcsetattr failure
// or unsupported baudrate.
bool configureTermios(int fd, const GpsSerialConfig &config, std::string *errorMessage)
{
    const speed_t speed = baudrateToSpeed(config.baudrate);
    if (speed == B0) {
        setError(errorMessage,
                 "gps baudrate not supported by termios: " +
                     std::to_string(config.baudrate));
        return false;
    }

    struct termios tty;
    std::memset(&tty, 0, sizeof(tty));
    if (::tcgetattr(fd, &tty) != 0) {
        setError(errorMessage, errnoMessage("gps tcgetattr failed"));
        return false;
    }

    ::cfsetispeed(&tty, speed);
    ::cfsetospeed(&tty, speed);

    // Control mode flags: enable receiver, ignore modem control lines.
    tty.c_cflag |= (CLOCAL | CREAD);

    // Data bits (CS5/CS6/CS7/CS8).
    tty.c_cflag &= ~CSIZE;
    switch (config.dataBits) {
    case 5: tty.c_cflag |= CS5; break;
    case 6: tty.c_cflag |= CS6; break;
    case 7: tty.c_cflag |= CS7; break;
    case 8: tty.c_cflag |= CS8; break;
    default:
        setError(errorMessage,
                 "gps data_bits unsupported: " + std::to_string(config.dataBits));
        return false;
    }

    // Parity.
    switch (config.parity) {
    case 'N':
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~PARODD;
        tty.c_iflag &= ~INPCK;
        break;
    case 'O':
        tty.c_cflag |= PARENB;
        tty.c_cflag |= PARODD;
        tty.c_iflag |= INPCK;
        break;
    case 'E':
        tty.c_cflag |= PARENB;
        tty.c_cflag &= ~PARODD;
        tty.c_iflag |= INPCK;
        break;
    default:
        setError(errorMessage,
                 std::string("gps parity unsupported: ") +
                     std::string(1, config.parity));
        return false;
    }

    // Stop bits.
    switch (config.stopBits) {
    case 1: tty.c_cflag &= ~CSTOPB; break;
    case 2: tty.c_cflag |= CSTOPB;  break;
    default:
        setError(errorMessage,
                 "gps stop_bits unsupported: " + std::to_string(config.stopBits));
        return false;
    }

    // Disable hardware flow control (CRTSCTS may not exist on all platforms).
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif

    // Local mode flags: raw input, no echo, no canonical processing.
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    // Input mode flags: disable software flow control and special input
    // handling. Raw bytes from the GPS module pass through unchanged.
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    // Output mode flags: raw output, no post-processing.
    tty.c_oflag &= ~OPOST;

    // Control characters: VMIN=0 VTIME=0 so read() returns immediately with
    // whatever is available (poll() handles the wait). The device layer never
    // blocks in read() — it polls first, then reads.
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
        setError(errorMessage, errnoMessage("gps tcsetattr failed"));
        return false;
    }

    // Flush any stale data in the input buffer after configuration.
    ::tcflush(fd, TCIFLUSH);
    return true;
}

#endif // __linux__

} // namespace

GpsDevice::GpsDevice(const GpsSerialConfig &config)
    : config_(config),
      fd_(-1)
{
}

GpsDevice::~GpsDevice()
{
    closeDevice();
}

bool GpsDevice::openDevice(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked();

    // When GPS is disabled in config, openDevice succeeds as a no-op.
    // isAvailable() returns false because fd_ stays -1.
    if (!config_.enabled) {
        return true;
    }

    if (config_.devicePath.empty()) {
        setError(errorMessage, "gps device path is empty");
        return false;
    }

#ifdef __linux__
    // O_RDWR: GPS modules are receive-only in practice but some send config
    // commands. O_NOCTTY: do not make this port the controlling terminal.
    // O_NONBLOCK: open() never blocks on DCD. O_CLOEXEC: fd closes on exec.
    fd_ = ::open(config_.devicePath.c_str(),
                 O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        setError(errorMessage,
                 errnoMessage("gps open failed: " + config_.devicePath));
        return false;
    }

    std::string termiosError;
    if (!configureTermios(fd_, config_, &termiosError)) {
        setError(errorMessage, termiosError);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Clear O_NONBLOCK after configuration: we rely on poll() for waiting,
    // and read() should return EAGAIN only when poll() said no data.
    const int fdFlags = ::fcntl(fd_, F_GETFL, 0);
    if (fdFlags >= 0) {
        ::fcntl(fd_, F_SETFL, fdFlags & ~O_NONBLOCK);
    }

    VLOG_INFO(GPS_MODULE,
              "gps opened: device=%s baud=%d data=%d parity=%c stop=%d timeout=%dms",
              config_.devicePath.c_str(), config_.baudrate, config_.dataBits,
              config_.parity, config_.stopBits, config_.readTimeoutMs);
    return true;
#else
    setError(errorMessage, "gps serial not supported on this platform");
    return false;
#endif
}

void GpsDevice::closeDevice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked();
}

void GpsDevice::closeDeviceLocked()
{
#ifdef __linux__
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        VLOG_DEBUG(GPS_MODULE, "gps device closed");
    }
#else
    fd_ = -1;
#endif
}

bool GpsDevice::isAvailable() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_ >= 0;
}

int GpsDevice::readChunk(char *buffer, std::size_t bufferSize,
                         std::string *outError)
{
    if (outError != nullptr) {
        outError->clear();
    }

    if (buffer == nullptr || bufferSize == 0) {
        setError(outError, "gps read: invalid buffer");
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (fd_ < 0) {
        setError(outError, "gps read: device not open");
        return -1;
    }

#ifdef __linux__
    struct pollfd event;
    event.fd = fd_;
    event.events = POLLIN;
    event.revents = 0;

    int ret = 0;
    do {
        ret = ::poll(&event, 1, config_.readTimeoutMs);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        setError(outError, errnoMessage("gps poll failed"));
        return -1;
    }

    if (ret == 0) {
        // Timeout, no data. Not an error.
        return 0;
    }

    if ((event.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        setError(outError, "gps poll: device error/hangup");
        return -1;
    }

    if ((event.revents & POLLIN) == 0) {
        // poll returned >0 but no POLLIN — unexpected, treat as timeout.
        return 0;
    }

    const ssize_t bytesRead = ::read(fd_, buffer, bufferSize);
    if (bytesRead < 0) {
        if (errno == EAGAIN
#ifdef EWOULDBLOCK
            || errno == EWOULDBLOCK
#endif
        ) {
            // Spurious wakeup: poll said readable but read says EAGAIN.
            // Not an error, return 0 bytes.
            return 0;
        }
        setError(outError, errnoMessage("gps read failed"));
        return -1;
    }

    return static_cast<int>(bytesRead);
#else
    setError(outError, "gps serial not supported on this platform");
    return -1;
#endif
}

} // namespace vehicledevice
