#include "weatherformat.h"

#include <cctype>

namespace vehicleweather {
namespace {

bool isDigitAt(const std::string &value, std::size_t index)
{
    return index < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[index])) != 0;
}

void setFallback(std::string *formattedText)
{
    if (formattedText != nullptr) {
        *formattedText = "N/A";
    }
}

} // namespace

bool formatIsoDateToMonthDay(const std::string &dateText,
                             std::string *formattedText)
{
    if (dateText.size() != 10 ||
        !isDigitAt(dateText, 0) ||
        !isDigitAt(dateText, 1) ||
        !isDigitAt(dateText, 2) ||
        !isDigitAt(dateText, 3) ||
        dateText[4] != '-' ||
        !isDigitAt(dateText, 5) ||
        !isDigitAt(dateText, 6) ||
        dateText[7] != '-' ||
        !isDigitAt(dateText, 8) ||
        !isDigitAt(dateText, 9)) {
        setFallback(formattedText);
        return false;
    }

    if (formattedText != nullptr) {
        *formattedText = dateText.substr(5, 2) + "-" + dateText.substr(8, 2);
    }
    return true;
}

} // namespace vehicleweather
