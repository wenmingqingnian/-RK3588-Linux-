#ifndef AP3216WORKER_H
#define AP3216WORKER_H

#include "ap3216device.h"
#include "vehicledeviceconfig.h"

#include <memory>
#include <QObject>
#include <QString>

class QSocketNotifier;
class QTimer;

class Ap3216Worker : public QObject
{
    Q_OBJECT

public:
    explicit Ap3216Worker(const vehicledevice::Ap3216Config &config,
                          QObject *parent = nullptr);
    ~Ap3216Worker();

public slots:
    void start();
    void stop();

signals:
    void availabilityChanged(bool available);
    void backendChanged(QString backend);
    void readingUpdated(QString als, QString ps, QString ir);
    void proximityAlertChanged(bool active, QString source);
    void warning(QString message);

private slots:
    void onPollTimeout();
    void onEventReady();

private:
    bool ensureBackendOpen();
    void closeBackend();
    void configureEventNotifier();
    void setAvailable(bool available);
    void setAlertState(bool active, const QString &source);
    void updatePollingAlert(int ps, const QString &source);
    void warnOnce(const QString &message);

    vehicledevice::Ap3216Config config_;
    std::unique_ptr<vehicledevice::IAp3216Backend> backend_;
    QTimer *pollTimer_;
    QSocketNotifier *eventNotifier_;
    bool running_;
    bool workerAvailable_;
    bool alertStateKnown_;
    bool alertActive_;
    int consecutiveFailures_;
    QString lastWarning_;
};

#endif // AP3216WORKER_H
