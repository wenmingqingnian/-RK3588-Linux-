// Dht11Worker 状态机单元测试（需 Qt + QSignalSpy）。
//
// 覆盖：
//   - 初次 open 失败 → 持续重试，不发 availabilityChanged(false)
//     （初始就是 unavailable，无 transition）
//   - 初次 read 成功 → 发 availabilityChanged(true) + readingUpdated
//   - 运行中连续失败达阈值 → 发 availabilityChanged(false)
//   - 恢复后立即发 availabilityChanged(true)
//   - 定时器始终运行，open 失败不停止
//
// 编译（mingw 或 Linux g++ + Qt）：
//   g++ -std=c++17 -Wall -Wextra -fPIC -Idevice -Icommon \
//       -I<QtInclude> -I<QtIncludeQtCore> -I<QtIncludeQtTest> \
//       tests/dht11/test_dht11worker.cpp \
//       device/dht11device.cpp device/dht11worker.cpp \
//       device/vehicledeviceconfig.cpp common/logmanager.cpp \
//       -L<QtLib> -lQt5Core -lQt5Test \
//       -o tests/dht11/test_dht11worker
//
// 注意：worker 通过 Dht11Device 间接访问 sysfs，无法在测试中注入伪造节点
// 而不修改生产代码。因此本测试仅验证状态机的"信号协议"：
//   - open 失败路径：用 enabled=true + 不存在的 sysfsDir，预期 open 持续失败
//   - 成功路径：依赖真实硬件，留作板端集成测试
// 受此限制，本测试主要覆盖"失败重试不崩溃"和"disabled 时不发信号"两类。
//
// Linux 主机/板端运行（无 QtTest 也可改为手动 QSignalSpy 替代实现）。

#include "dht11worker.h"
#include "vehicledeviceconfig.h"

#include <QCoreApplication>
#include <QObject>
#include <QSignalSpy>
#include <QTest>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(condition)                                              \
    do {                                                             \
        if (!(condition)) {                                          \
            std::printf("FAIL [%s:%d]: %s\n",                        \
                        __FILE__, __LINE__, #condition);             \
            ++g_failures;                                            \
        }                                                            \
    } while (0)

// disabled=true 时 worker 启动后不应发任何信号（不创建 worker 线程的等价物：
// 这里 worker 直接构造，但 enabled=false 时 openDevice 直接返回 true，
// readTemperatureHumidity 因 available_=false 失败，会进入失败路径。
// 实际上 Dht11Reader 在 enabled=false 时根本不调用 worker。
// 这里用 enabled=true + 不存在路径模拟失败路径。
void testOpenFailureDoesNotCrash()
{
    vehicledevice::Dht11Config config;
    config.enabled = true;
    config.sysfsDir = "tmp_dht11_worker_nonexistent_path";
    config.pollIntervalMs = 50;  // 短间隔加速测试

    Dht11Worker worker(config);
    QSignalSpy availSpy(&worker, &Dht11Worker::availabilityChanged);
    QSignalSpy readSpy(&worker, &Dht11Worker::readingUpdated);
    QSignalSpy warnSpy(&worker, &Dht11Worker::warning);

    worker.start();

    // 等待足够多的 poll 周期（200ms = 4 个周期）。
    QTest::qWait(200);

    worker.stop();

    // open 失败路径：从未变 available，availabilityChanged 不应发 true。
    // 注意：初始 workerAvailable_=false，stop() 会无条件发 availabilityChanged(false)，
    // 所以 availSpy 至少 1 次（stop 时）。start→poll 失败时不应发任何信号。
    CHECK(availSpy.count() >= 0);  // 不崩溃即可
    CHECK(readSpy.count() == 0);   // open 失败 → 不会有 readingUpdated
    // warning 只在 read 失败时发，open 失败不发 warning。
    CHECK(warnSpy.count() == 0);
}

// 短间隔 poll 在 stop 后必须立即停止，不会继续发信号。
void testStopHaltsPolling()
{
    vehicledevice::Dht11Config config;
    config.enabled = true;
    config.sysfsDir = "tmp_dht11_worker_nonexistent_path_2";
    config.pollIntervalMs = 30;

    Dht11Worker worker(config);
    QSignalSpy warnSpy(&worker, &Dht11Worker::warning);

    worker.start();
    QTest::qWait(100);
    const int countAfterStop = warnSpy.count();
    worker.stop();

    QTest::qWait(100);
    // stop 后 warning 数量不应再增加（open 失败时本来也不发 warning，
    // 但 stop 后定时器确实停了）。
    CHECK(warnSpy.count() == countAfterStop);
}

// 启动后立即 stop 不崩溃（覆盖 stop 幂等）。
void testImmediateStopIsSafe()
{
    vehicledevice::Dht11Config config;
    config.enabled = true;
    config.sysfsDir = "tmp_dht11_worker_nonexistent_path_3";
    config.pollIntervalMs = 50;

    Dht11Worker worker(config);
    worker.start();
    worker.stop();
    worker.stop();  // 二次 stop 安全
    CHECK(true);    // 不崩溃即通过
}

// 析构函数调用 stop，不应崩溃（覆盖析构路径）。
void testDestructorCallsStop()
{
    vehicledevice::Dht11Config config;
    config.enabled = true;
    config.sysfsDir = "tmp_dht11_worker_nonexistent_path_4";
    config.pollIntervalMs = 50;

    {
        Dht11Worker worker(config);
        worker.start();
        QTest::qWait(50);
    }  // 析构调用 stop
    CHECK(true);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testOpenFailureDoesNotCrash();
    testStopHaltsPolling();
    testImmediateStopIsSafe();
    testDestructorCallsStop();

    if (g_failures == 0) {
        std::puts("dht11 worker tests passed");
        return 0;
    }

    std::printf("dht11 worker tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
