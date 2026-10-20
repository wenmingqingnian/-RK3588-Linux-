#ifndef VEHICLECOMMON_NMEAPARSER_H
#define VEHICLECOMMON_NMEAPARSER_H

// Qt-free NMEA 0183 parser and streaming buffer.
//
// This module is intentionally independent of Qt, termios, threads and GUI so
// it can be unit-tested with a plain C++17 compiler and reused by both the
// real serial device layer and the simulation file player.
//
// Responsibilities:
//   - Validate NMEA checksum ($BODY*XX)
//   - Parse $GPRMC / $GNRMC (recommended minimum)
//   - Parse $GPGGA / $GNGGA (fix data)
//   - Convert ddmm.mmmm / dddmm.mmmm to decimal degrees
//   - Apply S/W hemisphere sign
//   - Convert knots to km/h (x1.852)
//   - Reject NaN/Inf, out-of-range lat/lon, negative speed/sats, missing
//     fields, bad numbers, overlong lines
//   - Never overwrite a valid fix with an invalid one (caller-driven)
//
// Constants are named; no magic numbers are scattered in the implementation.

#include <cstddef>
#include <string>
#include <vector>

namespace vehiclegps {

// Maximum accepted NMEA sentence length (including leading '$' and trailing
// "*CC"). Sentences longer than this are rejected and discarded.
constexpr int kMaxNmeaLineLength = 256;

// Maximum raw receive buffer size for the streaming reassembler. Protects
// against unbounded growth when noise is received without a valid '$' start.
constexpr int kMaxRxBufSize = 4096;

// Structured GPS fix produced by the parser. `simulated` is false for real
// serial input; the simulation player sets it to true after parsing.
struct GpsFix {
    bool valid = false;
    bool simulated = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double speedKmh = 0.0;
    double courseDegrees = 0.0;
    double altitudeMeters = 0.0;
    int satellites = 0;
    int fixQuality = 0;
    std::string utcTime;
    std::string utcDate;
};

// Verify the NMEA XOR checksum. `body` is the text strictly between '$' and
// '*'. `csHex` is the two-character hexadecimal checksum after '*'.
// Returns true when the XOR of every byte in `body` equals the parsed hex.
bool verifyChecksum(const std::string &body, const std::string &csHex);

// Parse a complete NMEA sentence. The sentence may include the leading '$'
// and trailing "*CC" (CR/LF are tolerated and stripped). Unknown sentence
// types are silently ignored (return true with outFix unchanged) so a stream
// of mixed sentences can be fed safely.
//
// On a recognized RMC/GGA sentence:
//   - Returns true.
//   - On success, outFix->valid is set true and fields are filled.
//   - On a parse error (bad checksum, missing fields, NaN/Inf, range error,
//     negative speed/satellites), returns false and outFix is left unchanged
//     so the caller's last valid fix is preserved.
//
// `simulated` is never set true here; the simulation layer owns that flag.
bool parseNmeaSentence(const std::string &sentence, GpsFix *outFix);

// Convert a ddmm.mmmm latitude token + N/S hemisphere to decimal degrees.
// S produces a negative value. Returns false on empty/invalid number or
// out-of-range result ([-90, 90]).
bool convertLatitude(const std::string &raw, char hemisphere, double *out);

// Convert a dddmm.mmmm longitude token + E/W hemisphere to decimal degrees.
// W produces a negative value. Returns false on empty/invalid number or
// out-of-range result ([-180, 180]).
bool convertLongitude(const std::string &raw, char hemisphere, double *out);

// Streaming reassembler. GPS serial reads do not align to sentence boundaries:
// one read may contain a partial sentence, multiple sentences, CRLF or LF
// endings, and leading noise. Feed raw bytes and collect complete sentences.
//
// Returned sentences include the leading '$' and trailing "*CC" (no CR/LF),
// ready to pass to parseNmeaSentence(). Overlong lines are dropped and the
// buffer resynchronizes on the next '$'. The internal buffer never exceeds
// kMaxRxBufSize bytes.
class NmeaStreamBuffer {
public:
    // Feed raw bytes. Appends complete sentences (if any) to `outCompleted`.
    // Returns the number of complete sentences appended.
    std::size_t feed(const char *data, std::size_t len,
                     std::vector<std::string> *outCompleted);

    // Reset internal state (e.g. after a device reopen).
    void reset();

    // Current pending bytes (for diagnostics / cap enforcement).
    std::size_t pendingBytes() const;

private:
    std::string m_buf;
};

} // namespace vehiclegps

#endif // VEHICLECOMMON_NMEAPARSER_H
