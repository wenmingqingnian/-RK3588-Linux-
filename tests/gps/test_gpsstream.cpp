// Unit tests for vehiclegps::NmeaStreamBuffer streaming reassembler (Qt-free).
//
// Build (host, no Qt):
//   g++ -std=c++17 -Wall -Wextra -Werror -I../../common
//       test_gpsstream.cpp ../../common/nmeaparser.cpp -o test_gpsstream
// Run: ./test_gpsstream
//
// Covers spec streaming tests 21-26:
//   21. One RMC sentence split into three reads still parses.
//   22. Two sentences in one read produce two complete sentences.
//   23. After leading noise, the buffer resynchronizes on '$'.
//   24. After an invalid sentence, a subsequent valid sentence still parses.
//   25. A partial (half) sentence produces no output.
//   26. The receive buffer never grows unboundedly (cap = kMaxRxBufSize).

#include "nmeaparser.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

// A valid GPRMC sentence (checksum 79) used across streaming tests. Body
// between '$' and '*' XORs to 0x79 (verified via test_nmeaparser.cpp).
const char *kGprmcOk =
    "$GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A*79\r\n";

// A valid GPGGA sentence (checksum 61).
const char *kGpggaOk =
    "$GPGGA,100000,4146.0284,N,12325.3728,E,1,08,1.2,70.0,M,,M,,*61\r\n";

// Helper: feed a raw buffer and return completed sentences.
std::vector<std::string> feedAll(vehiclegps::NmeaStreamBuffer &buf,
                                 const std::string &raw)
{
    std::vector<std::string> completed;
    buf.feed(raw.data(), raw.size(), &completed);
    return completed;
}

// --- Streaming tests ---

// 21. Split one sentence into three reads; final read yields the sentence.
void test21_SplitThreeReads()
{
    vehiclegps::NmeaStreamBuffer buf;
    const std::string s(kGprmcOk);

    std::vector<std::string> r1 = feedAll(buf, s.substr(0, 10));
    std::vector<std::string> r2 = feedAll(buf, s.substr(10, 20));
    std::vector<std::string> r3 = feedAll(buf, s.substr(30));

    CHECK(r1.empty());
    CHECK(r2.empty());
    CHECK(r3.size() == 1);
    CHECK(r3[0].find("$GPRMC") == 0);
    CHECK(r3[0].find("*79") != std::string::npos);

    // The reassembled sentence must parse successfully.
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(r3[0], &fix));
    CHECK(fix.valid);
}

// 22. Two sentences in one read produce two complete sentences in order.
void test22_TwoSentencesOneRead()
{
    vehiclegps::NmeaStreamBuffer buf;
    std::string raw;
    raw += kGprmcOk;
    raw += kGpggaOk;

    std::vector<std::string> r = feedAll(buf, raw);
    CHECK(r.size() == 2);
    CHECK(r[0].find("$GPRMC") == 0);
    CHECK(r[1].find("$GPGGA") == 0);
}

// 23. Leading garbage before '$' is skipped until sync resumes.
void test23_NoiseResync()
{
    vehiclegps::NmeaStreamBuffer buf;
    std::string raw = "\x02\x03noise!@#$%"; // noise, then '$' begins a sentence
    raw += kGprmcOk;

    std::vector<std::string> r = feedAll(buf, raw);
    CHECK(r.size() == 1);
    CHECK(r[0].find("$GPRMC") == 0);
}

// 24. After an invalid (bad checksum) sentence, a valid one still parses.
void test24_InvalidThenValid()
{
    vehiclegps::NmeaStreamBuffer buf;
    // Construct a syntactically well-formed sentence with a wrong checksum.
    std::string bad = "$GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A*00\r\n";
    std::string raw = bad + kGprmcOk;

    std::vector<std::string> r = feedAll(buf, raw);
    CHECK(r.size() == 2); // both are emitted as complete sentences

    // The first must fail to parse, the second must succeed.
    vehiclegps::GpsFix f1;
    CHECK(!vehiclegps::parseNmeaSentence(r[0], &f1));
    vehiclegps::GpsFix f2;
    CHECK(vehiclegps::parseNmeaSentence(r[1], &f2));
    CHECK(f2.valid);
}

// 25. A partial sentence (no terminator yet) yields no output.
void test25_HalfSentenceNoOutput()
{
    vehiclegps::NmeaStreamBuffer buf;
    std::string s(kGprmcOk);
    // Feed only the first half, without trailing CR/LF.
    std::vector<std::string> r = feedAll(buf, s.substr(0, s.size() / 2));
    CHECK(r.empty());
    CHECK(buf.pendingBytes() > 0); // bytes are buffered
}

// 26. The internal buffer never exceeds kMaxRxBufSize under noise / overlong
//    input. Feeding >kMaxRxBufSize bytes of garbage must not grow pendingBytes
//    past the cap.
void test26_BufferCapped()
{
    vehiclegps::NmeaStreamBuffer buf;
    const std::size_t feedSize =
        static_cast<std::size_t>(vehiclegps::kMaxRxBufSize) * 4 + 1234;
    std::string noise;
    noise.assign(feedSize, 'X'); // no '$', no terminator

    std::vector<std::string> r;
    buf.feed(noise.data(), noise.size(), &r);

    CHECK(r.empty());
    CHECK(buf.pendingBytes() <=
          static_cast<std::size_t>(vehiclegps::kMaxRxBufSize));
}

// Bonus: reset() clears pending state and pendingBytes returns to 0.
void testBonus_ResetClearsBuffer()
{
    vehiclegps::NmeaStreamBuffer buf;
    feedAll(buf, std::string(kGprmcOk).substr(0, 20));
    CHECK(buf.pendingBytes() > 0);
    buf.reset();
    CHECK(buf.pendingBytes() == 0);
}

// Bonus: LF-only (no CR) endings are accepted.
void testBonus_LfOnlyEnding()
{
    vehiclegps::NmeaStreamBuffer buf;
    std::string s = "$GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A*79\n";
    std::vector<std::string> r = feedAll(buf, s);
    CHECK(r.size() == 1);
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(r[0], &fix));
    CHECK(fix.valid);
}

} // namespace

int main()
{
    test21_SplitThreeReads();
    test22_TwoSentencesOneRead();
    test23_NoiseResync();
    test24_InvalidThenValid();
    test25_HalfSentenceNoOutput();
    test26_BufferCapped();

    testBonus_ResetClearsBuffer();
    testBonus_LfOnlyEnding();

    if (g_failures == 0) {
        std::puts("gpsstream tests passed");
        return 0;
    }
    std::printf("gpsstream tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
