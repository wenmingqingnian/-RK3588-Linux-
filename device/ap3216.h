/******************************************************************
Copyright © Deng Zhimao Co., Ltd. 1990-2030. All rights reserved.
* @projectName   sensor
* @brief         ap3216c.h
 * @author        Deng Zhimao
 * @date          2020-07-10
*******************************************************************/
#ifndef AP3216C_H
#define AP3216C_H

#include "vehicledeviceconfig.h"

#include <QObject>
#include <QString>
#include <string>

class Ap3216Worker;
class QThread;

class Ap3216c : public QObject
{
    Q_OBJECT

public:
    explicit Ap3216c(QObject *parent = nullptr);
    explicit Ap3216c(const vehicledevice::Ap3216Config &config,
                     QObject *parent = nullptr);
    ~Ap3216c();

    QString alsData() const;
    QString psData() const;
    QString irData() const;

    QString readAlsData() const;
    QString readPsData() const;
    QString readIrData() const;

    bool startReading(std::string *errorMessage);
    void stopReading();
    bool isAvailable() const;

signals:
    void updateAp3216Data(QString als, QString ps, QString ir);
    void availabilityChanged(bool available);
    void backendChanged(QString backend);
    void proximityAlertChanged(bool active, QString source);
    void warning(QString message);

private slots:
    void onWorkerAvailabilityChanged(bool available);
    void onWorkerReadingUpdated(QString als, QString ps, QString ir);
    void onWorkerBackendChanged(QString backend);
    void onWorkerProximityAlertChanged(bool active, QString source);
    void onWorkerWarning(QString message);

private:
    vehicledevice::Ap3216Config config_;
    QThread *workerThread_;
    Ap3216Worker *worker_;
    bool available_;
    QString alsdata;
    QString psdata;
    QString irdata;
};

#endif // AP3216C_H
