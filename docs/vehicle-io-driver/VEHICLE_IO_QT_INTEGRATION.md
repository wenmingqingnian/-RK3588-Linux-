# Vehicle-IO Qt应用层集成

## 1. 集成范围

Vehicle-IO已集成到正式CarTerminal工程：

    <HOME>/RK3588S_CarTerminal

主要文件包括：

- `device/vehicleioinput.h`
- `device/vehicleioinput.cpp`
- `gui/mainwindow.h`
- `gui/mainwindow.cpp`
- `device/vehicledeviceconfig.h`
- `device/vehicledeviceconfig.cpp`
- `config/vehicle_devices.conf`
- `CarTerminal.pro`

冻结副本位于：

    source-snapshot/application/

## 2. VehicleIoInput职责

`VehicleIoInput`承担以下功能：

1. 扫描`/sys/class/input/event*`；
2. 读取各节点的`device/name`；
3. 定位名称为`quarkpi-vehicle-io`的设备；
4. 打开相应`/dev/input/eventX`；
5. 使用`QSocketNotifier`进行事件驱动读取；
6. 解析REVERSE、ACC和BUTTON按键事件；
7. 获取应用启动时的初始按键状态；
8. 向MainWindow发送业务状态变化；
9. 在设备暂时不可用时继续执行自动扫描。

应用不依赖固定event编号。

## 3. REVERSE业务行为

REVERSE有效时调用现有Monitor入口，打开或切换到Monitor界面。

最终行为：

- REVERSE变为ON：打开Monitor；
- REVERSE变为OFF：仅记录释放状态；
- 释放后不自动关闭Monitor；
- 释放后不自动返回主页面。

最终日志包含：

    vehicle-io REVERSE=ON
    REVERSE press opened Monitor page
    vehicle-io REVERSE=OFF

人工界面观察确认释放后Monitor继续保持。

## 4. ACC业务行为

ACC当前只记录状态变化：

    vehicle-io ACC=ON
    vehicle-io ACC=OFF

本阶段没有为ACC附加电源控制、页面切换或其他业务动作。

## 5. BUTTON业务行为

BUTTON按下后执行：

1. 调用`setBuzzerAlarm(true)`；
2. 启动单次定时器；
3. 200 ms后调用`setBuzzerAlarm(false)`。

BUTTON释放事件不重复启动定时器，也不提前终止已经开始的脉冲。

最终日志包含：

    vehicle-io BUTTON=ON
    vehicle-io BUTTON buzzer pulse started: 200 ms
    vehicle-io BUTTON=OFF
    vehicle-io BUTTON buzzer pulse finished

人工验证确认有源蜂鸣器实际短响并自动停止。

## 6. 蜂鸣器正式配置

正式配置内容：

    buzzer_enabled=true
    buzzer_gpio_chip=/dev/gpiochip4
    buzzer_gpio_line=3
    buzzer_active_low=true

对应GPIO4_A3，即J11 Pin 16。

低电平表示鸣叫，高电平表示静音。

## 7. 构建与部署

交叉编译产物：

    build-week7-vehicleio-aarch64/CarTerminal

Buildroot软件包位置：

    buildroot/package/car-terminal-bin/files/CarTerminal

板端正式位置：

    /usr/bin/CarTerminal

最终三处SHA256一致：

    e013f586f9e5a36fd18d1d5cb25d6003ba2231cead788ea47f030fcf17eb2539

## 8. 最终运行状态

冷启动后，正式CarTerminal由启动服务自动运行，并自动连接Vehicle-IO设备。最终专项错误检查未发现Vehicle-IO连接错误、蜂鸣器打开错误、崩溃或断言失败。
