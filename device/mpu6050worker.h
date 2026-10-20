#ifndef MPU6050WORKER_H
#define MPU6050WORKER_H

#include "attitude_estimator.h"
#include "iimusamplesource.h"
#include "mpu6050device.h"
#include "vehicledeviceconfig.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <QObject>
#include <QString>

class QTimer;

// Mpu6050Worker — 在 Qt worker 线程中周期性读取 MPU6500 并做姿态融合。
//
// 第4周改造：持有 IImuSampleSource 抽象指针，支持两种后端：
//   - user_i2c: Mpu6050Device（旧实现，读 /dev/i2c-3）
//   - iio_buffer: Mpu6050IioBufferSource（新实现，读 /dev/iio:deviceX）
// 通过 config_.backend 字段选择。
//
// 状态机（参考 Dht11Worker 模式）：
//   - start() 后定时器**始终**运行，无论 open 是否成功。
//   - 每次 onPollTimeout()：若设备未 open 则尝试 open；然后尝试 read。
//   - read 成功：用 AttitudeEstimator 做互补滤波，发 readingUpdated 信号。
//     若之前处于 unavailable，发 availabilityChanged(true) 恢复。
//   - read 失败：consecutiveFailures++，发 warning。
//     连续失败 >= kFailureThreshold 时，发 availabilityChanged(false)，
//     关闭设备（下次 timeout 会重新 open 重试），但定时器不停。
//
// 周期：默认 20ms = 50 Hz。dt 用 steady_clock 实际测量，避免 QTimer
// 不精确导致姿态积分漂移。
class Mpu6050Worker : public QObject
{
    Q_OBJECT

public:
    explicit Mpu6050Worker(const vehicledevice::Mpu6050Config &config,
                           QObject *parent = nullptr);
    ~Mpu6050Worker();

public slots:
    void start();
    void stop();

signals:
    void availabilityChanged(bool available);
    // accelX/Y/Z (m/s^2), gyroX/Y/Z (deg/s), temperature (°C),
    // pitch/roll (degrees, after complementary filter)
    void readingUpdated(double ax, double ay, double az,
                        double gx, double gy, double gz,
                        double temperature,
                        double pitch, double roll);
    void warning(QString message);

private slots:
    void onPollTimeout();

private:
    void resetAbStats();
    void maybeLogAbStats(const std::chrono::steady_clock::time_point &now,
                         bool force);
    static double currentThreadCpuMs();

    vehicledevice::Mpu6050Config config_;
    std::unique_ptr<vehicledevice::IImuSampleSource> source_;
    common::AttitudeEstimator estimator_;
    QTimer *pollTimer_;
    bool running_;
    int consecutiveFailures_;
    bool workerAvailable_;
    std::chrono::steady_clock::time_point lastUpdateTime_;

    bool abStatsEnabled_ = false;
    bool abEverOpened_ = false;

    std::uint64_t abPollCallbacks_ = 0;
    std::uint64_t abOpenAttempts_ = 0;
    std::uint64_t abOpenSuccesses_ = 0;
    std::uint64_t abOpenErrors_ = 0;
    std::uint64_t abReopenCount_ = 0;
    std::uint64_t abReadAttempts_ = 0;
    std::uint64_t abSuccessfulSamples_ = 0;
    std::uint64_t abReadErrors_ = 0;
    std::uint64_t abMissedPeriods_ = 0;
    std::uint64_t abIntervalCount_ = 0;
    std::uint64_t abReadLatencyCount_ = 0;

    double abIntervalSumMs_ = 0.0;
    double abIntervalSumSqMs_ = 0.0;
    double abIntervalMinMs_ = 0.0;
    double abIntervalMaxMs_ = 0.0;
    double abMaxAbsJitterMs_ = 0.0;
    double abReadLatencySumUs_ = 0.0;
    double abReadLatencyMaxUs_ = 0.0;
    double abThreadCpuStartMs_ = 0.0;

    std::chrono::steady_clock::time_point abStartTime_;
    std::chrono::steady_clock::time_point abLastLogTime_;
    std::chrono::steady_clock::time_point abLastSuccessTime_;
};

#endif // MPU6050WORKER_H
