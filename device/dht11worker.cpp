#include "dht11worker.h"

#include "logmanager.h"

#include <QTimer>

namespace {
const char *const MODULE = "dht11";
// 连续采样失败达到此阈值后，标记设备 unavailable，但定时器继续运行。
// 一旦后续采样成功，立即恢复 available。
const int kFailureThreshold = 3;
} // namespace

Dht11Worker::Dht11Worker(const vehicledevice::Dht11Config &config,
                         QObject *parent)
    : QObject(parent),
      config_(config),
      device_(config),
      pollTimer_(nullptr),
      running_(false),
      consecutiveFailures_(0),
      workerAvailable_(false)
{
}

Dht11Worker::~Dht11Worker()
{
    stop();
}

void Dht11Worker::start()
{
    if (running_) {
        return;
    }

    running_ = true;

    // 定时器始终启动——即使 openDevice 失败也要继续重试。
    // openDevice 失败（例如传感器还没就绪、sysfs 节点未出现）不应导致
    // 整个运行周期永久放弃；定时器持续运行，每次 timeout 尝试重新打开。
    if (pollTimer_ == nullptr) {
        pollTimer_ = new QTimer(this);
        connect(pollTimer_, &QTimer::timeout, this, &Dht11Worker::onPollTimeout);
    }
    pollTimer_->setInterval(config_.pollIntervalMs);
    pollTimer_->start();

    // 首次立即尝试一次 openDevice + read，让 GUI 尽快知道传感器状态。
    onPollTimeout();
}

void Dht11Worker::stop()
{
    if (pollTimer_ != nullptr && pollTimer_->isActive()) {
        pollTimer_->stop();
    }

    device_.closeDevice();
    running_ = false;
    workerAvailable_ = false;
    consecutiveFailures_ = 0;
    emit availabilityChanged(false);
}

void Dht11Worker::onPollTimeout()
{
    // 若设备未打开，尝试打开（不触发采样，只校验 sysfs 节点身份）。
    if (!device_.isAvailable()) {
        std::string openError;
        if (!device_.openDevice(&openError)) {
            // 打开失败：保持 unavailable，定时器继续运行等待下次重试。
            if (!workerAvailable_) {
                // 已是 unavailable，避免重复发信号；只记日志。
                VLOG_WARN(MODULE, "open retry failed: %s", openError.c_str());
            }
            return;
        }
        // 打开成功：进入正常采样路径。
    }

    std::string readError;
    const vehicledevice::Dht11Reading reading =
        device_.readTemperatureHumidity(&readError);

    if (reading.valid) {
        consecutiveFailures_ = 0;
        if (!workerAvailable_) {
            workerAvailable_ = true;
            emit availabilityChanged(true);
            VLOG_INFO(MODULE, "device became available");
        }
        emit readingUpdated(QString::number(reading.humidity, 'f', 1),
                            QString::number(reading.temperature, 'f', 1));
        return;
    }

    // 采样失败。
    ++consecutiveFailures_;
    emit warning(QString("dht11 read error: ") +
                 QString::fromStdString(reading.errorMessage.empty()
                                            ? readError
                                            : reading.errorMessage));

    // 连续失败达到阈值：标记 unavailable，但定时器继续运行，等待恢复。
    // 条件不依赖 workerAvailable_——即使第一帧从未成功，连续失败也必须
    // 触发 closeDevice()，让下次 onPollTimeout() 重新 openDevice()，
    // 这样 IIO 编号变化或 sysfs 节点重创后能自动重新定位。
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
        // 始终关闭设备，强制下次 timeout 重新 openDevice：
        //  - openDevice 会重新解析 resolvedSysfsDir_
        //    （配置空时重新扫描 iio:device*/name=="dht11"）；
        //  - 这也是 openDevice 失败重试的入口。
        device_.closeDevice();
    }
}
