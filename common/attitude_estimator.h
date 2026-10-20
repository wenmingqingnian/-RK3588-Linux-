#ifndef ATTITUDE_ESTIMATOR_H
#define ATTITUDE_ESTIMATOR_H

#include <chrono>

namespace common {

// AttitudeEstimator — 6-axis IMU (accel + gyro) complementary filter for
// pitch/roll estimation. No magnetometer → yaw angle is not integrated
// (gyro yaw drift would accumulate without magnetometer correction).
//
// Algorithm (complementary filter, classic for vehicle attitude):
//   pitch_accel = atan2(-accelX, sqrt(accelY^2 + accelZ^2))  // gravity-based
//   roll_accel  = atan2(accelY, accelZ)                       // gravity-based
//   pitch = alpha * (pitch + gyroY * dt) + (1 - alpha) * pitch_accel
//   roll  = alpha * (roll  + gyroX * dt) + (1 - alpha) * roll_accel
//
// alpha = 0.98 is a widely-used value: trust gyro integration for
// short-term accuracy (high-frequency motion) and accel for long-term
// drift correction (low-frequency, gravity reference).
//
// Vehicle use case:
//   - Pitch: forward/backward slope (hill climb / descent)
//   - Roll: left/right tilt (cornering, side slope)
//
// Reset: re-instantiating the class resets the filter to level (0, 0).
// Cold-start settling: ~1-2 seconds for the filter to converge from
// arbitrary initial conditions. Caller may want to discard the first
// few samples after construction.
class AttitudeEstimator {
public:
    AttitudeEstimator();

    // Updates the filter with a new IMU sample.
    //   accelX/Y/Z: accelerometer in m/s^2
    //   gyroX/Y/X:  gyroscope in deg/s (X=roll rate, Y=pitch rate)
    //   dt:         time since previous update (seconds)
    void update(double accelX, double accelY, double accelZ,
                double gyroX, double gyroY, double gyroZ, double dt);

    // Returns pitch angle in degrees (-90..+90). Positive = nose up.
    double pitch() const { return pitch_; }

    // Returns roll angle in degrees (-180..+180). Positive = right tilt.
    double roll() const { return roll_; }

    // True after at least one update() call.
    bool initialized() const { return initialized_; }

    // Resets to level (0, 0). Called on device reopen / cold start.
    void reset();

private:
    double pitch_;       // degrees
    double roll_;        // degrees
    bool initialized_;

    static constexpr double kAlpha = 0.98;  // gyro weight
};

} // namespace common

#endif // ATTITUDE_ESTIMATOR_H
