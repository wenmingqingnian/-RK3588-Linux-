// Unit tests for vehiclegps NMEA parser (Qt-free).
//
// Build (host, no Qt):
//   g++ -std=c++17 -Wall -Wextra -Werror -I../../common
//       test_nmeaparser.cpp ../../common/nmeaparser.cpp -o test_nmeaparser
// Run: ./test_nmeaparser
//
// Covers spec tests 1-22 (basic parsing + error rejection).

#include "nmeaparser.h"

#include <cmath>
#include <cstdio>
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

// Valid sentences (checksums precomputed).
const char *kGprmcOk = "$GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A*79";
const char *kGnrmcOk = "$GNRMC,100012,A,4146.0284,N,12325.3728,E,15.0,90.0,160726,,,A*65";
const char *kGpggaOk = "$GPGGA,100000,4146.0284,N,12325.3728,E,1,08,1.2,70.0,M,,M,,*61";
const char *kGnggaOk = "$GNGGA,100012,4146.0284,N,12325.3728,E,1,10,1.0,72.0,M,,M,,*75";
const char *kGprmcV  = "$GPRMC,100000,V,4146.0284,N,12325.3728,E,,,160726,,,N*6D";
const char *kGpggaQ0 = "$GPGGA,100000,4146.0284,N,12325.3728,E,0,00,,,,,,,*5C";
const char *kGprmcSw = "$GPRMC,100000,A,4146.0284,S,12325.3728,W,22.4,84.4,160726,,,A*76";
const char *kGprmcLatOor = "$GPRMC,100000,A,9999.9999,N,12325.3728,E,22.4,84.4,160726,,,A*70";
const char *kGprmcLonOor = "$GPRMC,100000,A,4146.0284,N,99999.9999,E,22.4,84.4,160726,,,A*79";
const char *kGpvtgUnknown = "$GPVTG,100.0,T,100.0,M,0.0,N,185.2,K,A*2D";
const char *kGprmcBadNum = "$GPRMC,100000,A,abc,N,12325.3728,E,22.4,84.4,160726,,,A*3E";

// --- Basic parsing tests (1-12) ---

void test1_GprmcOk()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcOk, &fix));
    CHECK(fix.valid);
    CHECK(!fix.simulated);
    CHECK_EQ_DOUBLE(fix.latitude, 41.76714, kEps);   // 41 + 46.0284/60
    CHECK_EQ_DOUBLE(fix.longitude, 123.42288, kEps); // 123 + 25.3728/60
    CHECK_EQ_DOUBLE(fix.speedKmh, 22.4 * 1.852, kEps);
    CHECK_EQ_DOUBLE(fix.courseDegrees, 84.4, kEps);
    CHECK(fix.utcTime == "100000");
    CHECK(fix.utcDate == "160726");
}

void test2_GnrmcOk()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGnrmcOk, &fix));
    CHECK(fix.valid);
    CHECK_EQ_DOUBLE(fix.latitude, 41.76714, kEps);
    CHECK_EQ_DOUBLE(fix.longitude, 123.42288, kEps);
    CHECK_EQ_DOUBLE(fix.speedKmh, 15.0 * 1.852, kEps);
    CHECK_EQ_DOUBLE(fix.courseDegrees, 90.0, kEps);
}

void test3_GpggaOk()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGpggaOk, &fix));
    CHECK(fix.valid);
    CHECK_EQ_DOUBLE(fix.latitude, 41.76714, kEps);
    CHECK_EQ_DOUBLE(fix.longitude, 123.42288, kEps);
    CHECK(fix.satellites == 8);
    CHECK(fix.fixQuality == 1);
    CHECK_EQ_DOUBLE(fix.altitudeMeters, 70.0, kEps);
}

void test4_GnggaOk()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGnggaOk, &fix));
    CHECK(fix.valid);
    CHECK(fix.satellites == 10);
    CHECK(fix.fixQuality == 1);
    CHECK_EQ_DOUBLE(fix.altitudeMeters, 72.0, kEps);
}

void test5_StatusAValid()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcOk, &fix));
    CHECK(fix.valid);
}

void test6_StatusVInvalid()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcV, &fix));
    CHECK(!fix.valid);
}

void test7_FixQuality0Invalid()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGpggaQ0, &fix));
    CHECK(!fix.valid);
}

void test8_NorthEastPositive()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcOk, &fix));
    CHECK(fix.latitude > 0.0);
    CHECK(fix.longitude > 0.0);
}

void test9_SouthWestNegative()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcSw, &fix));
    CHECK(fix.valid);
    CHECK_EQ_DOUBLE(fix.latitude, -41.76714, kEps);
    CHECK_EQ_DOUBLE(fix.longitude, -123.42288, kEps);
}

void test10_KnotsToKmh()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcOk, &fix));
    CHECK_EQ_DOUBLE(fix.speedKmh, 22.4 * 1.852, kEps);
}

void test11_AltitudeSatsCourse()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGpggaOk, &fix));
    CHECK_EQ_DOUBLE(fix.altitudeMeters, 70.0, kEps);
    CHECK(fix.satellites == 8);
    vehiclegps::GpsFix rmc;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcOk, &rmc));
    CHECK_EQ_DOUBLE(rmc.courseDegrees, 84.4, kEps);
}

void test12_UtcTimeDate()
{
    vehiclegps::GpsFix fix;
    CHECK(vehiclegps::parseNmeaSentence(kGprmcOk, &fix));
    CHECK(fix.utcTime == "100000");
    CHECK(fix.utcDate == "160726");
}

// --- Error tests (13-22) ---

void test13_BadChecksumRejected()
{
    vehiclegps::GpsFix fix;
    std::string bad = kGprmcOk;
    bad[bad.size() - 1] = '0'; // corrupt last checksum nibble
    CHECK(!vehiclegps::parseNmeaSentence(bad, &fix));
}

void test14_MissingChecksumRejected()
{
    vehiclegps::GpsFix fix;
    std::string noCs = "$GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A";
    CHECK(!vehiclegps::parseNmeaSentence(noCs, &fix));
}

void test15_MissingFieldRejected()
{
    vehiclegps::GpsFix fix;
    // Remove the speed/course/date fields to leave too few commas.
    std::string missing = "$GPRMC,100000,A,4146.0284,N,12325.3728,E*00";
    CHECK(!vehiclegps::parseNmeaSentence(missing, &fix));
}

void test16_InvalidNumberRejected()
{
    vehiclegps::GpsFix fix;
    CHECK(!vehiclegps::parseNmeaSentence(kGprmcBadNum, &fix));
}

void test17_NanInfRejected()
{
    double out = 0.0;
    CHECK(!vehiclegps::convertLatitude("nan", 'N', &out));
    CHECK(!vehiclegps::convertLatitude("inf", 'N', &out));
    CHECK(!vehiclegps::convertLongitude("nan", 'E', &out));
    CHECK(!vehiclegps::convertLongitude("inf", 'E', &out));
}

void test18_LatitudeOutOfRangeRejected()
{
    vehiclegps::GpsFix fix;
    CHECK(!vehiclegps::parseNmeaSentence(kGprmcLatOor, &fix));
}

void test19_LongitudeOutOfRangeRejected()
{
    vehiclegps::GpsFix fix;
    CHECK(!vehiclegps::parseNmeaSentence(kGprmcLonOor, &fix));
}

void test20_OverlongSentenceRejected()
{
    vehiclegps::GpsFix fix;
    std::string longLine = "$GPRMC,";
    longLine.append(vehiclegps::kMaxNmeaLineLength + 10, 'x');
    CHECK(!vehiclegps::parseNmeaSentence(longLine, &fix));
}

void test21_EmptyInputRejected()
{
    vehiclegps::GpsFix fix;
    CHECK(!vehiclegps::parseNmeaSentence("", &fix));
}

void test22_UnknownTypeSafelyIgnored()
{
    vehiclegps::GpsFix fix;
    fix.valid = true; // sentinel: unknown sentence must not clear it
    CHECK(vehiclegps::parseNmeaSentence(kGpvtgUnknown, &fix));
    CHECK(fix.valid); // unchanged
}

void testChecksumHelper()
{
    // XOR of "GPRMC,100000,A" = 0x0B (precomputed).
    CHECK(vehiclegps::verifyChecksum("GPRMC,100000,A", "0B"));
    CHECK(!vehiclegps::verifyChecksum("GPRMC,100000,A", "0F"));
    CHECK(!vehiclegps::verifyChecksum("GPRMC,100000,A", "10"));
    // Direct known value: body of line 1 XORs to 0x79.
    CHECK(vehiclegps::verifyChecksum(
        "GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A", "79"));
    CHECK(!vehiclegps::verifyChecksum(
        "GPRMC,100000,A,4146.0284,N,12325.3728,E,22.4,84.4,160726,,,A", "00"));
    CHECK(!vehiclegps::verifyChecksum("ABC", "ZZ")); // bad hex
    CHECK(!vehiclegps::verifyChecksum("ABC", ""));   // empty hex
    CHECK(!vehiclegps::verifyChecksum("ABC", "1"));  // too short
    CHECK(!vehiclegps::verifyChecksum("ABC", "123")); // too long
}

} // namespace

int main()
{
    test1_GprmcOk();
    test2_GnrmcOk();
    test3_GpggaOk();
    test4_GnggaOk();
    test5_StatusAValid();
    test6_StatusVInvalid();
    test7_FixQuality0Invalid();
    test8_NorthEastPositive();
    test9_SouthWestNegative();
    test10_KnotsToKmh();
    test11_AltitudeSatsCourse();
    test12_UtcTimeDate();

    test13_BadChecksumRejected();
    test14_MissingChecksumRejected();
    test15_MissingFieldRejected();
    test16_InvalidNumberRejected();
    test17_NanInfRejected();
    test18_LatitudeOutOfRangeRejected();
    test19_LongitudeOutOfRangeRejected();
    test20_OverlongSentenceRejected();
    test21_EmptyInputRejected();
    test22_UnknownTypeSafelyIgnored();

    testChecksumHelper();

    if (g_failures == 0) {
        std::puts("nmeaparser tests passed");
        return 0;
    }
    std::printf("nmeaparser tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
