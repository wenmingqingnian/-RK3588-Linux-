#include "keyinputdevice.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#define VEHICLE_OPEN _open
#define VEHICLE_READ _read
#define VEHICLE_CLOSE _close
#else
#include <fcntl.h>
#include <unistd.h>
#define VEHICLE_OPEN open
#define VEHICLE_READ read
#define VEHICLE_CLOSE close
#endif

namespace vehicledevice {
namespace {

void setError(std::string *errorMessage, const std::string &message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

std::string errnoMessage(const std::string &prefix)
{
    return prefix + ": " + std::strerror(errno);
}

} // namespace

KeyInputDevice::KeyInputDevice()
    : config_(),
      fd_(-1)
{
}

KeyInputDevice::KeyInputDevice(const KeyInputConfig &config)
    : config_(config),
      fd_(-1)
{
}

KeyInputDevice::~KeyInputDevice()
{
    closeDevice();
}

void KeyInputDevice::configure(const KeyInputConfig &config)
{
    closeDevice();
    config_ = config;
}

bool KeyInputDevice::openDevice(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    closeDevice();
    if (!config_.enabled) {
        return true;
    }

    fd_ = VEHICLE_OPEN(config_.devicePath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        setError(errorMessage, errnoMessage("failed to open key input device " +
                                            config_.devicePath));
        return false;
    }

    return true;
}

int KeyInputDevice::readKeyValue(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    if (!config_.enabled || fd_ < 0) {
        return 0;
    }

    unsigned char value = 0;
    const int ret = static_cast<int>(VEHICLE_READ(fd_, &value, sizeof(value)));
    if (ret == static_cast<int>(sizeof(value))) {
        return static_cast<int>(value);
    }
    if (ret < 0 && errno != EAGAIN
#ifdef EWOULDBLOCK
        && errno != EWOULDBLOCK
#endif
    ) {
        setError(errorMessage, errnoMessage("failed to read key input device " +
                                            config_.devicePath));
    }

    return 0;
}

bool KeyInputDevice::isAvailable() const
{
    return config_.enabled && fd_ >= 0;
}

void KeyInputDevice::closeDevice()
{
    if (fd_ >= 0) {
        VEHICLE_CLOSE(fd_);
        fd_ = -1;
    }
}

} // namespace vehicledevice
