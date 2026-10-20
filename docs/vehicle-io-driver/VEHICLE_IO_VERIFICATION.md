# Vehicle-IO最终验证记录

## 1. 验收范围

本次最终验收覆盖：

- 自研Vehicle-IO内核input驱动；
- REVERSE、ACC和BUTTON三路GPIO输入；
- DTS、binding、Kconfig、Makefile和defconfig；
- Qt对`/dev/input/eventX`的自动扫描和事件读取；
- MainWindow业务行为；
- BUTTON触发200 ms蜂鸣器；
- IRQ计数；
- 正式二进制持久化；
- Buildroot软件包同步；
- 冷启动自动恢复；
- 上电阶段蜂鸣器静音；
- 最终错误检查。

## 2. 编译与二进制一致性

最终程序为ARM64 ELF动态链接可执行文件。

最终SHA256：

    e013f586f9e5a36fd18d1d5cb25d6003ba2231cead788ea47f030fcf17eb2539

已核对以下位置完全一致：

1. `<HOME>/RK3588S_CarTerminal/build-week7-vehicleio-aarch64/CarTerminal`
2. `<HOME>/quarkpi-sdk-clean/buildroot/package/car-terminal-bin/files/CarTerminal`
3. 板端`/usr/bin/CarTerminal`

证据文件：

    evidence/final-binary-sha256.txt

## 3. IRQ验证

| 信号 | IRQ | 操作前 | 操作后 | 增量 |
|---|---:|---:|---:|---:|
| REVERSE | 149 | 6 | 8 | 2 |
| ACC | 150 | 7 | 9 | 2 |
| BUTTON | 151 | 6 | 8 | 2 |

一次接通并释放包含两个边沿，因此三路增量2均符合预期。

IRQ 153在测试过程中存在持续增长，该IRQ属于MPU6500，不属于Vehicle-IO验收对象。

## 4. 正式程序持久化

正式程序已安装到：

    /usr/bin/CarTerminal

正式配置已安装到：

    /usr/share/carterminal/config/vehicle_devices.conf

正式服务启动后验证：

    PROCESS_ALIVE=YES
    EXE=/usr/bin/CarTerminal
    EVENT_DEVICE=/dev/input/event2
    /proc/<pid>/fd/<fd> -> /dev/input/event2

## 5. 冷启动验证

重启前后Boot ID不同：

    BOOT_ID_CHANGED=YES

证明验证发生在新的系统启动周期。

重启后：

- 正式CarTerminal自动运行；
- 二进制SHA256保持正确；
- Vehicle-IO重新枚举为`/dev/input/event2`；
- CarTerminal重新打开该input设备；
- GPIO4_A3恢复为输出高电平；
- 启动日志显示蜂鸣器和Vehicle-IO初始化完成。

冷启动证据：

    evidence/boardtest/week7_vehicleio_cold_boot_result.log

## 6. 冷启动后端到端业务验证

最终业务日志：

    vehicle-io BUTTON=ON
    vehicle-io BUTTON buzzer pulse started: 200 ms
    vehicle-io BUTTON=OFF
    vehicle-io BUTTON buzzer pulse finished

    vehicle-io ACC=ON
    vehicle-io ACC=OFF

    vehicle-io REVERSE=ON
    REVERSE press opened Monitor page
    vehicle-io REVERSE=OFF

人工确认：

- BUTTON实际使蜂鸣器短响并自动停止；
- REVERSE使Monitor打开；
- REVERSE释放后Monitor不自动退出；
- ACC只记录状态变化。

## 7. 上电蜂鸣器静音验证

GPIO4_A3静音状态：

    gpio-131 (...) out hi

在蜂鸣器SIG与3.3 V之间增加10 kΩ上拉后，人工观察确认从接通电源或重启到主页面出现，蜂鸣器全程不误响。

## 8. 错误检查

最终专项错误检查为空，未发现：

- `vehicle-io error`
- `buzzer open failed`
- `setBuzzerAlarm`失败
- segmentation fault
- core dump
- assertion failure

应用日志中存在的ASR、TTS或MPP能力警告不属于Vehicle-IO范围，且未阻止Vehicle-IO、主窗口或蜂鸣器工作。

## 9. 最终结论

第7周Vehicle-IO内核驱动、Qt集成、业务行为、蜂鸣器控制、正式部署、Buildroot同步以及冷启动回归均已通过。

完整证据位于：

    evidence/
