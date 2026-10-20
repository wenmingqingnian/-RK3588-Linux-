#include "attitude_estimator.h"

#include <cmath>

namespace common {

namespace {

constexpr double kDegPerRad = 180.0 / M_PI;
constexpr double kRadPerDeg = M_PI / 180.0;

} // namespace

AttitudeEstimator::AttitudeEstimator()
    : pitch_(0.0),
      roll_(0.0),
      initialized_(false)
{
}

void AttitudeEstimator::update(double accelX, double accelY, double accelZ,
                                double gyroX, double gyroY, double gyroZ,
                                double dt)
{
    // Compute gravity-based pitch/roll from accelerometer.
    // Pitch: atan2(-X, sqrt(Y^2 + Z^2)) — positive = nose up.
    // Roll:  atan2( Y, Z)                  — positive = right tilt.
    // (Standard NED convention: chip flat, Z-up; X forward, Y left/right.)
    const double accelPitchRad = std::atan2(-accelX,
                                             std::sqrt(accelY * accelY +
                                                       accelZ * accelZ));
    const double accelRollRad = std::atan2(accelY, accelZ);

    const double accelPitch = accelPitchRad * kDegPerRad;
    const double accelRoll = accelRollRad * kDegPerRad;

    if (!initialized_) {
        // First sample: seed the filter with the accel-derived angles
        // to avoid the integrator starting at (0, 0) and ramping up.
        pitch_ = accelPitch;
        roll_ = accelRoll;
        initialized_ = true;
        return;
    }

    // Gyro integration: rate (deg/s) * dt (s) = angle increment (deg).
    // Gyro X = roll rate, Gyro Y = pitch rate (axis convention matches
    // the accel-based computation above).
    const double gyroPitch = pitch_ + gyroY * dt;
    const double gyroRoll  = roll_  + gyroX * dt;

    // Complementary filter: high-pass on gyro, low-pass on accel.
    pitch_ = kAlpha * gyroPitch + (1.0 - kAlpha) * accelPitch;
    roll_  = kAlpha * gyroRoll  + (1.0 - kAlpha) * accelRoll;
}

void AttitudeEstimator::reset()
{
    pitch_ = 0.0;
    roll_ = 0.0;
    initialized_ = false;
}

} // namespace common
