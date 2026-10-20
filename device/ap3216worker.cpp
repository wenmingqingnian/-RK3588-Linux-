#include "ap3216worker.h"

#include <QSocketNotifier>
#include <QTimer>

namespace {
constexpr int kFailureThreshold = 5;
}

Ap3216Worker::Ap3216Worker(const vehicledevice::Ap3216Config &config,
                           QObject *parent)
    : QObject(parent),
      config_(config),
      backend_(vehicledevice::createAp3216Backend(config)),
      pollTimer_(nullptr),
      eventNotifier_(nullptr),
      running_(false),
      workerAvailable_(false),
      alertStateKnown_(false),
      alertActive_(false),
      consecutiveFailures_(0)
{
}

Ap3216Worker::~Ap3216Worker()
{
    stop();
}

void Ap3216Worker::start()
{
    if (running_) {
        return;
    }

    running_ = true;
    emit backendChanged(QString::fromLatin1(backend_->backendName()));

    if (pollTimer_ == nullptr) {
        pollTimer_ = new QTimer(this);
        connect(pollTimer_, &QTimer::timeout,
                this, &Ap3216Worker::onPollTimeout);
    }
    pollTimer_->setInterval(config_.pollIntervalMs);
    pollTimer_->start();

    // Always execute one attempt immediately. Failure does not stop the timer;
    // the worker keeps retrying so iio:deviceN renumbering and driver rebinds
    // recover without restarting the GUI.
    onPollTimeout();
}

void Ap3216Worker::stop()
{
    if (pollTimer_ != nullptr && pollTimer_->isActive()) {
        pollTimer_->stop();
    }

    closeBackend();
    running_ = false;
    consecutiveFailures_ = 0;
    lastWarning_.clear();
    setAvailable(false);
    if (alertStateKnown_) {
        setAlertState(false, QStringLiteral("stopped"));
    }
    alertStateKnown_ = false;
}

bool Ap3216Worker::ensureBackendOpen()
{
    if (backend_->isOpen()) {
        return true;
    }

    std::string errorMessage;
    if (!backend_->open(&errorMessage)) {
        setAvailable(false);
        warnOnce(QStringLiteral("ap3216 %1 open failed: %2")
                     .arg(QString::fromLatin1(backend_->backendName()),
                          QString::fromStdString(errorMessage)));
        return false;
    }

    consecutiveFailures_ = 0;
    alertStateKnown_ = false;
    lastWarning_.clear();
    configureEventNotifier();
    setAvailable(true);
    return true;
}

void Ap3216Worker::closeBackend()
{
    if (eventNotifier_ != nullptr) {
        eventNotifier_->setEnabled(false);
        delete eventNotifier_;
        eventNotifier_ = nullptr;
    }
    backend_->close();
}

void Ap3216Worker::configureEventNotifier()
{
    if (eventNotifier_ != nullptr) {
        delete eventNotifier_;
        eventNotifier_ = nullptr;
    }

    const int fd = backend_->eventFileDescriptor();
    if (fd < 0) {
        return;
    }

    eventNotifier_ = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(eventNotifier_,
            &QSocketNotifier::activated,
            this,
            [this](QSocketDescriptor, QSocketNotifier::Type) {
                onEventReady();
            });
}

void Ap3216Worker::setAvailable(bool available)
{
    if (workerAvailable_ == available) {
        return;
    }
    workerAvailable_ = available;
    emit availabilityChanged(available);
}

void Ap3216Worker::setAlertState(bool active, const QString &source)
{
    if (alertStateKnown_ && alertActive_ == active) {
        return;
    }
    alertStateKnown_ = true;
    alertActive_ = active;
    emit proximityAlertChanged(active, source);
}

void Ap3216Worker::updatePollingAlert(int ps, const QString &source)
{
    if (!alertStateKnown_) {
        setAlertState(ps >= config_.psRisingThreshold, source);
        return;
    }

    if (!alertActive_ && ps >= config_.psRisingThreshold) {
        setAlertState(true, source);
    } else if (alertActive_ && ps <= config_.psFallingThreshold) {
        setAlertState(false, source);
    }
}

void Ap3216Worker::warnOnce(const QString &message)
{
    if (lastWarning_ == message) {
        return;
    }
    lastWarning_ = message;
    emit warning(message);
}

void Ap3216Worker::onPollTimeout()
{
    if (!running_ || !ensureBackendOpen()) {
        return;
    }

    std::string errorMessage;
    const vehicledevice::Ap3216Reading reading =
        backend_->readSensorValues(&errorMessage);

    if (reading.valid) {
        consecutiveFailures_ = 0;
        lastWarning_.clear();
        setAvailable(true);
        emit readingUpdated(QString::number(reading.als),
                            QString::number(reading.ps),
                            QString::number(reading.ir));

        // Establish the initial state from one coherent sample. Afterwards the
        // IIO backend is event-driven; user_i2c keeps using hysteresis polling.
        if (!alertStateKnown_ || backend_->eventFileDescriptor() < 0) {
            const QString source = backend_->eventFileDescriptor() >= 0
                                       ? QStringLiteral("initial_sample")
                                       : QStringLiteral("polling");
            updatePollingAlert(reading.ps, source);
        }
        return;
    }

    ++consecutiveFailures_;
    warnOnce(QStringLiteral("ap3216 %1 read failed: %2")
                 .arg(QString::fromLatin1(backend_->backendName()),
                      QString::fromStdString(errorMessage)));

    if (consecutiveFailures_ >= kFailureThreshold) {
        closeBackend();
        setAvailable(false);
        consecutiveFailures_ = 0;
        alertStateKnown_ = false;
    }
}

void Ap3216Worker::onEventReady()
{
    if (eventNotifier_ == nullptr) {
        return;
    }
    eventNotifier_->setEnabled(false);

    for (;;) {
        vehicledevice::Ap3216Event event;
        std::string errorMessage;
        const int result = backend_->readEvent(&event, &errorMessage);
        if (result == 0) {
            eventNotifier_->setEnabled(true);
            return;
        }
        if (result < 0) {
            warnOnce(QStringLiteral("ap3216 IIO event read failed: %1")
                         .arg(QString::fromStdString(errorMessage)));
            closeBackend();
            setAvailable(false);
            alertStateKnown_ = false;
            return;
        }

        const bool active =
            event.direction == vehicledevice::Ap3216EventDirection::Rising;
        setAlertState(active, QStringLiteral("iio_event"));
    }
}
