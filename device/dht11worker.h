#ifndef DHT11WORKER_H
#define DHT11WORKER_H

#include "dht11device.h"
#include "vehicledeviceconfig.h"

#include <QObject>
#include <QString>

class QTimer;

// Dht11Worker —— 在 Qt worker 线程中周期性读取 DHT11。
//
// 状态机：
//   - start() 后定时器**始终**运行，无论 openDevice 是否成功。
//   - 每次 onPollTimeout()：若设备未 open 则尝试 open；然后尝试 read。
//   - read 成功：consecutiveFailures 清零，发 readingUpdated。
//     若之前处于 unavailable，发 availabilityChanged(true) 恢复。
//   - read 失败：consecutiveFailures++，发 warning。
//     连续失败 >= kFailureThreshold 时，发 availabilityChanged(false)，
//     关闭设备（下次 timeout 会重新 openDevice 重试），但定时器不停。
//
// 这样保证了：启动时传感器未就绪、运行中瞬态失败、iio:deviceN 编号变化，
// 都能通过定时器持续重试自动恢复，无需重启应用。
class Dht11Worker : public QObject
{
    Q_OBJECT

public:
    explicit Dht11Worker(const vehicledevice::Dht11Config &config,
                         QObject *parent = nullptr);
    ~Dht11Worker();

public slots:
    void start();
    void stop();

signals:
    void availabilityChanged(bool available);
    void readingUpdated(QString humidity, QString temperature);
    void warning(QString message);

private slots:
    void onPollTimeout();

private:
    vehicledevice::Dht11Config config_;
    vehicledevice::Dht11Device device_;
    QTimer *pollTimer_;
    bool running_;
    int consecutiveFailures_;
    bool workerAvailable_;
};

#endif // DHT11WORKER_H
