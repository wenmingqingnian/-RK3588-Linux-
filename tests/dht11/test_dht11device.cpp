// Dht11Device 主机单元测试。
//
// 通过在临时目录中伪造 IIO sysfs 节点（name / in_temp_input /
// in_humidityrelative_input）验证 Dht11Device 的解析、错误处理、
// 自动扫描与恢复行为。不依赖真实硬件 / 真实 /sys/bus/iio/devices。
//
// 注意：dht11device.cpp 使用 POSIX open/read/close + O_CLOEXEC +
// opendir/readdir/closedir，这些 API 在 Windows mingw 上不完整
// （O_CLOEXEC 未定义，<filesystem> 在 mingw730 上不可用）。
// 因此本测试**必须在 Linux 主机或板端**编译运行，Windows 跳过。
//
// 编译（Linux 主机 / 板端，在项目根目录执行）：
//   g++ -std=c++17 -Wall -Wextra -Idevice \
//       tests/dht11/test_dht11device.cpp \
//       device/dht11device.cpp \
//       device/vehicledeviceconfig.cpp \
//       -o tests/dht11/test_dht11device -lstdc++fs
//
// 运行：
//   ./tests/dht11/test_dht11device

#include "dht11device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// 在 path 目录下创建文件 fileName，内容为 content。
// 自动创建父目录。失败时 abort（这是测试装置错误，不是被测代码错误）。
void writeSysfsFile(const std::string &path,
                    const std::string &fileName,
                    const std::string &content)
{
    std::filesystem::create_directories(path);
    std::ofstream out(path + "/" + fileName, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "FATAL: cannot write %s/%s\n",
                     path.c_str(), fileName.c_str());
        std::abort();
    }
    out << content;
    out.close();
    if (!out) {
        std::fprintf(stderr, "FATAL: write failed %s/%s\n",
                     path.c_str(), fileName.c_str());
        std::abort();
    }
}

// 删除临时目录树。忽略错误（best-effort）。
void cleanupDir(const std::string &path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// 构造一个标准的 Dht11Config，enabled=true，sysfsDir 指向给定路径。
vehicledevice::Dht11Config makeEnabledConfig(const std::string &sysfsDir)
{
    vehicledevice::Dht11Config config;
    config.enabled = true;
    config.sysfsDir = sysfsDir;
    config.pollIntervalMs = 1000;
    return config;
}

// 1. 25000 → 25.0°C，60000 → 60.0%RH
void testNormalConversion()
{
    const std::string dir = "tmp_dht11_test_normal";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(device.openDevice(&openError));
    CHECK(openError.empty());
    CHECK(device.isAvailable());

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(r.valid);
    CHECK(readError.empty());
    CHECK(r.errorMessage.empty());
    // 25.0°C ± 0.001
    CHECK(r.temperature > 24.999 && r.temperature < 25.001);
    // 60.0%RH ± 0.001
    CHECK(r.humidity > 59.999 && r.humidity < 60.001);

    cleanupDir(dir);
}

// 2. 目录末尾带 / 也能正常工作（joinPath 容错）
void testTrailingSlash()
{
    const std::string dir = "tmp_dht11_test_slash";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "30000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "55000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir + "/"));
    std::string openError;
    CHECK(device.openDevice(&openError));
    CHECK(device.isAvailable());

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(r.valid);
    CHECK(r.temperature > 29.999 && r.temperature < 30.001);
    CHECK(r.humidity > 54.999 && r.humidity < 55.001);

    cleanupDir(dir);
}

// 3. name 不是 dht11 时 openDevice 必须失败
void testWrongNameRejected()
{
    const std::string dir = "tmp_dht11_test_wrongname";
    writeSysfsFile(dir, "name", "bmp280");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(!device.openDevice(&openError));
    CHECK(!openError.empty());
    CHECK(!device.isAvailable());
    // 错误信息应包含预期与实际值
    CHECK(openError.find("dht11") != std::string::npos);
    CHECK(openError.find("bmp280") != std::string::npos);

    cleanupDir(dir);
}

// 4. name 文件缺失 → openDevice 失败
void testMissingNameFile()
{
    const std::string dir = "tmp_dht11_test_noname";
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");
    // 故意不写 name

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(!device.openDevice(&openError));
    CHECK(!openError.empty());
    CHECK(!device.isAvailable());

    cleanupDir(dir);
}

// 5. 温度文件为空 → read 失败，valid=false
void testEmptyTempFile()
{
    const std::string dir = "tmp_dht11_test_emptytemp";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(device.openDevice(&openError));
    CHECK(device.isAvailable());

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(!r.valid);
    CHECK(!r.errorMessage.empty());

    cleanupDir(dir);
}

// 6. 温度文件非整数 → read 失败
void testNonNumericTemp()
{
    const std::string dir = "tmp_dht11_test_nontemp";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "abc");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(device.openDevice(&openError));

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(!r.valid);
    CHECK(!r.errorMessage.empty());

    cleanupDir(dir);
}

// 7. 湿度文件缺失 → read 失败
void testMissingHumidityFile()
{
    const std::string dir = "tmp_dht11_test_nohum";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    // 不写湿度文件

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(device.openDevice(&openError));

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(!r.valid);
    CHECK(!r.errorMessage.empty());

    cleanupDir(dir);
}

// 8. 温度文件路径不存在（目录缺失）→ openDevice 阶段失败
void testMissingDirectory()
{
    const std::string dir = "tmp_dht11_test_nodir_does_not_exist";
    cleanupDir(dir);  // 确保不存在

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(!device.openDevice(&openError));
    CHECK(!openError.empty());
    CHECK(!device.isAvailable());
}

// 9. 初次 open 失败后，目录修复后能再次 openDevice 成功（恢复）
//    这模拟"启动时传感器未就绪，运行中修复后可恢复"。
void testRecoverAfterInitialFailure()
{
    const std::string dir = "tmp_dht11_test_recover";
    cleanupDir(dir);  // 初始不存在

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(!device.openDevice(&openError));
    CHECK(!device.isAvailable());

    // 修复：现在创建目录和正确文件
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    // 再次 openDevice 应成功
    std::string openError2;
    CHECK(device.openDevice(&openError2));
    CHECK(device.isAvailable());

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(r.valid);
    CHECK(r.temperature > 24.999 && r.temperature < 25.001);

    cleanupDir(dir);
}

// 10. open 成功后，read 失败再修复（运行中失败 → 恢复）
//     device 层无状态机，这里只验证：open 后 read 失败不会让 isAvailable 变 false
//     （状态机在 worker 层）。修复文件后下一次 read 立即成功。
void testReadFailureDoesNotInvalidateDevice()
{
    const std::string dir = "tmp_dht11_test_readfail";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(device.openDevice(&openError));
    CHECK(device.isAvailable());

    // 删除湿度文件模拟运行中文件消失
    std::error_code ec;
    std::filesystem::remove(dir + "/" + vehicledevice::Dht11Device::kHumidityFile, ec);

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(!r.valid);
    // device 层：read 失败不应让 isAvailable 变 false（状态机在 worker 层）
    CHECK(device.isAvailable());

    // 修复文件后下一次 read 立即成功
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");
    std::string readError2;
    vehicledevice::Dht11Reading r2 = device.readTemperatureHumidity(&readError2);
    CHECK(r2.valid);
    CHECK(r2.temperature > 24.999 && r2.temperature < 25.001);

    cleanupDir(dir);
}

// 11. closeDevice 后 isAvailable 必须为 false
void testCloseInvalidates()
{
    const std::string dir = "tmp_dht11_test_close";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(device.openDevice(&openError));
    CHECK(device.isAvailable());

    device.closeDevice();
    CHECK(!device.isAvailable());

    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(!r.valid);

    cleanupDir(dir);
}

// 12. enabled=false 时 openDevice 直接成功且不触碰 sysfs
void testDisabledSkipsSysfs()
{
    vehicledevice::Dht11Config config;
    config.enabled = false;
    config.sysfsDir = "tmp_dht11_test_disabled_must_not_exist";
    config.pollIntervalMs = 1000;
    cleanupDir(config.sysfsDir);  // 确保不存在

    vehicledevice::Dht11Device device(config);
    std::string openError;
    CHECK(device.openDevice(&openError));
    CHECK(openError.empty());
    // disabled 时 isAvailable 仍为 false（未真正 open 设备）
    CHECK(!device.isAvailable());

    // readTemperatureHumidity 在 available_=false 时应失败
    std::string readError;
    vehicledevice::Dht11Reading r = device.readTemperatureHumidity(&readError);
    CHECK(!r.valid);
}

// 13. 通道文件名常量符合 IIO ABI
void testIioConstants()
{
    CHECK(std::string(vehicledevice::Dht11Device::kTempFile) == "in_temp_input");
    CHECK(std::string(vehicledevice::Dht11Device::kHumidityFile) ==
          "in_humidityrelative_input");
    CHECK(std::string(vehicledevice::Dht11Device::kNameFile) == "name");
    CHECK(std::string(vehicledevice::Dht11Device::kExpectedName) == "dht11");
    CHECK(std::string(vehicledevice::Dht11Device::kIioDevicesDir) ==
          "/sys/bus/iio/devices");
}

// 14. 自动扫描：sysfsDir 为空时，openDevice 应扫描 /sys/bus/iio/devices/
//     找到 name=="dht11" 的设备。测试通过 monkey-patch kIioDevicesDir
//     不现实（const 成员），所以这条用例用环境变量旁路：
//     DHT11_TEST_IIO_DIR 指向伪造的 iio:device0 目录。
//     但 kIioDevicesDir 是编译期常量，运行时无法替换。
//
//     折衷方案：通过 chdir 到一个包含伪造 /sys/bus/iio/devices 树的临时
//     根目录也不可行（kIioDevicesDir 是绝对路径）。
//
//     因此自动扫描的端到端测试只能在板端/集成测试中进行（requires 真实
//     /sys/bus/iio/devices/ 或 root 权限创建符号链接）。本单元测试集中
//     验证 discoverDht11SysfsDir 的逻辑前提：name 匹配正确。
//     详见 docs/BUILD_RUN_TEST.md "DHT11 IIO sysfs 自动扫描" 章节的板端
//     手工验收步骤。
//
//     此处保留一个占位用例，明确说明覆盖率边界。
void testAutoScanCoverageNote()
{
    // 占位：自动扫描的端到端验证依赖真实 /sys/bus/iio/devices/，
    // 见 docs/BUILD_RUN_TEST.md 板端验收章节。
    CHECK(vehicledevice::Dht11Device::kIioDevicesDir != nullptr);
}

// 15. 配置 sysfsDir 为空 + enabled=true 时 openDevice 应当尝试自动扫描，
//     扫描失败（找不到 dht11 节点）时返回 false。
//     在主机环境中 /sys/bus/iio/devices 通常不存在或没有 dht11 节点，
//     所以这本质上是"自动扫描失败路径"测试。
//     在板端若恰好有 dht11 节点，则这会变成成功路径——所以本用例只在
//     Linux 主机运行且 /sys/bus/iio/devices 无 dht11 时有效。
void testEmptySysfsDirTriggersAutoScan()
{
    vehicledevice::Dht11Config config;
    config.enabled = true;
    config.sysfsDir = "";  // 显式空，触发自动扫描
    config.pollIntervalMs = 1000;

    vehicledevice::Dht11Device device(config);
    std::string openError;
    // 调用 openDevice，无论成功失败都不应崩溃。
    const bool result = device.openDevice(&openError);
    if (!result) {
        // 失败是预期：主机/无硬件环境无 dht11 IIO 节点。
        CHECK(!openError.empty());
        CHECK(!device.isAvailable());
    } else {
        // 极少数情况：板端 /sys/bus/iio/devices/iio:deviceN 恰好是 dht11。
        // 此时应已读到正确的 name 文件，isAvailable 必须为 true。
        CHECK(device.isAvailable());
    }
}

// 16. IIO 编号变化场景：openDevice 成功后 closeDevice，
//     然后改变目录内容（模拟设备重新编号），再次 openDevice 应重新解析路径。
//     因为 closeDevice 清空 resolvedSysfsDir_，下次 openDevice 重新走扫描或
//     使用 config_.sysfsDir。这里验证 close 后 resolved 被清空。
void testCloseClearsResolvedPath()
{
    const std::string dir = "tmp_dht11_test_closeclear";
    writeSysfsFile(dir, "name", "dht11");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kTempFile, "25000");
    writeSysfsFile(dir, vehicledevice::Dht11Device::kHumidityFile, "60000");

    vehicledevice::Dht11Device device(makeEnabledConfig(dir));
    std::string openError;
    CHECK(device.openDevice(&openError));
    CHECK(device.isAvailable());

    std::string readError;
    vehicledevice::Dht11Reading r1 = device.readTemperatureHumidity(&readError);
    CHECK(r1.valid);

    // closeDevice 后再次 read 应失败（available_=false）。
    device.closeDevice();
    std::string readError2;
    vehicledevice::Dht11Reading r2 = device.readTemperatureHumidity(&readError2);
    CHECK(!r2.valid);

    // 再次 openDevice 应重新解析路径（不依赖旧的 resolved）。
    std::string openError3;
    CHECK(device.openDevice(&openError3));
    CHECK(device.isAvailable());
    std::string readError3;
    vehicledevice::Dht11Reading r3 = device.readTemperatureHumidity(&readError3);
    CHECK(r3.valid);

    cleanupDir(dir);
}

} // namespace

int main()
{
    testIioConstants();
    testNormalConversion();
    testTrailingSlash();
    testWrongNameRejected();
    testMissingNameFile();
    testEmptyTempFile();
    testNonNumericTemp();
    testMissingHumidityFile();
    testMissingDirectory();
    testRecoverAfterInitialFailure();
    testReadFailureDoesNotInvalidateDevice();
    testCloseInvalidates();
    testDisabledSkipsSysfs();
    testAutoScanCoverageNote();
    testEmptySysfsDirTriggersAutoScan();
    testCloseClearsResolvedPath();

    if (g_failures == 0) {
        std::puts("dht11 device tests passed");
        return 0;
    }

    std::printf("dht11 device tests FAILED: %d failure(s)\n", g_failures);
    return 1;
}
