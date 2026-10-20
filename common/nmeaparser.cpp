#include "nmeaparser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace vehiclegps {
namespace {

// Knots-to-km/h conversion factor (1 knot = 1.852 km/h).
constexpr double kKnotsToKmh = 1.852;

// Minutes-to-degrees conversion factor (1 minute = 1/60 degree).
constexpr double kMinutesPerDegree = 60.0;

// Parse a string to double with full-string validation. Rejects NaN/Inf and
// trailing garbage. Does not throw.
bool parseDouble(const std::string &text, double *out)
{
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t pos = 0;
        const double value = std::stod(text, &pos);
        if (pos != text.size()) {
            return false;
        }
        if (std::isnan(value) || std::isinf(value)) {
            return false;
        }
        *out = value;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

// Parse a string to int with full-string validation. Does not throw.
bool parseInt(const std::string &text, int *out)
{
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t pos = 0;
        const int value = std::stoi(text, &pos);
        if (pos != text.size()) {
            return false;
        }
        *out = value;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

// Split a string by ',' preserving empty fields.
std::vector<std::string> splitByComma(const std::string &text)
{
    std::vector<std::string> result;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ',') {
            result.push_back(current);
            current.clear();
        } else {
            current.push_back(text[i]);
        }
    }
    result.push_back(current);
    return result;
}

// Strip trailing CR/LF characters from a sentence.
void stripTrailingNewlines(std::string *s)
{
    while (!s->empty() && (s->back() == '\r' || s->back() == '\n')) {
        s->pop_back();
    }
}

// Parse RMC sentence fields. Returns true on successful parse (including
// status V), false on parse error. On parse error, outFix is unchanged.
bool parseRmcFields(const std::vector<std::string> &fields, GpsFix *outFix)
{
    // Required: type(0), time(1), status(2), lat(3), N/S(4), lon(5), E/W(6),
    // speed(7), course(8), date(9).
    if (fields.size() < 10) {
        return false;
    }

    const std::string &status = fields[2];
    if (status.size() != 1) {
        return false;
    }
    const char statusChar = static_cast<char>(
        std::toupper(static_cast<unsigned char>(status[0])));
    if (statusChar != 'A' && statusChar != 'V') {
        return false;
    }

    // Status V: no valid fix. Mark invalid and return true (parsed OK).
    if (statusChar == 'V') {
        outFix->valid = false;
        return true;
    }

    // Status A: parse all required fields.
    if (fields[4].size() != 1 || fields[6].size() != 1) {
        return false;
    }
    const char latHemi = static_cast<char>(
        std::toupper(static_cast<unsigned char>(fields[4][0])));
    const char lonHemi = static_cast<char>(
        std::toupper(static_cast<unsigned char>(fields[6][0])));

    double latitude = 0.0;
    double longitude = 0.0;
    if (!convertLatitude(fields[3], latHemi, &latitude)) {
        return false;
    }
    if (!convertLongitude(fields[5], lonHemi, &longitude)) {
        return false;
    }

    double speedKnots = 0.0;
    if (!parseDouble(fields[7], &speedKnots)) {
        return false;
    }
    if (speedKnots < 0.0) {
        return false;
    }

    double course = 0.0;
    if (!parseDouble(fields[8], &course)) {
        return false;
    }
    if (course < 0.0 || course > 360.0) {
        return false;
    }

    outFix->valid = true;
    outFix->latitude = latitude;
    outFix->longitude = longitude;
    outFix->speedKmh = speedKnots * kKnotsToKmh;
    outFix->courseDegrees = course;
    outFix->utcTime = fields[1];
    outFix->utcDate = fields[9];
    return true;
}

// Parse GGA sentence fields. Returns true on successful parse (including
// fixQuality 0), false on parse error. On parse error, outFix is unchanged.
bool parseGgaFields(const std::vector<std::string> &fields, GpsFix *outFix)
{
    // Required: type(0), time(1), lat(2), N/S(3), lon(4), E/W(5),
    // fixQuality(6), satellites(7).
    if (fields.size() < 8) {
        return false;
    }

    int fixQuality = 0;
    if (!parseInt(fields[6], &fixQuality)) {
        return false;
    }
    if (fixQuality < 0) {
        return false;
    }

    // fixQuality 0: no valid fix. Mark invalid and return true (parsed OK).
    if (fixQuality == 0) {
        outFix->valid = false;
        return true;
    }

    if (fields[3].size() != 1 || fields[5].size() != 1) {
        return false;
    }
    const char latHemi = static_cast<char>(
        std::toupper(static_cast<unsigned char>(fields[3][0])));
    const char lonHemi = static_cast<char>(
        std::toupper(static_cast<unsigned char>(fields[5][0])));

    double latitude = 0.0;
    double longitude = 0.0;
    if (!convertLatitude(fields[2], latHemi, &latitude)) {
        return false;
    }
    if (!convertLongitude(fields[4], lonHemi, &longitude)) {
        return false;
    }

    int satellites = 0;
    if (!parseInt(fields[7], &satellites)) {
        return false;
    }
    if (satellites < 0) {
        return false;
    }

    double altitude = 0.0;
    // Altitude is field 9; it may be empty when the receiver has a fix but no
    // altitude. Field 8 is HDOP (ignored here).
    if (fields.size() > 9 && !fields[9].empty()) {
        if (!parseDouble(fields[9], &altitude)) {
            return false;
        }
        if (std::isnan(altitude) || std::isinf(altitude)) {
            return false;
        }
    }

    outFix->valid = true;
    outFix->latitude = latitude;
    outFix->longitude = longitude;
    outFix->satellites = satellites;
    outFix->fixQuality = fixQuality;
    outFix->altitudeMeters = altitude;
    outFix->utcTime = fields[1];
    return true;
}

} // namespace

bool verifyChecksum(const std::string &body, const std::string &csHex)
{
    if (csHex.size() != 2) {
        return false;
    }

    unsigned char expected = 0;
    for (int i = 0; i < 2; ++i) {
        const char c = csHex[i];
        int nibble = 0;
        if (c >= '0' && c <= '9') {
            nibble = c - '0';
        } else if (c >= 'A' && c <= 'F') {
            nibble = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'f') {
            nibble = c - 'a' + 10;
        } else {
            return false;
        }
        expected = static_cast<unsigned char>((expected << 4) | nibble);
    }

    unsigned char actual = 0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        actual ^= static_cast<unsigned char>(body[i]);
    }

    return actual == expected;
}

bool convertLatitude(const std::string &raw, char hemisphere, double *out)
{
    if (out == nullptr) {
        return false;
    }
    // Latitude format: ddmm.mmmm (at least 4 chars: 2 degree digits + minutes).
    if (raw.size() < 4) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (std::isdigit(static_cast<unsigned char>(raw[i])) == 0) {
            return false;
        }
    }
    const int degrees = (raw[0] - '0') * 10 + (raw[1] - '0');

    double minutes = 0.0;
    if (!parseDouble(raw.substr(2), &minutes)) {
        return false;
    }
    if (minutes < 0.0) {
        return false;
    }

    double decimal = static_cast<double>(degrees) + minutes / kMinutesPerDegree;

    const char h = static_cast<char>(
        std::toupper(static_cast<unsigned char>(hemisphere)));
    if (h == 'S') {
        decimal = -decimal;
    } else if (h != 'N') {
        return false;
    }

    if (decimal < -90.0 || decimal > 90.0) {
        return false;
    }

    *out = decimal;
    return true;
}

bool convertLongitude(const std::string &raw, char hemisphere, double *out)
{
    if (out == nullptr) {
        return false;
    }
    // Longitude format: dddmm.mmmm (at least 5 chars: 3 degree digits + minutes).
    if (raw.size() < 5) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (std::isdigit(static_cast<unsigned char>(raw[i])) == 0) {
            return false;
        }
    }
    const int degrees =
        (raw[0] - '0') * 100 + (raw[1] - '0') * 10 + (raw[2] - '0');

    double minutes = 0.0;
    if (!parseDouble(raw.substr(3), &minutes)) {
        return false;
    }
    if (minutes < 0.0) {
        return false;
    }

    double decimal = static_cast<double>(degrees) + minutes / kMinutesPerDegree;

    const char h = static_cast<char>(
        std::toupper(static_cast<unsigned char>(hemisphere)));
    if (h == 'W') {
        decimal = -decimal;
    } else if (h != 'E') {
        return false;
    }

    if (decimal < -180.0 || decimal > 180.0) {
        return false;
    }

    *out = decimal;
    return true;
}

bool parseNmeaSentence(const std::string &sentence, GpsFix *outFix)
{
    if (outFix == nullptr) {
        return false;
    }

    std::string s = sentence;
    stripTrailingNewlines(&s);

    if (s.empty()) {
        return false;
    }
    if (static_cast<int>(s.size()) > kMaxNmeaLineLength) {
        return false;
    }
    if (s[0] != '$') {
        return false;
    }

    const std::size_t star = s.find('*');
    if (star == std::string::npos) {
        return false;
    }

    const std::string body = s.substr(1, star - 1);
    const std::string csHex = s.substr(star + 1);

    if (!verifyChecksum(body, csHex)) {
        return false;
    }

    const std::vector<std::string> fields = splitByComma(body);
    if (fields.empty()) {
        return false;
    }

    const std::string &type = fields[0];
    if (type == "GPRMC" || type == "GNRMC") {
        return parseRmcFields(fields, outFix);
    }
    if (type == "GPGGA" || type == "GNGGA") {
        return parseGgaFields(fields, outFix);
    }

    // Unknown sentence type: safely ignore. Return true without modifying
    // outFix so a stream of mixed sentences can be fed without losing the
    // last valid fix.
    return true;
}

// --- NmeaStreamBuffer ---

std::size_t NmeaStreamBuffer::feed(const char *data,
                                   std::size_t len,
                                   std::vector<std::string> *outCompleted)
{
    if (data == nullptr || len == 0 || outCompleted == nullptr) {
        return 0;
    }

    m_buf.append(data, len);

    // Cap: if the buffer exceeds the maximum, keep only the tail. This
    // protects against unbounded growth when noise without a valid '$' is
    // received continuously.
    if (static_cast<int>(m_buf.size()) > kMaxRxBufSize) {
        m_buf.erase(0, m_buf.size() - static_cast<std::size_t>(kMaxRxBufSize));
    }

    const std::size_t countBefore = outCompleted->size();

    for (;;) {
        // Find the start of an NMEA sentence. A valid '$' is followed by at
        // least one alphabetic character (the first char of the talker ID).
        std::size_t dollar = std::string::npos;
        for (std::size_t i = 0; i < m_buf.size(); ++i) {
            if (m_buf[i] == '$') {
                if (i + 1 < m_buf.size()) {
                    if (std::isalpha(static_cast<unsigned char>(m_buf[i + 1])) != 0) {
                        dollar = i;
                        break;
                    }
                    // '$' followed by non-alpha: noise, skip.
                } else {
                    // '$' at end of buffer: keep for next feed.
                    dollar = i;
                    break;
                }
            }
        }

        if (dollar == std::string::npos) {
            // No '$' found at all: discard everything.
            m_buf.clear();
            break;
        }

        if (dollar > 0) {
            m_buf.erase(0, dollar);
        }

        // m_buf now starts with '$' (or '$' at end with 1 char).
        if (m_buf.size() < 2) {
            // Need more data to validate the char after '$'.
            break;
        }

        // Find line ending (CR or LF).
        std::size_t end = std::string::npos;
        for (std::size_t i = 1; i < m_buf.size(); ++i) {
            if (m_buf[i] == '\r' || m_buf[i] == '\n') {
                end = i;
                break;
            }
        }

        if (end == std::string::npos) {
            // Partial sentence (no line ending yet). If it's already overlong,
            // discard it to prevent memory bloat.
            if (static_cast<int>(m_buf.size()) > kMaxNmeaLineLength) {
                m_buf.clear();
            }
            break;
        }

        // Extract the complete line (from '$' to before CR/LF).
        std::string line = m_buf.substr(0, end);

        // Skip past the line ending (handle CRLF as a unit).
        std::size_t next = end + 1;
        if (m_buf[end] == '\r' && next < m_buf.size() && m_buf[next] == '\n') {
            ++next;
        }
        m_buf.erase(0, next);

        // Discard overlong lines but continue processing the rest.
        if (static_cast<int>(line.size()) > kMaxNmeaLineLength) {
            continue;
        }

        outCompleted->push_back(std::move(line));
    }

    return outCompleted->size() - countBefore;
}

void NmeaStreamBuffer::reset()
{
    m_buf.clear();
}

std::size_t NmeaStreamBuffer::pendingBytes() const
{
    return m_buf.size();
}

} // namespace vehiclegps
