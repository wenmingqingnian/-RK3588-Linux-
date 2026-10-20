#include "mpu6050worker.h"

#include "mpu6050_iiobuffersource.h"

#include "logmanager.h"

#include <QTimer>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <time.h>

namespace {
const char *const MODULE = "mpu6050";
// 连续采样失败达到此阈值后，标记设备 unavailable，但定时器继续运行。
// 一旦后续采样成功，立即恢复 available。
const int kFailureThreshold = 5;

// 姿态估计在 dt 异常时（>0.5s 或 <0.001s）跳过，避免积分出错。
// 这种情况通常出现在定时器启动初期或系统卡顿。
constexpr double kDtMinSec = 0.001;
constexpr double kDtMaxSec = 0.5;

// A/B 统计日志周期。只在 CARTERMINAL_MPU_AB_STATS=1 时启用。
constexpr double kAbStatsLogIntervalSec = 10.0;
} // namespace

Mpu6050Worker::Mpu6050Worker(const vehicledevice::Mpu6050Config &config,
                               QObject *parent)
    : QObject(parent),
      config_(config),
      source_(),
      estimator_(),
      pollTimer_(nullptr),
      running_(false),
      consecutiveFailures_(0),
      workerAvailable_(false),
      lastUpdateTime_()
{
    // 根据配置选择数据源后端
    if (config_.backend == "iio_buffer") {
        source_ = std::make_unique<vehicledevice::Mpu6050IioBufferSource>(config);
        VLOG_INFO(MODULE, "using IIO buffer backend");
    } else {
        // 默认 user_i2c（旧实现）
        source_ = std::make_unique<vehicledevice::Mpu6050Device>(config);
        VLOG_INFO(MODULE, "using user I2C backend");
    }

    abStatsEnabled_ =
        qEnvironmentVariableIntValue("CARTERMINAL_MPU_AB_STATS") > 0;
    if (abStatsEnabled_) {
        VLOG_INFO(MODULE, "A/B sampling statistics enabled");
    }
}

Mpu6050Worker::~Mpu6050Worker()
{
    stop();
}

void Mpu6050Worker::start()
{
    if (running_) {
        return;
    }

    running_ = true;
    if (abStatsEnabled_) {
        resetAbStats();
    }

    // 定时器始终启动——即使 openDevice 失败也要继续重试。
    if (pollTimer_ == nullptr) {
        pollTimer_ = new QTimer(this);
        connect(pollTimer_, &QTimer::timeout, this, &Mpu6050Worker::onPollTimeout);
    }
    pollTimer_->setInterval(config_.pollIntervalMs);
    pollTimer_->start();

    // 首次立即尝试一次 openDevice + read，让 GUI 尽快知道传感器状态。
    onPollTimeout();
}

void Mpu6050Worker::stop()
{
    if (abStatsEnabled_ && running_) {
        maybeLogAbStats(std::chrono::steady_clock::now(), true);
    }

    if (pollTimer_ != nullptr && pollTimer_->isActive()) {
        pollTimer_->stop();
    }

    if (source_)
        source_->close();
    estimator_.reset();
    running_ = false;
    workerAvailable_ = false;
    consecutiveFailures_ = 0;
    emit availabilityChanged(false);
}

double Mpu6050Worker::currentThreadCpuMs()
{
#if defined(__linux__)
    struct timespec ts;
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        return static_cast<double>(ts.tv_sec) * 1000.0 +
               static_cast<double>(ts.tv_nsec) / 1000000.0;
    }
#endif
    return 0.0;
}

void Mpu6050Worker::resetAbStats()
{
    const auto now = std::chrono::steady_clock::now();

    abEverOpened_ = false;
    abPollCallbacks_ = 0;
    abOpenAttempts_ = 0;
    abOpenSuccesses_ = 0;
    abOpenErrors_ = 0;
    abReopenCount_ = 0;
    abReadAttempts_ = 0;
    abSuccessfulSamples_ = 0;
    abReadErrors_ = 0;
    abMissedPeriods_ = 0;
    abIntervalCount_ = 0;
    abReadLatencyCount_ = 0;

    abIntervalSumMs_ = 0.0;
    abIntervalSumSqMs_ = 0.0;
    abIntervalMinMs_ = std::numeric_limits<double>::infinity();
    abIntervalMaxMs_ = 0.0;
    abMaxAbsJitterMs_ = 0.0;
    abReadLatencySumUs_ = 0.0;
    abReadLatencyMaxUs_ = 0.0;
    abThreadCpuStartMs_ = currentThreadCpuMs();

    abStartTime_ = now;
    abLastLogTime_ = now;
    abLastSuccessTime_ = std::chrono::steady_clock::time_point();
}

void Mpu6050Worker::maybeLogAbStats(
    const std::chrono::steady_clock::time_point &now,
    bool force)
{
    if (!abStatsEnabled_) {
        return;
    }

    const double sinceLastLogSec =
        std::chrono::duration<double>(now - abLastLogTime_).count();
    if (!force && sinceLastLogSec < kAbStatsLogIntervalSec) {
        return;
    }

    const double elapsedSec =
        std::chrono::duration<double>(now - abStartTime_).count();

    const double intervalMeanMs =
        abIntervalCount_ > 0
            ? abIntervalSumMs_ / static_cast<double>(abIntervalCount_)
            : 0.0;

    const double intervalVariance =
        abIntervalCount_ > 0
            ? std::max(
                  0.0,
                  abIntervalSumSqMs_ /
                          static_cast<double>(abIntervalCount_) -
                      intervalMeanMs * intervalMeanMs)
            : 0.0;

    const double intervalStdMs = std::sqrt(intervalVariance);
    const double intervalMinMs =
        abIntervalCount_ > 0 ? abIntervalMinMs_ : 0.0;

    const double effectiveRateHz =
        abIntervalCount_ > 0 && abIntervalSumMs_ > 0.0
            ? 1000.0 * static_cast<double>(abIntervalCount_) /
                  abIntervalSumMs_
            : 0.0;

    const double readLatencyMeanUs =
        abReadLatencyCount_ > 0
            ? abReadLatencySumUs_ /
                  static_cast<double>(abReadLatencyCount_)
            : 0.0;

    const double threadCpuMs =
        std::max(0.0, currentThreadCpuMs() - abThreadCpuStartMs_);
    const double threadCpuPercent =
        elapsedSec > 0.0 ? threadCpuMs / (elapsedSec * 10.0) : 0.0;

    const char *backend =
        config_.backend.empty() ? "user_i2c" : config_.backend.c_str();

    VLOG_INFO(
        MODULE,
        "AB_STATS backend=%s final=%d elapsed_s=%.3f "
        "polls=%llu open_attempts=%llu open_ok=%llu open_err=%llu "
        "reopen=%llu read_attempts=%llu samples=%llu read_err=%llu "
        "rate_hz=%.3f interval_mean_ms=%.3f interval_std_ms=%.3f "
        "interval_min_ms=%.3f interval_max_ms=%.3f "
        "jitter_max_ms=%.3f missed_periods=%llu "
        "read_mean_us=%.1f read_max_us=%.1f "
        "thread_cpu_ms=%.1f thread_cpu_pct=%.3f",
        backend,
        force ? 1 : 0,
        elapsedSec,
        static_cast<unsigned long long>(abPollCallbacks_),
        static_cast<unsigned long long>(abOpenAttempts_),
        static_cast<unsigned long long>(abOpenSuccesses_),
        static_cast<unsigned long long>(abOpenErrors_),
        static_cast<unsigned long long>(abReopenCount_),
        static_cast<unsigned long long>(abReadAttempts_),
        static_cast<unsigned long long>(abSuccessfulSamples_),
        static_cast<unsigned long long>(abReadErrors_),
        effectiveRateHz,
        intervalMeanMs,
        intervalStdMs,
        intervalMinMs,
        abIntervalMaxMs_,
        abMaxAbsJitterMs_,
        static_cast<unsigned long long>(abMissedPeriods_),
        readLatencyMeanUs,
        abReadLatencyMaxUs_,
        threadCpuMs,
        threadCpuPercent);

    abLastLogTime_ = now;
}

void Mpu6050Worker::onPollTimeout()
{
    if (abStatsEnabled_) {
        ++abPollCallbacks_;
    }

    // 若设备未打开，尝试打开（含 WHO_AM_I 校验 + 配置寄存器写入）。
    if (!source_ || !source_->isOpen()) {
        if (abStatsEnabled_) {
            ++abOpenAttempts_;
        }

        std::string openError;
        if (!source_ || !source_->open(&openError)) {
            if (abStatsEnabled_) {
                ++abOpenErrors_;
                maybeLogAbStats(std::chrono::steady_clock::now(), false);
            }

            if (!workerAvailable_) {
                // 已是 unavailable，避免重复发信号；只记日志。
                VLOG_WARN(MODULE, "open retry failed: %s", openError.c_str());
            }
            return;
        }
        // 打开成功：重置姿态估计器，避免上一会话的状态污染积分。
        estimator_.reset();
        lastUpdateTime_ = std::chrono::steady_clock::now();

        if (abStatsEnabled_) {
            ++abOpenSuccesses_;
            if (abEverOpened_) {
                ++abReopenCount_;
            } else {
                abEverOpened_ = true;
            }
            abLastSuccessTime_ =
                std::chrono::steady_clock::time_point();
        }

        VLOG_INFO(MODULE, "source opened (backend=%s)",
                  config_.backend.empty() ? "user_i2c" : config_.backend.c_str());
    }

    const auto readStart = std::chrono::steady_clock::now();
    if (abStatsEnabled_) {
        ++abReadAttempts_;
    }

    std::string readError;
    const vehicledevice::Mpu6050Reading reading =
        source_->readSample(&readError);
    const auto readEnd = std::chrono::steady_clock::now();

    if (abStatsEnabled_) {
        const double readLatencyUs =
            std::chrono::duration<double, std::micro>(
                readEnd - readStart).count();
        ++abReadLatencyCount_;
        abReadLatencySumUs_ += readLatencyUs;
        abReadLatencyMaxUs_ =
            std::max(abReadLatencyMaxUs_, readLatencyUs);
    }

    if (!reading.valid) {
        if (abStatsEnabled_) {
            ++abReadErrors_;
        }

        ++consecutiveFailures_;
        emit warning(QString("mpu6500 read error: ") +
                     QString::fromStdString(reading.errorMessage.empty()
                                                ? readError
                                                : reading.errorMessage));

        if (consecutiveFailures_ >= kFailureThreshold) {
            if (workerAvailable_) {
                workerAvailable_ = false;
                emit availabilityChanged(false);
                VLOG_WARN(MODULE,
                          "device marked unavailable after %d consecutive failures",
                          consecutiveFailures_);
            } else {
                VLOG_WARN(MODULE,
                          "device still failing (%d consecutive failures); "
                          "forcing reopen on next poll",
                          consecutiveFailures_);
            }
            // 始终关闭设备，强制下次 timeout 重新 open
            source_->close();
        }

        if (abStatsEnabled_) {
            maybeLogAbStats(readEnd, false);
        }
        return;
    }

    // 采样成功。
    if (consecutiveFailures_ > 0) {
        VLOG_INFO(MODULE, "recovered after %d failures",
                  consecutiveFailures_);
    }
    consecutiveFailures_ = 0;

    if (!workerAvailable_) {
        workerAvailable_ = true;
        emit availabilityChanged(true);
        VLOG_INFO(MODULE, "device became available");
    }

    if (abStatsEnabled_) {
        ++abSuccessfulSamples_;

        if (abLastSuccessTime_ !=
            std::chrono::steady_clock::time_point()) {
            const double intervalMs =
                std::chrono::duration<double, std::milli>(
                    readEnd - abLastSuccessTime_).count();

            ++abIntervalCount_;
            abIntervalSumMs_ += intervalMs;
            abIntervalSumSqMs_ += intervalMs * intervalMs;
            abIntervalMinMs_ =
                std::min(abIntervalMinMs_, intervalMs);
            abIntervalMaxMs_ =
                std::max(abIntervalMaxMs_, intervalMs);

            const double expectedMs =
                static_cast<double>(std::max(1, config_.pollIntervalMs));
            abMaxAbsJitterMs_ =
                std::max(abMaxAbsJitterMs_,
                         std::fabs(intervalMs - expectedMs));

            if (intervalMs > expectedMs * 1.5) {
                const long long observedPeriods =
                    static_cast<long long>(
                        std::llround(intervalMs / expectedMs));
                if (observedPeriods > 1) {
                    abMissedPeriods_ +=
                        static_cast<std::uint64_t>(
                            observedPeriods - 1);
                }
            }
        }

        abLastSuccessTime_ = readEnd;
    }

    // 计算 dt（秒），用 steady_clock 测量实际间隔避免 QTimer 抖动。
    const auto now = readEnd;
    const double dt = std::chrono::duration<double>(now - lastUpdateTime_).count();
    lastUpdateTime_ = now;

    // 互补滤波：更新 pitch/roll。
    // 跳过 dt 异常的样本（启动初期或系统卡顿），避免积分出错。
    if (dt >= kDtMinSec && dt <= kDtMaxSec) {
        estimator_.update(reading.accelX, reading.accelY, reading.accelZ,
                          reading.gyroX, reading.gyroY, reading.gyroZ, dt);
    }

    emit readingUpdated(reading.accelX, reading.accelY, reading.accelZ,
                         reading.gyroX, reading.gyroY, reading.gyroZ,
                         reading.temperature,
                         estimator_.pitch(), estimator_.roll());

    if (abStatsEnabled_) {
        maybeLogAbStats(now, false);
    }
}
