#include "ap3216_iiobackend.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/iio/events.h>
#include <linux/iio/types.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace vehicledevice {
namespace {

const char *const kIioDevicesDirectory = "/sys/bus/iio/devices";
const char *const kExpectedDeviceName = "ap3216c";
const char *const kNameFile = "name";
const char *const kFallingEnableFile = "events/in_proximity_thresh_falling_en";
const char *const kRisingEnableFile = "events/in_proximity_thresh_rising_en";
const char *const kFallingValueFile = "events/in_proximity_thresh_falling_value";
const char *const kRisingValueFile = "events/in_proximity_thresh_rising_value";

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

std::string trim(const std::string &value)
{
    const std::string whitespace = " \t\r\n";
    const std::string::size_type first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return std::string();
    }
    const std::string::size_type last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::string joinPath(const std::string &directory, const std::string &name)
{
    if (directory.empty()) {
        return name;
    }
    if (directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

bool readTextFile(const std::string &path,
                  std::string *value,
                  std::string *errorMessage)
{
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        setError(errorMessage, "ap3216 iio open failed: " + path);
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        setError(errorMessage, "ap3216 iio read failed: " + path);
        return false;
    }

    *value = trim(buffer.str());
    return true;
}

bool readIntFile(const std::string &path,
                 int *value,
                 std::string *errorMessage)
{
    std::string text;
    if (!readTextFile(path, &text, errorMessage)) {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        setError(errorMessage,
                 "ap3216 iio invalid integer in " + path + ": " + text);
        return false;
    }

    *value = static_cast<int>(parsed);
    return true;
}

bool writeTextFile(const std::string &path,
                   const std::string &value,
                   std::string *errorMessage)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary);
    if (!output) {
        setError(errorMessage, "ap3216 iio open-for-write failed: " + path);
        return false;
    }

    output << value;
    output.flush();
    if (!output) {
        setError(errorMessage, "ap3216 iio write failed: " + path);
        return false;
    }

    return true;
}

bool isIioDeviceEntry(const char *name)
{
    return name != nullptr && std::strncmp(name, "iio:device", 10) == 0;
}

std::string baseName(std::string path)
{
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    const std::string::size_type separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

} // namespace

Ap3216IioBackend::Ap3216IioBackend(const Ap3216Config &config)
    : config_(config),
      eventFd_(-1),
      open_(false),
      eventsEnabled_(false)
{
}

Ap3216IioBackend::~Ap3216IioBackend()
{
    close();
}

bool Ap3216IioBackend::validateSysfsPath(
    const std::string &path,
    std::string *errorMessage) const
{
    std::string name;
    if (!readTextFile(joinPath(path, kNameFile), &name, errorMessage)) {
        return false;
    }
    if (name != kExpectedDeviceName) {
        setError(errorMessage,
                 "ap3216 iio name mismatch at " + path + ": " + name);
        return false;
    }

    const std::string channelFiles[] = {
        config_.alsFile,
        config_.psFile,
        config_.irFile
    };
    for (const std::string &file : channelFiles) {
        const std::string channelPath = joinPath(path, file);
        if (::access(channelPath.c_str(), R_OK) != 0) {
            setError(errorMessage,
                     "ap3216 IIO channel is not readable: " + channelPath +
                         ": " + std::strerror(errno));
            return false;
        }
    }

    return true;
}

bool Ap3216IioBackend::resolveSysfsPath(std::string *errorMessage)
{
    if (!config_.sysfsDir.empty()) {
        if (!validateSysfsPath(config_.sysfsDir, errorMessage)) {
            return false;
        }
        sysfsPath_ = config_.sysfsDir;
        return true;
    }

    DIR *directory = ::opendir(kIioDevicesDirectory);
    if (directory == nullptr) {
        setError(errorMessage,
                 std::string("ap3216 cannot open ") + kIioDevicesDirectory +
                     ": " + std::strerror(errno));
        return false;
    }

    std::vector<std::string> candidates;
    for (dirent *entry = ::readdir(directory);
         entry != nullptr;
         entry = ::readdir(directory)) {
        if (isIioDeviceEntry(entry->d_name)) {
            candidates.emplace_back(entry->d_name);
        }
    }
    ::closedir(directory);
    std::sort(candidates.begin(), candidates.end());

    std::string lastError;
    for (const std::string &candidate : candidates) {
        const std::string path = joinPath(kIioDevicesDirectory, candidate);
        std::string name;
        if (!readTextFile(joinPath(path, kNameFile), &name, &lastError)) {
            continue;
        }
        if (name != kExpectedDeviceName) {
            continue;
        }
        if (!validateSysfsPath(path, &lastError)) {
            setError(errorMessage, lastError);
            return false;
        }
        sysfsPath_ = path;
        return true;
    }

    setError(errorMessage,
             std::string("ap3216 iio device name=") +
                 kExpectedDeviceName + " not found");
    return false;
}

bool Ap3216IioBackend::enableEvents(std::string *errorMessage)
{
    const std::string deviceName = baseName(sysfsPath_);
    eventDevicePath_ = joinPath("/dev", deviceName);

    const int deviceFd = ::open(eventDevicePath_.c_str(),
                                O_RDONLY | O_CLOEXEC);
    if (deviceFd < 0) {
        setError(errorMessage,
                 "ap3216 cannot open event device " + eventDevicePath_ +
                     ": " + std::strerror(errno));
        eventDevicePath_.clear();
        return false;
    }

    int eventFd = -1;
    if (::ioctl(deviceFd, IIO_GET_EVENT_FD_IOCTL, &eventFd) < 0 ||
        eventFd < 0) {
        const int savedErrno = errno;
        ::close(deviceFd);
        setError(errorMessage,
                 "ap3216 cannot obtain IIO event fd from " +
                     eventDevicePath_ + ": " + std::strerror(savedErrno));
        eventDevicePath_.clear();
        return false;
    }

    if (::close(deviceFd) < 0) {
        const int savedErrno = errno;
        ::close(eventFd);
        setError(errorMessage,
                 "ap3216 cannot close IIO character device " +
                     eventDevicePath_ + ": " + std::strerror(savedErrno));
        eventDevicePath_.clear();
        return false;
    }

    const int statusFlags = ::fcntl(eventFd, F_GETFL, 0);
    if (statusFlags < 0 ||
        ::fcntl(eventFd, F_SETFL, statusFlags | O_NONBLOCK) < 0) {
        const int savedErrno = errno;
        ::close(eventFd);
        setError(errorMessage,
                 "ap3216 cannot configure nonblocking IIO event fd: " +
                     std::string(std::strerror(savedErrno)));
        eventDevicePath_.clear();
        return false;
    }

    const int descriptorFlags = ::fcntl(eventFd, F_GETFD, 0);
    if (descriptorFlags < 0 ||
        ::fcntl(eventFd, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
        const int savedErrno = errno;
        ::close(eventFd);
        setError(errorMessage,
                 "ap3216 cannot configure close-on-exec for IIO event fd: " +
                     std::string(std::strerror(savedErrno)));
        eventDevicePath_.clear();
        return false;
    }

    eventFd_ = eventFd;

    const std::string fallingValue =
        std::to_string(config_.psFallingThreshold);
    const std::string risingValue =
        std::to_string(config_.psRisingThreshold);

    if (!writeTextFile(joinPath(sysfsPath_, kFallingValueFile),
                       fallingValue, errorMessage) ||
        !writeTextFile(joinPath(sysfsPath_, kRisingValueFile),
                       risingValue, errorMessage) ||
        !writeTextFile(joinPath(sysfsPath_, kFallingEnableFile),
                       "1", errorMessage) ||
        !writeTextFile(joinPath(sysfsPath_, kRisingEnableFile),
                       "1", errorMessage)) {
        disableEvents();
        return false;
    }

    eventsEnabled_ = true;
    return true;
}

void Ap3216IioBackend::disableEvents()
{
    if (!sysfsPath_.empty()) {
        std::string ignored;
        writeTextFile(joinPath(sysfsPath_, kRisingEnableFile), "0", &ignored);
        writeTextFile(joinPath(sysfsPath_, kFallingEnableFile), "0", &ignored);
    }

    eventsEnabled_ = false;
    if (eventFd_ >= 0) {
        ::close(eventFd_);
        eventFd_ = -1;
    }
    eventDevicePath_.clear();
}

bool Ap3216IioBackend::open(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    close();

    if (!resolveSysfsPath(errorMessage)) {
        return false;
    }

    if (config_.useIioEvents && !enableEvents(errorMessage)) {
        close();
        return false;
    }

    open_ = true;
    return true;
}

void Ap3216IioBackend::close()
{
    disableEvents();
    open_ = false;
    sysfsPath_.clear();
}

bool Ap3216IioBackend::isOpen() const
{
    return open_;
}

Ap3216Reading Ap3216IioBackend::readSensorValues(
    std::string *errorMessage)
{
    Ap3216Reading reading;
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    if (!open_) {
        reading.errorMessage = "ap3216 iio backend is not open";
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    if (!readIntFile(joinPath(sysfsPath_, config_.alsFile),
                     &reading.als, &reading.errorMessage) ||
        !readIntFile(joinPath(sysfsPath_, config_.psFile),
                     &reading.ps, &reading.errorMessage) ||
        !readIntFile(joinPath(sysfsPath_, config_.irFile),
                     &reading.ir, &reading.errorMessage)) {
        setError(errorMessage, reading.errorMessage);
        return reading;
    }

    reading.valid = true;
    return reading;
}

const char *Ap3216IioBackend::backendName() const
{
    return "iio";
}

int Ap3216IioBackend::eventFileDescriptor() const
{
    return eventFd_;
}

int Ap3216IioBackend::readEvent(Ap3216Event *event,
                                std::string *errorMessage)
{
    if (event == nullptr) {
        setError(errorMessage, "ap3216 event output is null");
        return -1;
    }
    *event = Ap3216Event();

    if (eventFd_ < 0) {
        setError(errorMessage, "ap3216 IIO event fd is not open");
        return -1;
    }

    iio_event_data rawEvent{};
    const ssize_t bytes = ::read(eventFd_, &rawEvent, sizeof(rawEvent));
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        setError(errorMessage,
                 "ap3216 IIO event read failed: " +
                     std::string(std::strerror(errno)));
        return -1;
    }
    if (bytes != static_cast<ssize_t>(sizeof(rawEvent))) {
        setError(errorMessage, "ap3216 IIO event returned a short record");
        return -1;
    }

    if (IIO_EVENT_CODE_EXTRACT_CHAN_TYPE(rawEvent.id) != IIO_PROXIMITY ||
        IIO_EVENT_CODE_EXTRACT_TYPE(rawEvent.id) != IIO_EV_TYPE_THRESH) {
        setError(errorMessage, "ap3216 received an unexpected IIO event code");
        return -1;
    }

    const unsigned int direction = IIO_EVENT_CODE_EXTRACT_DIR(rawEvent.id);
    if (direction == IIO_EV_DIR_RISING) {
        event->direction = Ap3216EventDirection::Rising;
    } else if (direction == IIO_EV_DIR_FALLING) {
        event->direction = Ap3216EventDirection::Falling;
    } else {
        setError(errorMessage, "ap3216 received an unsupported event direction");
        return -1;
    }

    event->timestampNs = rawEvent.timestamp;
    event->valid = true;
    return 1;
}

} // namespace vehicledevice
