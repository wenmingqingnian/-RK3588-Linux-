# QuarkPi-CA2 Vehicle-IO内核驱动设计

## 1. 设计目标

Vehicle-IO驱动负责把REVERSE、ACC和BUTTON三路车辆离散输入接入Linux input子系统，并通过标准`/dev/input/eventX`接口提供给用户空间。

驱动不创建自定义字符设备，也不要求应用直接操作GPIO。用户空间只需按照标准input事件格式读取按键状态。

最终input设备名称为：

    quarkpi-vehicle-io

最终板端实测枚举为`/dev/input/event2`。event编号属于动态编号，不作为稳定ABI。

## 2. 硬件映射

| 业务信号 | J11物理引脚 | SoC GPIO | 有效状态 | input键值 |
|---|---:|---|---|---|
| REVERSE | Pin 15 | GPIO4_A2 | 低电平 | KEY_PROG2，数值149 |
| ACC | Pin 18 | GPIO4_A4 | 低电平 | KEY_POWER2，数值356 |
| BUTTON | Pin 22 | GPIO4_A5 | 低电平 | KEY_PROG1，数值148 |

三路输入均报告按下和释放状态。

## 3. 内核源码组成

驱动相关文件：

- `drivers/input/misc/quarkpi-vehicle-io.c`
- `drivers/input/misc/Kconfig`
- `drivers/input/misc/Makefile`

设备树与配置文件：

- `arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts`
- `arch/arm64/configs/rockchip_linux_defconfig`
- `Documentation/devicetree/bindings/input/alientek,quarkpi-vehicle-io.yaml`
- `Documentation/devicetree/bindings/vendor-prefixes.yaml`

冻结副本位于：

    source-snapshot/kernel/

## 4. Kconfig集成

当前内核配置包含：

    CONFIG_INPUT_QUARKPI_VEHICLE_IO=y

因此驱动静态编入内核，不依赖启动后手动加载模块。

## 5. 用户空间接口

应用通过以下接口定位和读取设备：

    /sys/class/input/event*/device/name
    /dev/input/eventX

扫描规则是读取每个event节点的设备名称，并选择名称等于`quarkpi-vehicle-io`的节点。

该方式避免把`/dev/input/event2`硬编码为固定设备路径。

## 6. IRQ验证结果

板端最终实测IRQ：

| 信号 | Linux IRQ | 一次接通并释放的增量 |
|---|---:|---:|
| REVERSE | 149 | 2 |
| ACC | 150 | 2 |
| BUTTON | 151 | 2 |

每次完整操作包含按下和释放两个边沿，因此增量2符合设计预期。

IRQ原始快照和计算结果位于：

    evidence/boardtest/irq-evidence/

## 7. 最终结论

驱动已完成DTS、binding、Kconfig、Makefile和defconfig集成，并已通过probe、input设备枚举、事件读取、IRQ增长以及冷启动恢复验证。
