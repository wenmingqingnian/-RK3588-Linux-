#include "staticmapresponse.h"

#include <algorithm>
#include <cctype>

namespace vehiclemap {
namespace {

std::string trim(const std::string &value)
{
    std::string::size_type first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }

    std::string::size_type last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

std::string normalizeContentType(const std::string &contentType)
{
    std::string normalized = contentType;
    const std::string::size_type semicolon = normalized.find(';');
    if (semicolon != std::string::npos) {
        normalized = normalized.substr(0, semicolon);
    }

    normalized = trim(normalized);
    std::transform(normalized.begin(),
                   normalized.end(),
                   normalized.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return normalized;
}

bool startsWithBytes(const std::string &data,
                     const unsigned char *magic,
                     std::size_t magicSize)
{
    if (data.size() < magicSize) {
        return false;
    }

    for (std::size_t index = 0; index < magicSize; ++index) {
        if (static_cast<unsigned char>(data[index]) != magic[index]) {
            return false;
        }
    }
    return true;
}

bool hasPngMagic(const std::string &data)
{
    static const unsigned char pngMagic[] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };
    return startsWithBytes(data, pngMagic, sizeof(pngMagic));
}

bool hasJpegMagic(const std::string &data)
{
    static const unsigned char jpegMagic[] = {
        0xff, 0xd8, 0xff
    };
    return startsWithBytes(data, jpegMagic, sizeof(jpegMagic));
}

} // namespace

bool isSupportedStaticMapImageResponse(const std::string &contentType,
                                       const std::string &data)
{
    const std::string normalizedContentType = normalizeContentType(contentType);
    if (normalizedContentType == "image/png") {
        return hasPngMagic(data);
    }
    if (normalizedContentType == "image/jpeg" ||
        normalizedContentType == "image/jpg") {
        return hasJpegMagic(data);
    }
    return false;
}

std::string makeStaticMapResponsePreview(const std::string &data,
                                         std::size_t maxLength)
{
    std::string preview;
    preview.reserve(std::min(maxLength, data.size()));

    for (std::size_t index = 0; index < data.size() && preview.size() < maxLength; ++index) {
        const unsigned char value = static_cast<unsigned char>(data[index]);
        if (std::isprint(value) != 0 || value == '\n' || value == '\r' || value == '\t') {
            preview.push_back(static_cast<char>(value));
        } else {
            preview.push_back('.');
        }
    }

    return preview;
}

} // namespace vehiclemap
