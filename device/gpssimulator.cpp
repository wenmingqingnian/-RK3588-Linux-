#include "gpssimulator.h"

#include <fstream>
#include <sstream>
#include <string>

namespace vehiclegps {

namespace {

// Strip trailing CR/LF/whitespace from a line read from the file.
std::string stripLine(const std::string &line)
{
    std::string out = line;
    while (!out.empty()) {
        const char c = out.back();
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            out.pop_back();
        } else {
            break;
        }
    }
    return out;
}

} // namespace

bool GpsSimulator::loadFile(const std::string &path)
{
    // Reset state before attempting to load.
    m_sentences.clear();
    m_index = 0;
    m_loaded = false;

    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::string line;
    bool hasValidFix = false;
    while (std::getline(input, line)) {
        const std::string stripped = stripLine(line);
        if (stripped.empty()) {
            continue;
        }
        // Only accept lines that look like NMEA sentences.
        if (stripped[0] != '$') {
            continue;
        }

        GpsFix fix;
        if (!parseNmeaSentence(stripped, &fix)) {
            // Parse error (bad checksum, missing fields, etc.): skip.
            continue;
        }
        if (!fix.valid) {
            // Parsed but no valid fix (status V, fixQuality 0): skip.
            continue;
        }

        m_sentences.push_back(stripped);
        hasValidFix = true;
    }

    if (!hasValidFix) {
        // Empty file, all-invalid file, or file with no valid fixes: fail.
        m_sentences.clear();
        return false;
    }

    m_loaded = true;
    return true;
}

bool GpsSimulator::loaded() const
{
    return m_loaded;
}

std::size_t GpsSimulator::sentenceCount() const
{
    return m_sentences.size();
}

bool GpsSimulator::nextFix(GpsFix *out)
{
    if (!m_loaded || out == nullptr) {
        return false;
    }
    if (m_sentences.empty()) {
        return false;
    }

    // Parse the current sentence directly into *out. Since loadFile only stored
    // sentences that produced valid fixes, this parse is guaranteed to succeed.
    // Parsing into *out (rather than a fresh GpsFix) preserves fields from
    // previous sentences of a different type — e.g. an RMC sentence fills
    // speed/course/date but leaves satellites/altitude from a prior GGA sentence
    // intact. This lets the caller accumulate a complete fix across RMC+GGA
    // pairs without any merge logic.
    if (!parseNmeaSentence(m_sentences[m_index], out)) {
        // Should not happen (loadFile validated), but fail-closed just in case.
        return false;
    }

    out->simulated = true;

    // Advance with wrap-around (loop playback).
    ++m_index;
    if (m_index >= m_sentences.size()) {
        m_index = 0;
    }

    return true;
}

void GpsSimulator::rewind()
{
    m_index = 0;
}

std::size_t GpsSimulator::position() const
{
    return m_index;
}

} // namespace vehiclegps
