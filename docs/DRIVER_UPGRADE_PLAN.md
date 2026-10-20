# 驱动开发能力提升路径（用户态 → 内核驱动）

> 记录当前所有外设的驱动实现方式、用户态 vs 内核驱动的对比、
> 以及各驱动在新 12 周计划（见 [PROJECT_ROADMAP.md](./PROJECT_ROADMAP.md)）中的对应位置。
>
> 创建：2026-07-21
> 重写：2026-07-21（修正 i2c_driver 被错称为 Platform Driver 的概念性错误；
>      原内容已并入新 12 周计划，本文档作为"驱动开发能力提升路径"附录保留）

---

## 一、关键概念澄清：i2c_driver ≠ platform_driver

> **本文档此前版本存在的概念性错误**：将所有内核驱动统称为 "Platform Driver"，
> 并把 `i2c_driver` 当作 `platform_driver` 的一种。**这是错误的**。

### 总线类型对照表

| 驱动类型 | 注册到的总线 | 注册 API | 匹配机制 | 典型设备 |
|---------|------------|---------|---------|---------|
| `i2c_driver` | `i2c_bus_type` | `module_i2c_driver()` / `i2c_add_driver()` | `of_match_table` + I²C `reg` 属性 | MPU6050、AP3216C、ES8323、IMX219 |
| `platform_driver` | `platform_bus_type` | `module_platform_driver()` / `platform_driver_register()` | `of_match_table` 或 platform_device name | Vehicle-IO、gpio-keys、SoC 内部 IP |
| `spi_driver` | `spi_bus_type` | `module_spi_driver()` | `of_match_table` + SPI `reg` 属性 | SPI Flash、SPI 屏 |
| `usb_driver` | `usb_bus_type` | `usb_register()` | USB ID Table | USB 设备 |
| `pci_driver` | `pci_bus_type` | `pci_register_driver()` | PCI ID | PCIe 设备 |

### 关键区别

- **`i2c_driver` 和 `platform_driver` 是平行的总线模型**，不能互相包含或混用。
- I²C 设备的内核驱动注册 `i2c_driver`，通过 `i2c_client` 访问硬件。
- SoC 内部 IP（如 GPIO 控制器、ISP、CIF）和无总线挂载的板级设备注册 `platform_driver`，通过 `platform_device` 访问硬件。
- 两者都使用 `of_match_table` 做设备树匹配，但匹配的 bus 不同。

### 本项目各驱动的正确总线归属

| 驱动 | 总线类型 | 驱动类型 | 注册方式 |
|------|---------|---------|---------|
| MPU6050 | I²C | `i2c_driver`（主线 `inv_mpu6050`） | `module_i2c_driver()` |
| AP3216C | I²C | `i2c_driver`（自研） | `module_i2c_driver()` |
| Vehicle-IO | platform_bus | `platform_driver`（自研） | `module_platform_driver()` |
| 第二种 Sensor | I²C | `i2c_driver`（自研/移植） | `module_i2c_driver()` |
| 蜂鸣器 | - | 用户态 libgpiod 或 PWM/LED 框架（不写私有字符设备） | - |
| GPS | UART | 用户态 termios（不写内核驱动） | - |

---

## 二、当前外设驱动实现方式盘点

| 设备 | 总线/接口 | 当前实现方式 | 代码位置 | 设备树 |
|---|---|---|---|---|
| GPS（ATGM336H） | UART5 | 用户态 `/dev/ttyS5` + termios | `device/gpsworker.cpp` | dtbo 合并 `rk3588s-uart5-m1.dtbo` |
| IMX415 摄像头 | I²C控制 + MIPI-CSI数据 | BSP内核Sensor驱动；应用通过 `/dev/video33` + V4L2 ioctl 采集 | `media/capture_thread.cpp`（应用）/ BSP Sensor驱动（内核） | BSP 默认启用 |
| 蜂鸣器 | GPIO4_A3 | 用户态 libgpiod `/dev/gpiochip4` | `device/buzzerdevice.cpp` | 无需 |
| AP3216C（光感/距离） | I2C | 用户态适配已完成，真实硬件与内核驱动未验收 | `device/ap3216device.cpp` | 当前无Sensor子节点；I²C控制器仍须使能 |
| DHT11（温湿度） | GPIO | 应用IIO适配完成；主线 `dht11.c` 与真实IIO节点待自编译镜像验证 | `drivers/iio/humidity/dht11.c` | 需加经binding核实的子节点 |
| **MPU6050/MPU9250**（IMU） | I2C3 | 用户态 `/dev/i2c-3` + `i2c_smbus_*`；真实WHO_AM_I须先确认 | `device/mpu6050device.cpp` | dtbo 合并 `rk3588s-i2c3-m1.dtbo` |
| ES8323 音频 | I2S + I2C | 内核 ALSA SoC（BSP 提供） | `sound/soc/rockchip/` | BSP 默认启用 |
| USB 麦克风 | USB | 内核 USB Audio（主线） | `sound/usb/` | 自动枚举 |

**结论**：当前自研外设访问主要位于用户态；IMX415/DHT11等内核能力来自 BSP 或主线。新计划只把适合标准内核子系统管理的设备升级为内核驱动，并保留用户态实现用于回退和 A/B 对照。

---

## 三、用户态 I2C 驱动 vs 内核 I2C 驱动 完整对比

### 3.1 概念

**用户态 I2C 驱动**
- 代码运行在**用户空间**（应用进程）
- 通过 `/dev/i2c-N` + `i2c_smbus_*` / `ioctl(I2C_SLAVE)` API 访问硬件
- 内核只需提供通用 `i2c-dev` 服务（导出 `/dev/i2c-N`），不感知挂了什么芯片

**内核 I2C 驱动（`i2c_driver`）**
- 代码运行在**内核空间**（kernel module 或编进 zImage）
- 通过 `i2c_driver` + `probe()` 注册到 **I2C 子系统**（不是 platform_bus）
- Device Tree/ACPI/板级信息先由 I²C core 实例化 `i2c_client`，随后总线按 compatible/ID 匹配 `i2c_driver` 并调用 probe
- 通常对接 IIO / Input / V4L2 / ALSA 等高层子系统，向用户态提供标准 ABI

### 3.2 代码形态对比

**用户态（当前 `device/mpu6050device.cpp`）**：
```cpp
int fd = ::open("/dev/i2c-3", O_RDWR);
::ioctl(fd, I2C_SLAVE, 0x68);
__s32 val = i2c_smbus_read_byte_data(fd, 0x75);   // WHO_AM_I
i2c_smbus_write_byte_data(fd, 0x6B, 0x01);         // 唤醒
double ax = raw * 9.80665 / 8192.0;
```

**内核态（基于 `i2c_driver` + IIO，对应 `drivers/iio/imu/inv_mpu6050/`）**：
```c
static const struct of_device_id mpu6050_of_match[] = {
    { .compatible = "invensense,mpu6050" },
    { }
};
MODULE_DEVICE_TABLE(of, mpu6050_of_match);

static struct i2c_driver mpu6050_driver = {
    .probe_new = mpu6050_probe,
    .remove    = mpu6050_remove,
    .driver = {
        .of_match_table = mpu6050_of_match,
        .name = "mpu6050",
        .pm = &mpu6050_pm_ops,
    },
};
module_i2c_driver(mpu6050_driver);   /* 注意：是 i2c_driver，不是 platform_driver */

static int mpu6050_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    i2c_smbus_read_byte_data(client, 0x75);           /* WHO_AM_I */
    i2c_smbus_write_byte_data(client, 0x6B, 0x01);     /* 唤醒 */
    devm_iio_device_register(&client->dev, indio_dev); /* 对接 IIO 子系统 */
}
```

用户读取（IIO 标准 ABI）：
```bash
cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw
```

### 3.3 优劣势对比表

| 维度 | 用户态 I2C 驱动 | 内核 I2C 驱动（`i2c_driver` + IIO） |
|---|---|---|
| 开发难度 | 低（普通 C++ + glibc） | 高（内核 API + kobject + 引用计数 + 并发锁） |
| 调试方式 | printf / GDB / valgrind | printk / KGDB / ftrace / crash dump |
| 崩溃影响 | 应用 segfault 进程挂 | kernel panic 整机挂 |
| 修改后生效 | 重新编译应用即可 | 重新编译 ko + rmmod + insmod（或重启） |
| 跨硬件可移植 | 依赖 `/dev/i2c-N`、权限和应用协议实现 | 标准驱动通常可跨SoC复用，但需适配目标内核API、DT和硬件资源 |
| 性能（单次读写） | 多一次系统调用/数据复制，但通常仍由I²C线速主导 | 可减少用户/内核往返并集中批量采样；是否更快必须实测 |
| 性能（连续采样） | 取决于I²C频率、Sensor ODR、批量读取和调度 | 同样受总线/ODR限制；IIO Buffer、IRQ和时间戳有利于降低抖动与CPU开销 |
| 中断支持 | 可通过GPIO character-device line event等方式接收，但需应用自行组织 | 可自然接入IRQ、threaded IRQ、IIO Trigger/Event等框架 |
| DMA 支持 | 不能直接决定底层控制器DMA策略 | DMA通常属于底层I²C控制器能力，不是Sensor Driver自动获得 |
| 电源管理 | 不参与 PM | 参与 PM runtime suspend/resume |
| 设备树集成 | 只需总线 status=okay | 需加子节点 + compatible |
| 多进程访问 | 需自行制定所有权和锁策略 | 驱动可集中仲裁，但业务语义、缓存和并发策略仍需正确实现 |
| 标准化数据接口 | 无（应用自定义） | sysfs / char device / input / IIO |
| udev 自动管理 | 不支持 | 支持 |
| 简历价值（嵌入式岗位） | 中（展示 I2C 协议理解） | 高（展示内核驱动开发能力） |

### 3.4 选型标准

**选用户态 I2C 访问**：
- 原型开发 / 快速验证硬件
- 采样频率低（50~200 Hz）
- 不需要中断（轮询足够）
- 应用专属外设（只有这个程序用）
- 临时性 / 项目内驱动（不提交 mainline）

**选内核 I2C 驱动（`i2c_driver` + IIO/Input/V4L2）**：
- 需要标准 ABI、连续 Buffer、统一时间戳或更可控的采样抖动
- 需要中断驱动（事件触发）
- 多进程共享设备
- 想提交内核 mainline
- 需要 udev 自动管理设备节点
- 简历需要展示内核驱动开发能力

---

## 四、驱动开发能力提升路径（对应新 12 周计划）

> 原文档第三节的"优先级 P1/P2/P3/P4"升级路线**已全部并入新 12 周计划**。
> 本节仅给出对应关系，详细任务分解见 [PROJECT_ROADMAP.md §四](./PROJECT_ROADMAP.md)。

### P1：MPU6050 → 内核 I2C 驱动 + IIO（适配主线）

- **总线类型**：I²C（`i2c_driver`，**不是** platform_driver）
- **驱动类型**：主线适配（`CONFIG_INV_MPU6050_IIO`）
- **对应周次**：第4周
- **简历价值**：最高。MPU6050 主线已有参考实现 `drivers/iio/imu/inv_mpu6050/`，可对照学习。

**升级步骤**（详见 PROJECT_ROADMAP.md §四第4周）：
1. 内核配置 `CONFIG_INV_MPU6050_IIO=m`
2. 读取真实 WHO_AM_I，区分 MPU6050/MPU9250，再按当前内核 binding 编写 I²C 子节点 + INT GPIO
3. pinctrl + regulator 描述
4. probe() + WHO_AM_I 调试
5. IIO channel + Data Ready IRQ + IIO trigger
6. IIO triggered buffer + timestamp
7. suspend/resume
8. 应用层：引入 `IImuSampleSource`，A/B 对比用户态I²C与 `/dev/iio:deviceX` Buffer；sysfs只用于配置和低频诊断

**验收标准**：
- `cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw` 输出有效数据
- `iio_info` / `iio_generic_buffer` 工具能采样
- 内核 log 无报错
- A/B 对比数据表（采样率、抖动、CPU 占用）

### P2：AP3216C → 独立实现内核 I2C 驱动 + IIO

- **总线类型**：I²C（`i2c_driver`，**不是** platform_driver）
- **驱动类型**：根据 datasheet 独立实现；允许参考同类 IIO 驱动的框架，但不得把第三方代码冒充自研
- **对应周次**：第5-6周
- **简历价值**：高。核心价值来自独立完成寄存器语义、IIO ABI、错误路径和板端验证，而不是简单宣称“主线无参考”。

**升级步骤**（详见 PROJECT_ROADMAP.md §四第5-6周）：
1. 编写 `drivers/iio/light/ap3216c.c`
   - `i2c_driver` 注册（`module_i2c_driver`）
   - `probe` 里用 regmap 初始化寄存器
   - `iio_info` + `iio_chan_spec` 定义 sysfs 通道
   - `read_raw` 回调里读 ALS / PS 数据
2. Kconfig / Makefile 加配置项
3. 设备树使用经厂商前缀核实的 compatible（例如 `liteon,ap3216c`）
4. Device Tree binding 文档（例如 `liteon,ap3216c.yaml`）
5. 应用层改为读 sysfs
6. 增强项（选 2 项）：Runtime PM、GPIO 中断 + IIO Event

> 不预设 AP3216C 一定存在 chip ID 寄存器。先核对 datasheet；若没有，使用复位默认值或受控写入-读回-恢复验证。驱动绑定时禁止用 `i2cdump` 与驱动并发访问。

**验收标准**：
- `cat /sys/bus/iio/devices/iio:deviceN/in_illuminance_raw` 输出光照值
- `cat /sys/.../in_proximity_raw` 输出距离值
- 30 分钟稳定采集
- Qt 环境监测页面接入

### P3：Vehicle-IO → 自研 platform_driver + Input（全自研）

- **总线类型**：platform_bus（`platform_driver`，**这才是** platform_driver）
- **驱动类型**：**全自研**
- **对应周次**：第7周
- **简历价值**：高。展示 platform_driver + GPIO IRQ + Input 子系统 + 软件消抖能力。

**升级步骤**（详见 PROJECT_ROADMAP.md §四第7周）：
1. 编写 `drivers/input/misc/quarkpi-vehicle-io.c`
   - `platform_driver` 注册（`module_platform_driver`）
   - DTS 节点 + `of_match_table`
   - `devm_gpiod_get()` 获取 GPIO
   - `gpiod_to_irq()` 转中断号
   - `threaded_irq` 注册
   - `delayed_work` 软件消抖
   - Input subsystem 注册 `input_device`
   - 按语义上报事件：瞬时按键用 `input_report_key()`；持续状态只在有合理标准 code 时用 `input_report_switch()`
2. Kconfig + Makefile
3. 设备树使用合法厂商前缀并编写 binding（如 `alientek,quarkpi-vehicle-io.yaml`）
4. 应用层：Qt 监听 `/dev/input/eventX`

**验收标准**：
- `/dev/input/eventX` 出现
- `evtest` 能收到 EV_KEY 事件
- rmmod 无报错
- 倒车信号 → Qt 切换后视画面

### P4：第二种 Sensor → 内核 I2C 驱动 + V4L2 Subdev（移植/自研）

- **总线类型**：I²C（`i2c_driver`，**不是** platform_driver）
- **驱动类型**：移植或自研（取决于选型）
- **对应周次**：第8-9周
- **简历价值**：高。展示 V4L2 Subdev + Media Graph + DPHY/CSI2/ISP 完整链路。

**升级步骤**（详见 PROJECT_ROADMAP.md §四第8-9周）：
1. 硬件确认（电源时序、MCLK、RESET/PWDN、CSI lane mapping）
2. I²C 读取 chip ID
3. 编写 `drivers/media/i2c/imx219.c`（或其他型号）
   - `i2c_driver` 注册
   - `v4l2_subdev` + media entity 注册
   - format enumeration + frame size enum
   - 一套 mode register list
   - `s_stream()` + Runtime PM
4. DTS endpoint + link frequencies + data lanes
5. DPHY/CSI2/CIF/ISP 连接

**验收标准**：
- `dmesg | grep -i sensor` 看到 probe
- `media-ctl -p` 看到节点
- `v4l2-ctl --stream-mmap --stream-count=300` 能连续采集

### P5（机动项）：AMP RPMsg → rpmsg_driver（适配）

- **总线类型**：rpmsg_bus（`rpmsg_driver`）
- **驱动类型**：适配
- **对应周次**：仅核心驱动、摄像头、稳定性草稿均完成且至少提前5个工作日时执行；否则放到12周之后
- **简历价值**：中。展示异构通信能力。

**升级步骤**（详见 PROJECT_ROADMAP.md §四第11周的 AMP 机动项）：
1. RT-Thread 固件编译
2. 指定 CPU 小核启动
3. Linux 与 RTOS RPMsg ping/pong
4. 消息序号 + CRC + 超时
5. 延迟统计 + 连续 10 万条消息稳定性

本轮只承诺 RTOS 启动和 RPMsg ping/pong；MPU6050 迁移、`rpmsg_driver + IIO` 虚拟传感器属于后续扩展，不得提前写入简历完成项。

### 不升级项

| 设备 | 不升级理由 |
|------|-----------|
| 蜂鸣器 | 不写私有字符设备；用 libgpiod 或 PWM/LED 框架 |
| GPS | termios 用户态足够；写内核驱动属于过度工程 |
| 简单按键 | 用主线 `gpio-keys` 即可，不重写 |
| DHT11 | 使用主线 IIO 驱动，只做 DTS 使能 |

---

## 五、升级后简历写法对比

### 当前（用户态）：
> 用户态 I2C 驱动开发：基于 `/dev/i2c-N` + `i2c_smbus_*` API 实现 MPU6050 6 轴 IMU 驱动，
> 含 WHO_AM_I 校验、PWR_MGMT 唤醒、DLPF/GYRO/ACCEL 量程配置、批量读 14 字节大端转换；
> 50 Hz 采样 + 互补滤波（α=0.98）实时输出 pitch/roll

### 升级后（内核驱动）：
> Linux 内核驱动开发：
> - **I2C 驱动**：编写 `drivers/iio/light/ap3216c.c` 自研 IIO 驱动，
>   `module_i2c_driver` 注册 + `of_match_table` 设备树匹配；
>   实现 probe/remove 生命周期、regmap 寄存器访问、`threaded_irq` 中断处理、
>   `devm_iio_device_register` 注册到 IIO 子系统；
>   sysfs 输出 `in_illuminance_raw` / `in_intensity_ir_raw` / `in_proximity_raw` 标准接口
> - **Platform 驱动**：编写 `drivers/input/misc/quarkpi-vehicle-io.c` 自研 platform_driver，
>   `module_platform_driver` 注册 + `devm_gpiod_get` + `gpiod_to_irq` + `threaded_irq` +
>   Input 子系统 + 软件消抖
> - **V4L2 Subdev**：移植第二种 Sensor 驱动，`v4l2_subdev` + media entity + DPHY/CSI2/ISP 完整链路

额外加分项：
- Linux 内核驱动开发经验（秋招嵌入式岗位强加分）
- 设备树子节点 + compatible 匹配机制实战
- IIO / Input / V4L2 / Media 多子系统熟悉度
- 中断处理（threaded IRQ）+ 并发同步
- Runtime PM / suspend-resume

---

## 六、升级前置条件

升级前必须满足以下条件，否则会推倒重来：

1. **第0阶段软件基线封板完成**（参考 [PROJECT_ROADMAP.md §四第0阶段](./PROJECT_ROADMAP.md)）
   - 现有 Qt/RKNN/MPP/RTSP 作为驱动验收平台
2. **自编译 BSP 镜像完成**（参考 [PROJECT_ROADMAP.md §四第2-3周](./PROJECT_ROADMAP.md)）
   - Ubuntu 虚拟机 `~/buildroot/kernel` 可正常 `make dtbs`
   - 交叉编译链 `aarch64-linux-gnu-` 可用
   - 烧录工具 `rkflash.sh` / `upgrade_tool` 流程跑通
3. **板端启动方式确认**
   - 当前 dtb 路径 `/boot/rk3588s-quarkpi-ca2.dtb`
   - 备份原 dtb 再操作
4. **MPU6050 INT 引脚接 GPIO**（如果要走中断驱动）
   - 查已占用引脚清单，避开冲突
5. **第二种 Sensor 在第1周完成采购与硬件门禁**
   - 接口、电平、电源、MCLK、RESET/PWDN、lane mapping、公开datasheet/上游驱动和IQ文件至少满足最低出图条件

---

## 七、执行原则

1. **一次只升级一个驱动**：避免同时改多个驱动，问题来源混淆
2. **保留用户态实现**：升级期间用户态代码不删，通过 IMU 专用 `IImuSampleSource` 做 A/B 回退
3. **每升级一个就板端验收 + 写入 MODULE_STATUS.md**：保留工程证据
4. **不强求提交 mainline**：按 upstream 习惯编写和自查，但12周目标以板端验收为主
5. **参考框架、独立验证**：MPU6050复用并适配主线；AP3216C依据datasheet独立实现，参考同类IIO框架时保留来源和许可证
6. **严格区分总线类型**：I²C 设备用 `i2c_driver`，SoC IP 用 `platform_driver`，不可混用

---

## 八、学习资源

- Linux 内核源码：`drivers/iio/` 目录下所有 IIO 驱动可参考
- Documentation/iio/iio_configfs.rst
- Documentation/devicetree/bindings/iio/
- 《Linux 设备驱动程序》（LDD3）第 14 章
- kernel.org 官方文档：https://www.kernel.org/doc/html/latest/driver-api/iio/index.html
- 正点原子《嵌入式 Linux 驱动开发指南》I2C / IIO / Platform 章节

---

Last updated: 2026-07-21（重写：修正 i2c_driver 被错称为 Platform Driver 的概念性错误；原"优先级 P1-P4 升级路线"已并入新 12 周计划 PROJECT_ROADMAP.md，本文档作为"驱动开发能力提升路径"附录保留，提供用户态 vs 内核驱动的对比学习材料）
