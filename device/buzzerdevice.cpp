#include "buzzerdevice.h"

#include "logmanager.h"

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/gpio.h>
#endif

namespace vehicledevice {

namespace {

[[maybe_unused]] const char *BUZZER_MODULE = "buzzer";

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

} // namespace

// activeLow=true (low-level trigger):  active=true  -> 0 (beep)
//                                     active=false -> 1 (silent)
// activeLow=false (high-level trigger): active=true  -> 1 (beep)
//                                      active=false -> 0 (silent)
// Equivalent to XOR of activeLow and active.
int buzzerAlarmToPhysicalLevel(bool activeLow, bool active)
{
    return (activeLow != active) ? 1 : 0;
}

BuzzerDevice::BuzzerDevice(const BuzzerConfig &config)
    : config_(config),
      chipFd_(-1),
      lineHandleFd_(-1)
{
}

BuzzerDevice::~BuzzerDevice()
{
    closeDevice();
}

bool BuzzerDevice::openDevice(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked();

    if (!config_.enabled) {
        return true;
    }

#ifdef __linux__
    if (config_.gpioChip.empty()) {
        setError(errorMessage, "buzzer gpio_chip is empty");
        return false;
    }
    if (config_.gpioLine < 0) {
        setError(errorMessage, "buzzer gpio_line is negative");
        return false;
    }

    chipFd_ = ::open(config_.gpioChip.c_str(), O_RDONLY);
    if (chipFd_ < 0) {
        setError(errorMessage,
                 "buzzer open gpiochip failed: " + config_.gpioChip +
                 " errno=" + std::string(std::strerror(errno)));
        return false;
    }

    struct gpiohandle_request request;
    std::memset(&request, 0, sizeof(request));
    request.lineoffsets[0] = static_cast<__u32>(config_.gpioLine);
    request.lines = 1;
    request.flags = GPIOHANDLE_REQUEST_OUTPUT;

    // Set initial value to silent level (alarm off).
    const int silentLevel = buzzerAlarmToPhysicalLevel(config_.activeLow, false);
    request.default_values[0] = static_cast<__u8>(silentLevel);

    if (::ioctl(chipFd_, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
        setError(errorMessage,
                 "buzzer GPIO_GET_LINEHANDLE_IOCTL failed: errno=" +
                 std::string(std::strerror(errno)));
        ::close(chipFd_);
        chipFd_ = -1;
        return false;
    }

    lineHandleFd_ = request.fd;
    VLOG_INFO(BUZZER_MODULE,
              "buzzer opened: chip=%s line=%d activeLow=%d",
              config_.gpioChip.c_str(), config_.gpioLine,
              static_cast<int>(config_.activeLow));
    return true;
#else
    setError(errorMessage, "buzzer not supported on this platform");
    return false;
#endif
}

void BuzzerDevice::closeDevice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked();
}

void BuzzerDevice::closeDeviceLocked()
{
#ifdef __linux__
    if (lineHandleFd_ >= 0) {
        ::close(lineHandleFd_);
        lineHandleFd_ = -1;
    }
    if (chipFd_ >= 0) {
        ::close(chipFd_);
        chipFd_ = -1;
    }
#else
    lineHandleFd_ = -1;
    chipFd_ = -1;
#endif
}

bool BuzzerDevice::isAvailable() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lineHandleFd_ >= 0;
}

bool BuzzerDevice::setAlarm(bool active, std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (lineHandleFd_ < 0) {
        setError(errorMessage, "buzzer device not open");
        return false;
    }

#ifdef __linux__
    const int level = buzzerAlarmToPhysicalLevel(config_.activeLow, active);

    struct gpiohandle_data data;
    std::memset(&data, 0, sizeof(data));
    data.values[0] = static_cast<__u8>(level);

    if (::ioctl(lineHandleFd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
        setError(errorMessage,
                 "buzzer GPIOHANDLE_SET_LINE_VALUES_IOCTL failed: errno=" +
                 std::string(std::strerror(errno)));
        return false;
    }

    VLOG_DEBUG(BUZZER_MODULE,
               "buzzer setAlarm: active=%d level=%d",
               static_cast<int>(active), level);
    return true;
#else
    (void)active;
    setError(errorMessage, "buzzer not supported on this platform");
    return false;
#endif
}

} // namespace vehicledevice
