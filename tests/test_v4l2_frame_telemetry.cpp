#include "v4l2_frame_telemetry.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

bool require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

bool near(double actual, double expected, double tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

bool testContinuousSequenceHasNoGap()
{
    vehiclecamera::V4L2FrameTelemetry telemetry;
    telemetry.reset(0);

    telemetry.observe(100, 10000000ULL);
    telemetry.observe(101, 43333333ULL);
    telemetry.observe(102, 76666666ULL);

    const auto snapshot = telemetry.snapshot(100000000ULL);

    bool ok = true;
    ok &= require(snapshot.hasFrames,
                  "continuous sequence should report frames");
    ok &= require(snapshot.frames == 3,
                  "continuous sequence should count three frames");
    ok &= require(snapshot.sequenceGaps == 0,
                  "continuous sequence should have zero gaps");
    ok &= require(snapshot.firstSequence == 100,
                  "first sequence should be preserved");
    ok &= require(snapshot.lastSequence == 102,
                  "last sequence should be preserved");
    return ok;
}

bool testForwardJumpCountsMissingFrames()
{
    vehiclecamera::V4L2FrameTelemetry telemetry;
    telemetry.reset(0);

    telemetry.observe(100, 10000000ULL);
    telemetry.observe(103, 43333333ULL);

    const auto snapshot = telemetry.snapshot(50000000ULL);

    return require(snapshot.sequenceGaps == 2,
                   "sequence 100 to 103 should report two missing frames");
}

bool testSequenceWrapDoesNotCreateFalseGap()
{
    vehiclecamera::V4L2FrameTelemetry telemetry;
    telemetry.reset(0);

    telemetry.observe(UINT32_MAX - 1U, 10000000ULL);
    telemetry.observe(UINT32_MAX,      43333333ULL);
    telemetry.observe(0,               76666666ULL);
    telemetry.observe(1,              109999999ULL);

    const auto snapshot = telemetry.snapshot(120000000ULL);

    bool ok = true;
    ok &= require(snapshot.frames == 4,
                  "wrap test should count four frames");
    ok &= require(snapshot.sequenceGaps == 0,
                  "32-bit sequence wrap must not report a gap");
    ok &= require(snapshot.lastSequence == 1,
                  "last sequence after wrap should be one");
    return ok;
}

bool testFpsUsesStreamRuntime()
{
    vehiclecamera::V4L2FrameTelemetry telemetry;
    telemetry.reset(0);

    for (std::uint32_t sequence = 1; sequence <= 30; ++sequence) {
        telemetry.observe(
            sequence,
            static_cast<std::uint64_t>(sequence) * 33333333ULL);
    }

    const auto snapshot = telemetry.snapshot(1000000000ULL);

    bool ok = true;
    ok &= require(snapshot.frames == 30,
                  "one-second sample should contain thirty frames");
    ok &= require(near(snapshot.fps, 30.0, 0.01),
                  "thirty frames in one second should report 30 FPS");
    return ok;
}

bool testMaximumFrameInterval()
{
    vehiclecamera::V4L2FrameTelemetry telemetry;
    telemetry.reset(0);

    telemetry.observe(1, 10000000ULL);
    telemetry.observe(2, 40000000ULL);
    telemetry.observe(3, 95000000ULL);

    const auto snapshot = telemetry.snapshot(100000000ULL);

    return require(
        near(snapshot.maxFrameIntervalMs, 55.0, 0.001),
        "maximum interval should report the 55 ms frame stall");
}

bool testResetStartsIndependentStream()
{
    vehiclecamera::V4L2FrameTelemetry telemetry;
    telemetry.reset(0);

    telemetry.observe(10, 10000000ULL);
    telemetry.observe(15, 80000000ULL);

    telemetry.reset(2000000000ULL);
    const auto empty = telemetry.snapshot(2000000000ULL);

    bool ok = true;
    ok &= require(!empty.hasFrames,
                  "reset stream should initially contain no frames");
    ok &= require(empty.frames == 0,
                  "reset should clear frame count");
    ok &= require(empty.sequenceGaps == 0,
                  "reset should clear sequence gaps");
    ok &= require(empty.firstSequence == 0 &&
                      empty.lastSequence == 0,
                  "reset should clear sequence bounds");
    ok &= require(empty.fps == 0.0,
                  "reset should clear FPS");
    ok &= require(empty.maxFrameIntervalMs == 0.0,
                  "reset should clear maximum frame interval");

    telemetry.observe(7, 2010000000ULL);
    const auto restarted = telemetry.snapshot(2100000000ULL);

    ok &= require(restarted.hasFrames,
                  "restarted stream should accept its first frame");
    ok &= require(restarted.frames == 1,
                  "restarted stream should count independently");
    ok &= require(restarted.firstSequence == 7 &&
                      restarted.lastSequence == 7,
                  "restarted stream should establish a new sequence baseline");
    ok &= require(restarted.sequenceGaps == 0,
                  "first frame after reset cannot create a sequence gap");
    ok &= require(restarted.maxFrameIntervalMs == 0.0,
                  "one frame cannot create a frame interval");
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= testContinuousSequenceHasNoGap();
    ok &= testForwardJumpCountsMissingFrames();
    ok &= testSequenceWrapDoesNotCreateFalseGap();
    ok &= testFpsUsesStreamRuntime();
    ok &= testMaximumFrameInterval();
    ok &= testResetStartsIndependentStream();

    if (!ok)
        return 1;

    std::cout << "v4l2 frame telemetry tests passed\n";
    return 0;
}
