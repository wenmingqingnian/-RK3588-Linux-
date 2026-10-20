#ifndef STATICMAPRESPONSE_H
#define STATICMAPRESPONSE_H

#include <cstddef>
#include <string>

namespace vehiclemap {

bool isSupportedStaticMapImageResponse(const std::string &contentType,
                                       const std::string &data);
std::string makeStaticMapResponsePreview(const std::string &data,
                                         std::size_t maxLength);

} // namespace vehiclemap

#endif // STATICMAPRESPONSE_H
