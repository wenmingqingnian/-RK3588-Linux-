#include "ap3216.h"
#include "ap3216worker.h"
#include "logmanager.h"

#include <QMetaObject>
#include <QThread>

static const char *MODULE = "ap3216";

Ap3216c::Ap3216c(QObject *parent)
    : Ap3216c(vehicledevice::Ap3216Config(), parent)
{
}

Ap3216c::Ap3216c(const vehicledevice::Ap3216Config &config, QObject *parent)
    : QObject(parent),
      config_(config),
      workerThread_(nullptr),
      worker_(nullptr),
      available_(false),
      alsdata("N/A"),
      psdata("N/A"),
      irdata("N/A")
{
}

Ap3216c::~Ap3216c()
{
    stopReading();
}

QString Ap3216c::alsData() const
{
    return alsdata;
}

QString Ap3216c::irData() const
{
    return irdata;
}

QString Ap3216c::psData() const
{
    return psdata;
}

QString Ap3216c::readAlsData() const
{
    return alsdata;
}

QString Ap3216c::readPsData() const
{
    return psdata;
}

QString Ap3216c::readIrData() const
{
    return irdata;
}

bool Ap3216c::startReading(std::string *errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    if (!config_.enabled) {
        VLOG_DEBUG(MODULE, "%s", "ap3216 input disabled");
        return true;
    }

    if (workerThread_ != nullptr) {
        return true;
    }

    workerThread_ = new QThread(this);
    worker_ = new Ap3216Worker(config_);
    worker_->moveToThread(workerThread_);

    connect(workerThread_, &QThread::started,
            worker_, &Ap3216Worker::start);
    connect(worker_, &Ap3216Worker::availabilityChanged,
            this, &Ap3216c::onWorkerAvailabilityChanged);
    connect(worker_, &Ap3216Worker::readingUpdated,
            this, &Ap3216c::onWorkerReadingUpdated);
    connect(worker_, &Ap3216Worker::backendChanged,
            this, &Ap3216c::onWorkerBackendChanged);
    connect(worker_, &Ap3216Worker::proximityAlertChanged,
            this, &Ap3216c::onWorkerProximityAlertChanged);
    connect(worker_, &Ap3216Worker::warning,
            this, &Ap3216c::onWorkerWarning);

    workerThread_->start();
    return true;
}

void Ap3216c::stopReading()
{
    if (workerThread_ == nullptr) {
        available_ = false;
        return;
    }

    if (worker_ != nullptr && workerThread_->isRunning()) {
        QMetaObject::invokeMethod(worker_, "stop",
                                  Qt::BlockingQueuedConnection);
    }

    workerThread_->quit();
    workerThread_->wait();
    delete worker_;
    worker_ = nullptr;
    delete workerThread_;
    workerThread_ = nullptr;
    available_ = false;
}

bool Ap3216c::isAvailable() const
{
    return available_;
}

void Ap3216c::onWorkerAvailabilityChanged(bool available)
{
    available_ = available;
    emit availabilityChanged(available);
}

void Ap3216c::onWorkerReadingUpdated(QString als, QString ps, QString ir)
{
    alsdata = als;
    psdata = ps;
    irdata = ir;
    emit updateAp3216Data(alsdata, psdata, irdata);
}

void Ap3216c::onWorkerBackendChanged(QString backend)
{
    emit backendChanged(backend);
}

void Ap3216c::onWorkerProximityAlertChanged(bool active, QString source)
{
    emit proximityAlertChanged(active, source);
}

void Ap3216c::onWorkerWarning(QString message)
{
    VLOG_WARN(MODULE, "%s", message.toUtf8().constData());
    emit warning(message);
}
