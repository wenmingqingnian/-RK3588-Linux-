#ifndef WEATHERFORMAT_H
#define WEATHERFORMAT_H

#include <string>

namespace vehicleweather {

bool formatIsoDateToMonthDay(const std::string &dateText,
                             std::string *formattedText);

} // namespace vehicleweather

#endif // WEATHERFORMAT_H
