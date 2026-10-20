#ifndef VEHICLEDEVICE_GPSSIMULATOR_H
#define VEHICLEDEVICE_GPSSIMULATOR_H

// Qt-free GPS simulation file player.
//
// The simulator reads an NMEA text file, parses each line through the SAME
// vehiclegps::parseNmeaSentence entry point used by the real serial device,
// and yields GpsFix values with simulated=true. It never invents coordinates
// directly: a simulation result is only produced when a real NMEA sentence
// parses successfully.
//
// Failure policy (fail-closed, no fake coordinates):
//   - File missing             -> loadFile returns false, loaded()==false
//   - File empty               -> loadFile returns false
//   - All sentences invalid    -> loadFile returns false
//   - nextFix on unloaded state-> returns false
//
// Playback wraps (loops) at EOF by default; the caller may stop at any time.
// This class owns no threads and no Qt state; the Qt worker layer drives it.

#include "nmeaparser.h"

#include <cstddef>
#include <string>
#include <vector>

namespace vehiclegps {

class GpsSimulator {
public:
    // Load and validate an NMEA file. Returns true only if the file exists,
    // contains at least one parseable sentence, and at least one sentence
    // yields a valid fix. On failure the simulator stays unloaded.
    bool loadFile(const std::string &path);

    // Whether loadFile succeeded.
    bool loaded() const;

    // Total valid NMEA sentences stored (0 when unloaded).
    std::size_t sentenceCount() const;

    // Produce the next fix by parsing the current sentence directly into *out.
    // On success out->simulated is forced true. Returns false when unloaded.
    // Wraps to the start at EOF (loop playback).
    //
    // Parsing into *out (rather than a fresh GpsFix) preserves fields from
    // prior sentences of the complementary type: an RMC sentence fills
    // speed/course/date but leaves satellites/altitude from a previous GGA
    // sentence intact, and vice versa. Callers that want a complete fix across
    // RMC+GGA pairs should pass a persistent GpsFix rather than a fresh one.
    // Callers that want isolated per-sentence fixes should pass a default-
    // initialized GpsFix each call.
    bool nextFix(GpsFix *out);

    // Reset playback position to the first sentence.
    void rewind();

    // Current playback index (for diagnostics).
    std::size_t position() const;

private:
    std::vector<std::string> m_sentences;
    std::size_t m_index = 0;
    bool m_loaded = false;
};

} // namespace vehiclegps

#endif // VEHICLEDEVICE_GPSSIMULATOR_H
