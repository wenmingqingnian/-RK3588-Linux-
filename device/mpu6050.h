#ifndef MPU6050_H
#define MPU6050_H

#include "vehicledeviceconfig.h"

#include <QObject>
#include <QString>

class Mpu6050Worker;
class QThread;

// Mpu6050Reader —— Mpu6050Worker 的 GUI 线程代理。
//
// 与 Dht11Reader / Ap3216c 同构：startReading() 异步启动 worker 线程，
// 启动结果、设备可用性变化、采样数据、告警**全部通过信号**传递：
//   - availabilityChanged(bool)：设备 open 成功/失败、运行中恢复/失效
//   - readingUpdated(9 个 double)：采样成功，含 accel/gyro/temp/pitch/roll
//   - warning(QString)：采样失败告警
//
// 信号在 GUI 线程触发，直接 connect 到 UI 控件更新或日志即可。
class Mpu6050Reader : public QObject
{
    Q_OBJECT

public:
    explicit Mpu6050Reader(const vehicledevice::Mpu6050Config &config,
                            QObject *parent = nullptr);
    ~Mpu6050Reader();

    // 异步启动 worker 线程。重复调用是 no-op。
    // enabled=false 时仅记日志，不创建线程。
    void startReading();
    void stopReading();
    bool isAvailable() const;

signals:
    void updateMpu6050Data(double ax, double ay, double az,
                            double gx, double gy, double gz,
                            double temperature,
                            double pitch, double roll);
    void availabilityChanged(bool available);
    void warning(QString message);

private slots:
    void onWorkerAvailabilityChanged(bool available);
    void onWorkerWarning(QString message);

private:
    vehicledevice::Mpu6050Config config_;
    QThread *workerThread_;
    Mpu6050Worker *worker_;
    bool available_;
};

#endif // MPU6050_H
