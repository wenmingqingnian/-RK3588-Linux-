#include "mpu6050.h"
#include "mpu6050worker.h"

#include "logmanager.h"

#include <QMetaObject>
#include <QThread>

namespace {
const char *const MODULE = "mpu6050";
} // namespace

Mpu6050Reader::Mpu6050Reader(const vehicledevice::Mpu6050Config &config,
                               QObject *parent)
    : QObject(parent),
      config_(config),
      workerThread_(nullptr),
      worker_(nullptr),
      available_(false)
{
}

Mpu6050Reader::~Mpu6050Reader()
{
    stopReading();
}

void Mpu6050Reader::startReading()
{
    if (!config_.enabled) {
        VLOG_INFO(MODULE, "input disabled");
        return;
    }

    if (workerThread_ != nullptr) {
        return;
    }

    workerThread_ = new QThread(this);
    worker_ = new Mpu6050Worker(config_);
    worker_->moveToThread(workerThread_);

    connect(workerThread_, &QThread::started, worker_, &Mpu6050Worker::start);
    connect(worker_, &Mpu6050Worker::availabilityChanged,
            this, &Mpu6050Reader::onWorkerAvailabilityChanged);
    connect(worker_, &Mpu6050Worker::availabilityChanged,
            this, &Mpu6050Reader::availabilityChanged);
    connect(worker_, &Mpu6050Worker::readingUpdated,
            this, &Mpu6050Reader::updateMpu6050Data);
    connect(worker_, &Mpu6050Worker::warning,
            this, &Mpu6050Reader::onWorkerWarning);
    connect(worker_, &Mpu6050Worker::warning,
            this, &Mpu6050Reader::warning);

    workerThread_->start();
}

void Mpu6050Reader::stopReading()
{
    if (workerThread_ == nullptr) {
        available_ = false;
        return;
    }

    if (worker_ != nullptr && workerThread_->isRunning()) {
        QMetaObject::invokeMethod(worker_, "stop", Qt::BlockingQueuedConnection);
    }

    workerThread_->quit();
    workerThread_->wait();
    delete worker_;
    worker_ = nullptr;
    delete workerThread_;
    workerThread_ = nullptr;
    available_ = false;
}

bool Mpu6050Reader::isAvailable() const
{
    return available_;
}

void Mpu6050Reader::onWorkerAvailabilityChanged(bool available)
{
    available_ = available;
}

void Mpu6050Reader::onWorkerWarning(QString message)
{
    VLOG_WARN(MODULE, "%s", message.toUtf8().constData());
}
