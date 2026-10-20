#include "dht11.h"
#include "dht11worker.h"

#include "logmanager.h"

#include <QMetaObject>
#include <QThread>

namespace {
const char *const MODULE = "dht11";
} // namespace

Dht11Reader::Dht11Reader(const vehicledevice::Dht11Config &config, QObject *parent)
    : QObject(parent),
      config_(config),
      workerThread_(nullptr),
      worker_(nullptr),
      available_(false)
{
}

Dht11Reader::~Dht11Reader()
{
    stopReading();
}

void Dht11Reader::startReading()
{
    if (!config_.enabled) {
        VLOG_INFO(MODULE, "input disabled");
        return;
    }

    if (workerThread_ != nullptr) {
        return;
    }

    workerThread_ = new QThread(this);
    worker_ = new Dht11Worker(config_);
    worker_->moveToThread(workerThread_);

    connect(workerThread_, &QThread::started, worker_, &Dht11Worker::start);
    connect(worker_, &Dht11Worker::availabilityChanged,
            this, &Dht11Reader::onWorkerAvailabilityChanged);
    connect(worker_, &Dht11Worker::availabilityChanged,
            this, &Dht11Reader::availabilityChanged);
    connect(worker_, &Dht11Worker::readingUpdated,
            this, &Dht11Reader::updateDht11Data);
    connect(worker_, &Dht11Worker::warning,
            this, &Dht11Reader::onWorkerWarning);
    connect(worker_, &Dht11Worker::warning,
            this, &Dht11Reader::warning);

    workerThread_->start();
}

void Dht11Reader::stopReading()
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

bool Dht11Reader::isAvailable() const
{
    return available_;
}

void Dht11Reader::onWorkerAvailabilityChanged(bool available)
{
    available_ = available;
}

void Dht11Reader::onWorkerWarning(QString message)
{
    VLOG_WARN(MODULE, "%s", message.toUtf8().constData());
}
