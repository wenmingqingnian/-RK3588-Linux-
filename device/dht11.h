#ifndef DHT11_H
#define DHT11_H

#include "vehicledeviceconfig.h"

#include <QObject>
#include <QString>

class Dht11Worker;
class QThread;

// Dht11Reader —— Dht11Worker 的 GUI 线程代理。
//
// startReading() 异步启动 worker 线程，立即返回。
// 启动结果、设备可用性变化、采样数据、告警**全部通过信号**传递：
//   - availabilityChanged(bool)：设备 open 成功/失败、运行中恢复/失效
//   - updateDht11Data(QString, QString)：温湿度采样成功
//   - warning(QString)：采样失败告警
//
// 因此 startReading() 无返回值——真正的 open 结果通过信号异步到达，
// 调用方不应在 startReading() 返回后立即判断 isAvailable()。
class Dht11Reader : public QObject
{
    Q_OBJECT

public:
    explicit Dht11Reader(const vehicledevice::Dht11Config &config,
                         QObject *parent = nullptr);
    ~Dht11Reader();

    // 异步启动 worker 线程。重复调用是 no-op。
    // enabled=false 时仅记日志，不创建线程。
    void startReading();
    void stopReading();
    bool isAvailable() const;

signals:
    void updateDht11Data(QString humidity, QString temperature);
    void availabilityChanged(bool available);
    void warning(QString message);

private slots:
    void onWorkerAvailabilityChanged(bool available);
    void onWorkerWarning(QString message);

private:
    vehicledevice::Dht11Config config_;
    QThread *workerThread_;
    Dht11Worker *worker_;
    bool available_;
};

#endif // DHT11_H
