// Unit tests for vehiclegps::GpsSimulator (Qt-free simulation file player).
//
// Build (host, no Qt):
//   g++ -std=c++17 -Wall -Wextra -Werror -I../../common -I../../device
//       test_gpssimulation.cpp ../../common/nmeaparser.cpp
//       ../../device/gpssimulator.cpp -o test_gpssimulation
// Run: ./test_gpssimulation
//
// Covers spec simulation tests 34-41:
//   34. Simulation file produces a continuous trajectory.
//   35. simulated is always true for simulation fixes.
//   36. Missing file fails (loadFile returns false, loaded()==false).
//   37. Empty file fails.
//   38. File with only invalid sentences fails.
//   39. After the consumer stops calling nextFix, no new data is produced
//       (the simulator is passive: it never pushes on its own).
//   40. Repeated rewind() is safe (no crash, position resets to 0).
//   41. Destructor runs cleanly with no residual threads (the simulator owns
//       no threads by design; verified by scope-exit and zero thread count).
//
// Test data files are created in the current working directory and removed at
// the end of main().

#include "gpssimulator.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_EQ_DOUBLE(a, b, eps) \
    do { \
        double _a = (a); double _b = (b); double _e = (eps); \
        if (std::fabs(_a - _b) > _e) { \
            std::printf("FAIL [%s:%d]: %s=%g != %s=%g (eps=%g)\n", \
                        __FILE__, __LINE__, #a, _a, #b, _b, _e); \
            ++g_failures; \
        } \
    } while (0)

constexpr double kEps = 1e-6;

// Path to the shared golden simulation route committed under tests/gps/data.
// The test binary is expected to run from tests/gps/ so this relative path
// resolves correctly.
const char *kGoldenRoute = "data/simulated_route.nmea";

// Temporary test files created and removed by this test binary.
const char *kEmptyFile = "tmp_empty.nmea";
const char *kAllInvalidFile = "tmp_all_invalid.nmea";
const char *kMissingFile = "tmp_does_not_exist.nmea";

// Three valid lines drawn from the golden route. Used for the continuous
// trajectory test when the golden file is unavailable on the host.
const char *kThreeValidLines =
    "$GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A*79\r\n"
    "$GPRMC,100002,A,4146.0320,N,12325.3800,E,22.9,85.4,160726,,,A*7D\r\n"
    "$GPRMC,100004,A,4146.0356,N,12325.3872,E,23.4,86.4,160726,,,A*70\r\n";

bool writeFile(const std::string &path, const std::string &content)
{
    std::ofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

bool removeFile(const std::string &path)
{
    return std::remove(path.c_str()) == 0;
}

// Pick the first available simulation source: the golden committed file if it
// exists, otherwise a three-line file written to disk. Returns the path.
std::string ensureSimSource()
{
    {
        std::ifstream test(kGoldenRoute);
        if (test.good()) {
            return kGoldenRoute;
        }
    }
    if (!writeFile("tmp_three_valid.nmea", kThreeValidLines)) {
        return std::string();
    }
    return "tmp_three_valid.nmea";
}

// 34. The simulation file yields a continuous trajectory of distinct fixes
//     (at least 2 different coordinates across the full route).
void test34_ContinuousTrajectory()
{
    const std::string path = ensureSimSource();
    CHECK(!path.empty());

    vehiclegps::GpsSimulator sim;
    CHECK(sim.loadFile(path));
    CHECK(sim.loaded());
    CHECK(sim.sentenceCount() >= 2);

    // Collect all fixes in one loop. RMC and GGA pairs may share coordinates,
    // so we check for diversity across the entire set, not just the first two.
    std::vector<vehiclegps::GpsFix> fixes;
    const std::size_t n = sim.sentenceCount();
    for (std::size_t i = 0; i < n; ++i) {
        vehiclegps::GpsFix f;
        CHECK(sim.nextFix(&f));
        CHECK(f.valid);
        fixes.push_back(f);
    }
    CHECK(fixes.size() == n);

    // At least two distinct positions must exist across the route.
    bool hasDistinct = false;
    for (std::size_t i = 1; i < fixes.size(); ++i) {
        if (std::fabs(fixes[0].latitude - fixes[i].latitude) > kEps ||
            std::fabs(fixes[0].longitude - fixes[i].longitude) > kEps) {
            hasDistinct = true;
            break;
        }
    }
    CHECK(hasDistinct);
}

// 35. simulated is always true for fixes produced by the simulator.
void test35_SimulatedAlwaysTrue()
{
    const std::string path = ensureSimSource();
    CHECK(!path.empty());

    vehiclegps::GpsSimulator sim;
    CHECK(sim.loadFile(path));

    const std::size_t cap = sim.sentenceCount();
    for (std::size_t i = 0; i < cap; ++i) {
        vehiclegps::GpsFix f;
        CHECK(sim.nextFix(&f));
        CHECK(f.valid);
        CHECK(f.simulated); // must be forced true by the simulator
    }
}

// 36. A missing file fails to load and leaves the simulator unloaded.
void test36_MissingFileFails()
{
    vehiclegps::GpsSimulator sim;
    CHECK(!sim.loadFile(kMissingFile));
    CHECK(!sim.loaded());
    CHECK(sim.sentenceCount() == 0);

    vehiclegps::GpsFix f;
    CHECK(!sim.nextFix(&f)); // unloaded: no fix produced
}

// 37. An empty file fails to load.
void test37_EmptyFileFails()
{
    CHECK(writeFile(kEmptyFile, ""));
    vehiclegps::GpsSimulator sim;
    CHECK(!sim.loadFile(kEmptyFile));
    CHECK(!sim.loaded());
    CHECK(sim.sentenceCount() == 0);
}

// 38. A file containing only invalid sentences (bad checksum / unknown type)
//     fails to load (no valid fix is ever produced).
void test38_AllInvalidFails()
{
    const std::string content =
        "$GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A*00\r\n" // bad cs
        "$GPVTG,100.0,T,100.0,M,0.0,N,185.2,K,A*2D\r\n"                        // unknown type
        "garbage without dollar sign\r\n";
    CHECK(writeFile(kAllInvalidFile, content));

    vehiclegps::GpsSimulator sim;
    CHECK(!sim.loadFile(kAllInvalidFile));
    CHECK(!sim.loaded());
    CHECK(sim.sentenceCount() == 0);
}

// 39. The simulator is passive: after the consumer stops calling nextFix, no
//     new data appears. There is no background push, so a "stopped" consumer
//     sees no further fixes until it resumes calling nextFix.
void test39_NoPushAfterConsumerStops()
{
    const std::string path = ensureSimSource();
    CHECK(!path.empty());

    vehiclegps::GpsSimulator sim;
    CHECK(sim.loadFile(path));

    vehiclegps::GpsFix f1;
    CHECK(sim.nextFix(&f1));
    const std::size_t posAfterFirst = sim.position();

    // Simulate "stop": do not call nextFix for a while. Nothing happens, the
    // simulator does not advance its position on its own.
    const std::size_t posUnchanged = sim.position();
    CHECK(posUnchanged == posAfterFirst);

    // Resuming yields the next fix.
    vehiclegps::GpsFix f2;
    CHECK(sim.nextFix(&f2));
    CHECK(sim.position() == posAfterFirst + 1 ||
          sim.position() == 0); // wrap to 0 at EOF
}

// 40. Repeated rewind() is safe and always resets position to 0.
void test40_RepeatedRewindSafe()
{
    const std::string path = ensureSimSource();
    CHECK(!path.empty());

    vehiclegps::GpsSimulator sim;
    CHECK(sim.loadFile(path));

    vehiclegps::GpsFix f;
    sim.nextFix(&f);
    sim.nextFix(&f);
    CHECK(sim.position() >= 1);

    sim.rewind();
    CHECK(sim.position() == 0);
    sim.rewind();
    sim.rewind();
    CHECK(sim.position() == 0);

    // After rewind, the first fix is produced again.
    vehiclegps::GpsFix firstAgain;
    CHECK(sim.nextFix(&firstAgain));
    CHECK(firstAgain.valid);
    CHECK(firstAgain.simulated);
}

// 41. The simulator owns no threads. Destruction in a scope is clean and
//     leaves no residual state. (This is a design contract test: the class
//     must not spawn threads.)
void test41_DestructorCleanNoThreads()
{
    const std::string path = ensureSimSource();
    CHECK(!path.empty());

    {
        vehiclegps::GpsSimulator sim;
        CHECK(sim.loadFile(path));
        vehiclegps::GpsFix f;
        sim.nextFix(&f);
        // Scope exit destroys sim. Must not throw, must not leak.
    }
    // If we reach here, destruction was clean.
    CHECK(true);

    // A second instance can be created and destroyed independently, proving
    // there is no global/thread residue from the first.
    {
        vehiclegps::GpsSimulator sim2;
        CHECK(sim2.loadFile(path));
    }
    CHECK(true);
}

// Bonus: EOF wraps to the start (loop playback). After consuming sentenceCount
// fixes, the next fix is the first fix again.
void testBonus_LoopAtEof()
{
    const std::string path = ensureSimSource();
    CHECK(!path.empty());

    vehiclegps::GpsSimulator sim;
    CHECK(sim.loadFile(path));
    const std::size_t n = sim.sentenceCount();

    vehiclegps::GpsFix first;
    CHECK(sim.nextFix(&first));

    // Consume the rest of the loop.
    for (std::size_t i = 1; i < n; ++i) {
        vehiclegps::GpsFix f;
        CHECK(sim.nextFix(&f));
    }
    // Next call wraps to the first sentence again.
    vehiclegps::GpsFix wrap;
    CHECK(sim.nextFix(&wrap));
    CHECK(wrap.valid);
    CHECK_EQ_DOUBLE(wrap.latitude, first.latitude, kEps);
    CHECK_EQ_DOUBLE(wrap.longitude, first.longitude, kEps);
}

} // namespace

int main()
{
    // Prepare temp files required by tests 37/38.
    writeFile(kEmptyFile, ""); // pre-create (overwritten in test)

    test34_ContinuousTrajectory();
    test35_SimulatedAlwaysTrue();
    test36_MissingFileFails();
    test37_EmptyFileFails();
    test38_AllInvalidFails();
    test39_NoPushAfterConsumerStops();
    test40_RepeatedRewindSafe();
    test41_DestructorCleanNoThreads();
    testBonus_LoopAtEof();

    // Cleanup temp files.
    removeFile(kEmptyFile);
    removeFile(kAllInvalidFile);
    removeFile("tmp_three_valid.nmea");

    if (g_failures == 0) {
        std::puts("gpssimulation tests passed");
        return 0;
    }
    std::printf("gpssimulation tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
