#include "ap3216device.h"

#include "ap3216_iiobackend.h"
#include "ap3216_useri2cbackend.h"

namespace vehicledevice {

std::unique_ptr<IAp3216Backend>
createAp3216Backend(const Ap3216Config &config)
{
    if (config.backend == "user_i2c") {
        return std::make_unique<Ap3216UserI2cBackend>(config);
    }

    return std::make_unique<Ap3216IioBackend>(config);
}

} // namespace vehicledevice
