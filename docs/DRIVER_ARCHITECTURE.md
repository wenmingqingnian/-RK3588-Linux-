# 驱动架构分析（Driver Architecture Analysis）

> 本文档分析当前 RK3588S_CarTerminal 项目中所有外设的驱动实现方式，
> 对比用户态 I2C、内核 I2C 驱动、IIO 子系统三种方式的优缺点，
> 作为后续 P1-P2 阶段驱动开发的决策依据。
>
> 创建：2026-07-21

---

## 一、I2C 设备驱动三种方式的关系

### 关键澄清

"内核 I2C 驱动" 和 "IIO 驱动" **不是并列关系**，而是**包含关系**：

```
              I2C 设备驱动大类
             (都基于 i2c_driver 框架)
                     │
   ┌─────────────────┼─────────────────────┐
   │                 │                     │
纯 I2C 驱动       IIO 驱动             其他子系统驱动
(不接子系统)      (接 IIO 框架)         (接 Input/V4L2/等)
   │                 │                     │
• 自定义 fops     • inv_mpu6050          • gpio-keys
• 自定义 ioctl    • ap3216c (自研)        • IMX415 (V4L2)
• 自定义字符设备  • sht31                • 触摸屏 (Input)
```

IIO 驱动底层**也是 `i2c_driver`**，只是上层注册到 IIO 子系统，
通过 `/sys/bus/iio/` 和 `/dev/iio:deviceX` 标准化暴露数据。

---

## 二、三种方式详细对比

### 方式 1：用户态 I2C（i2c-dev）

**当前 MPU6050 采用的方式。**

#### 原理

应用层直接 `open("/dev/i2c-N")`，用 `ioctl(I2C_SLAVE, addr)` 选从机，
用 `i2c_smbus_*` API 读写寄存器。内核里的 `i2c-dev.c` 只提供通用字符设备接口，
**不感知具体芯片**。

#### 代码示例

```cpp
// device/mpu6050device.cpp
int fd = open("/dev/i2c-3", O_RDWR);
ioctl(fd, I2C_SLAVE, 0x68);
int whoami = i2c_smbus_read_byte_data(fd, 0x75);  // 读 WHO_AM_I
i2c_smbus_write_byte_data(fd, 0x6B, 0x00);          // 写 PWR_MGMT_1
```

#### 优缺点

| 优点 | 缺点 |
|------|------|
| 开发最快，不进内核 | 没有中断、没有 buffer |
| 不需要内核编译环境 | 用户态轮询有抖动 |
| 寄存器配置在应用层，易调试 | CPU 占用高 |
| 失败不会 crash 内核 | 跨应用无法共享 |
| 适合原型验证 | 简历价值低（仅证明懂协议） |

#### 适用场景

- 快速硬件验证、原型开发
- 一次性测试脚本
- 非生产场景

---

### 方式 2：纯内核 I2C 驱动（不接子系统）

**不接入任何子系统，自己实现 file_operations。**

#### 原理

写一个 `i2c_driver`，内核启动时 probe 设备，但只提供 I2C 寄存器访问 API，
数据通路自己实现（自定义 ioctl / 字符设备）。

#### 代码示例

```c
// drivers/misc/my_chip.c
static int my_chip_probe(struct i2c_client *client,
                         const struct i2c_device_id *id) {
    struct my_chip *chip;

    chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
    chip->regmap = devm_regmap_init_i2c(client, &my_regmap_cfg);

    // 自己注册字符设备
    // 自己实现 file_operations.read/write/ioctl
    // 暴露 /dev/my_chip

    return 0;
}

static struct i2c_driver my_chip_driver = {
    .driver = {
        .name = "my-chip",
        .of_match_table = my_chip_of_match,
    },
    .probe = my_chip_probe,
    .id_table = my_chip_id,
};
module_i2c_driver(my_chip_driver);
```

#### 优缺点

| 优点 | 缺点 |
|------|------|
| 完整的内核驱动能力 | 数据通路要自己设计 |
| 可以注册 IRQ | 不标准化，每个驱动各搞各的 |
| 可以实现 mmap/DMA | 跨应用共享要自己做 |
| 适合非传感器设备 | 不适合传感器（重复造轮子） |
| **面试核心考点** | 工作量大 |

#### 适用场景

- 非传感器设备（EEPROM、RTC、私有协议芯片）
- 需要自定义字符设备的特殊硬件
- 音频编解码器（接 ASoC 子系统）
- 面试必考的基础能力展示

---

### 方式 3：IIO 子系统驱动

**MPU6050 升级目标、AP3216C 自研目标、SHT30 采用的方式。**

#### 原理

IIO（Industrial I/O）是 Linux 为传感器设计的标准化框架。
驱动注册到 IIO 后，自动提供：
- **sysfs 通道**：`/sys/bus/iio/devices/iio:deviceX/in_accel_x_raw`
- **triggered buffer**：`/dev/iio:deviceX` 字符设备 + 中断触发 + 内核缓冲
- **scale/sampling_frequency**：标准化参数

#### 代码示例

```c
// drivers/iio/light/ap3216c.c (第5-6周自研)
static int ap3216c_probe(struct i2c_client *client) {
    struct iio_dev *indio_dev;
    struct ap3216c_data *data;

    indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
    data = iio_priv(indio_dev);

    data->regmap = devm_regmap_init_i2c(client, &ap3216c_regmap_cfg);

    indio_dev->channels = ap3216c_channels;
    indio_dev->num_channels = ARRAY_SIZE(ap3216c_channels);
    indio_dev->info = &ap3216c_info;
    indio_dev->modes = INDIO_DIRECT_MODE;

    return devm_iio_device_register(&client->dev, indio_dev);
}

static struct i2c_driver ap3216c_driver = {
    .driver = {
        .name = "ap3216c",
        .of_match_table = ap3216c_of_match,
    },
    .probe = ap3216c_probe,
};
module_i2c_driver(ap3216c_driver);  // ← 底层还是 i2c_driver!
```

#### 优缺点

| 优点 | 缺点 |
|------|------|
| 标准化 ABI，跨应用共享 | 开发难度高 |
| 中断驱动，低 CPU 占用 | 需理解 IIO 框架 |
| triggered buffer + 时间戳 | 不适合非传感器 |
| 内核缓冲，低抖动 | 调试需要 iio_info/iio_generic_buffer |
| **简历价值高**（子系统理解） | 编译需要 CONFIG_IIO=y |

#### 适用场景

- 所有传感器（加速度、陀螺仪、光照、温湿度、压力、接近）
- ADC/DAC
- 连续采集 + 中断触发场景

---

## 三、三种方式总览对比表

| 维度 | 用户态 I2C | 纯内核 I2C 驱动 | IIO 子系统驱动 |
|------|-----------|----------------|---------------|
| 代码位置 | 应用层 C++ | `drivers/i2c/` 或 `drivers/misc/` | `drivers/iio/` |
| 是否进内核 | ❌ | ✅ | ✅ |
| 芯片初始化 | 应用层 | 内核 probe | 内核 probe |
| 数据访问 | ioctl + i2c_smbus_* | 自定义 fops | 标准 sysfs + `/dev/iio:X` |
| 中断支持 | ❌ | ✅ | ✅ Data Ready IRQ |
| 内核缓冲 | ❌ | 需自己实现 | ✅ triggered buffer |
| 时间戳精度 | 用户态 clock_gettime | 需自己实现 | ✅ 内核中断 iio_get_time_ns |
| 采样抖动 | 大（受调度影响） | 中 | 小（硬中断触发） |
| CPU 占用 | 高（轮询） | 中 | 低（中断驱动） |
| 跨应用共享 | ❌ | ✅ 可共享 | ✅ 多应用可同时读 sysfs |
| 标准化程度 | ❌ 无标准 | ❌ 各搞各的 | ✅ 统一 ABI |
| 开发难度 | 最低 | 中 | 高 |
| 简历价值 | 低（仅懂协议） | **高（面试核心）** | 高（子系统理解） |
| 适用场景 | 原型/调试 | 非传感器/自定义设备 | 传感器标准接入 |

---

## 四、IIO 子系统适用场景分析

IIO 不仅适用于"连续采集"，它的完整定位是**"传感器标准化接入框架"**。

| 场景 | IIO 是否适合 | 例子 |
|------|-------------|------|
| 连续高速采集 | ✅ 最佳 | MPU6050 50Hz、IMU 1kHz、ADC 1MHz |
| 低速周期采样 | ✅ 适合 | AP3216C 光照 1Hz、SHT30 温湿度 0.5Hz |
| 事件触发采集 | ✅ 适合 | 接近传感器 PS 阈值告警 |
| 非传感器设备 | ❌ 不适合 | EEPROM、RTC、音频Codec、显示屏 |
| 需要自定义协议 | ❌ 不适合 | 私有字符设备协议 |

---

## 五、当前外设驱动现状与升级计划

### 5.1 MPU6050（六轴姿态传感器）

**WHO_AM_I 说明**：代码兼容 0x68 (MPU6050) 和 0x70 (MPU9250 克隆)。
当前板端读出 0x70，说明是 MPU9250 兼容克隆（GY-9255 之类），
六轴部分寄存器与 MPU6050 完全兼容。

| 项 | 当前 | 升级目标（第4周） |
|---|------|------------------|
| 方式 | 用户态 I2C | IIO 子系统 |
| 内核驱动 | 无 | 启用主线 `inv_mpu6050` |
| 设备树 | dtbo overlay 启用 I2C3 | 添加 mpu6050 子节点 |
| 数据访问 | `ioctl + i2c_smbus_*` | `/dev/iio:deviceX` read() |
| 采样 | QTimer 50Hz 轮询 | triggered buffer + Data Ready IRQ |
| 代码 | `device/mpu6050device.cpp` | 新增 `device/mpu6050_iiobuffersource.cpp` |

### 5.2 AP3216C（光照/接近/红外传感器）

| 项 | 当前 | 升级目标（第5-6周） |
|---|------|---------------------|
| 方式 | IIO sysfs（出厂 BSP 提供） | **自研完整 IIO 驱动** |
| 内核驱动 | BSP 自带 | `drivers/iio/light/ap3216c.c`（新） |
| 数据访问 | sysfs 逐轴读 | triggered buffer + IRQ |
| 简历价值 | 低 | **最高**（独立开发 IIO Sensor Driver） |

### 5.3 SHT30（温湿度传感器，替换 DHT11）

**决策**：放弃 DHT11，改用 SHT30。

| 项 | DHT11（放弃） | SHT30（采用） |
|---|--------------|--------------|
| 接口 | 单总线（时序敏感） | I2C（标准） |
| 精度 | 温度 ±2℃，湿度 ±5%RH | 温度 ±0.2℃，湿度 ±2%RH |
| 采样率 | 0.5 Hz（2 秒一次） | 最高 10 Hz |
| 内核驱动 | `drivers/iio/humidity/dht11.c`（时序脆弱） | `drivers/iio/humidity/sht31.c`（主线已有，稳定） |
| 用户态实现 | 难（时序难保证） | 容易（标准 I2C） |

SHT30 方案：主线 IIO 驱动 `sht31.c` + 设备树启用，应用层读 sysfs。

### 5.4 GPS（ATGM336H）

| 项 | 内容 |
|---|------|
| 接口 | UART5 (/dev/ttyS5, 9600bps) |
| 方式 | 用户态 termios |
| 内核依赖 | TTY 子系统 + 8250 UART 驱动（主线已有） |
| 升级计划 | **不升级**（termios 用户态足够） |

**为什么不写内核串口驱动**：TTY 子系统已标准化，RK3588S 的 8250 兼容 UART 控制器驱动主线已有。
自写内核串口驱动是重复造轮子，简历价值为零。

### 5.5 蜂鸣器

| 项 | 内容 |
|---|------|
| 接口 | GPIO（active-low 触发） |
| 方式 | 用户态 libgpiod（底层 ioctl） |
| 升级计划 | **不升级**（用 libgpiod/PWM/LED 框架，不写私有字符设备） |

**libgpiod vs 内核 GPIO 驱动**：
- libgpiod：用户态 C 库，封装 `/dev/gpiochipN` ioctl，适合输出控制
- 内核 GPIO 驱动：内核里直接调 `gpiod_*`，适合 platform 驱动里用
- 蜂鸣器是"输出控制"，libgpiod 完全够用

### 5.6 按键

| 项 | 当前 | 升级目标（第7周） |
|---|------|------------------|
| 方式 | 用户态 libgpiod | platform + Input 子系统 |
| 代码 | `device/keyinputdevice.cpp` | `drivers/input/misc/quarkpi-vehicle-io.c`（新） |
| 事件 | 用户态 poll | `/dev/input/eventX` |
| 场景 | 单按键 | 倒车/ACC/告警多信号 + 消抖 |

**为什么按键要升级**：按键要上报到 Input 子系统（`/dev/input/eventX`），
libgpiod 做不到，必须写 platform 驱动。

### 5.7 IMX415 摄像头

| 项 | 内容 |
|---|------|
| 接口 | MIPI CSI-2 |
| 方式 | V4L2 + Media Controller（内核驱动已有） |
| 应用层 | `media/capture_thread.cpp` V4L2 ioctl 采集 |
| 升级计划 | **不动**（作为基线保留） |

**为什么摄像头必须内核驱动**：
1. MIPI CSI-2 物理层是高速差分串行链路（D-PHY），时序 ps 级，用户态做不了
2. ISP 图像处理（demosaic、白平衡、曝光）在内核 ISP 驱动
3. DMA 传输由内核管理
4. V4L2 buffer 管理（mmap、VIDIOC_QBUF/DQBUF）是内核态

---

## 六、面试考察层次分析

### 考察层次

| 层次 | 内容 | 面试频率 |
|------|------|---------|
| L1 协议层 | I2C 时序、START/STOP、ACK/NACK、7-bit 地址、多主仲裁 | ⭐⭐⭐⭐⭐ |
| L2 总线框架 | i2c_driver、i2c_client、of_match_table、probe、i2c_transfer | ⭐⭐⭐⭐⭐ |
| L3 子系统框架 | IIO/Input/V4L2/regmap/devm_* | ⭐⭐⭐ |
| L4 具体驱动 | inv_mpu6050、ap3216c | ⭐⭐ |

### 为什么 I2C 感觉是面试重点

1. **I2C 是基础协议**，所有 I2C 设备驱动都用到，是共同基础
2. **IIO 是特定子系统的框架**，只有传感器岗位才深入问
3. **面试官想看"设备-总线-驱动"模型的理解**，这是 L2 层

### I2C 驱动面试核心考点

```
1. 设备树匹配：
   of_match_table → compatible → DTS 节点 → i2c_client

2. probe 流程：
   分配设备结构 → 初始化 regmap → 读 chip ID 校验 → 配置寄存器 → 注册到子系统

3. 数据访问：
   i2c_transfer() / i2c_smbus_read_byte_data() / regmap_read()

4. 中断处理：
   devm_request_threaded_irq() → 顶半部 → 底半部

5. 资源管理：
   devm_* 自动释放，probe 失败路径
```

**无论接 IIO 还是不接，都要会**。

### 最优简历策略

在当前项目里**两种都展示**：
- MPU6050/AP3216C/SHT30 走 IIO → 展示子系统理解
- vehicle-io 走 platform + Input → 展示基础框架能力

---

## 七、当前项目外设驱动总览

| 外设 | 当前方式 | 后续动作 | 代码位置 |
|------|---------|---------|---------|
| MPU6050 | 用户态 I2C | 启用主线 IIO 驱动 + 设备树 | `device/` + `drivers/iio/imu/inv_mpu6050/` |
| AP3216C | IIO sysfs（BSP 提供） | **自研**完整 IIO 驱动 | `device/` + `drivers/iio/light/ap3216c.c`（新） |
| DHT11 → **SHT30** | 用户态（DHT11） | 换 SHT30，主线 IIO 驱动 + DTS | `drivers/iio/humidity/sht31.c` |
| GPS | termios | 不动 | `device/gpsdevice.cpp` |
| 蜂鸣器 | libgpiod | 不动 | `device/buzzerdevice.cpp` |
| 按键 | libgpiod | 第7周整合到 vehicle-io | `drivers/input/misc/`（新） |
| IMX415 | V4L2 应用层采集 | 不动（基线） | `media/capture_thread.cpp` |
| 第二 Sensor | 无 | 第8-9周移植 V4L2 subdev | `drivers/media/i2c/`（新） |

---

## 八、升级优先级与简历价值

| 优先级 | 任务 | 简历价值 | 周次 |
|--------|------|---------|------|
| P1 | MPU6050 IIO 适配 | 展示 IIO trigger/buffer + A/B 对比 | 第4周 |
| P1 | AP3216C 自研 IIO 驱动 | **最高**：独立开发 IIO Sensor Driver | 第5-6周 |
| P1 | Vehicle-IO platform + Input | 展示 platform_driver + GPIO IRQ + Input | 第7周 |
| P2 | 第二种 Sensor V4L2 移植 | 展示 V4L2 subdev + Media Graph | 第8-9周 |
| P3 | SHT30 替换 DHT11 | 主线驱动 backport sht31.c + 简历展示 backport 能力 | 第4-6周机动 |

---

## 九、第2-3周 BSP 编译工作手册

> 基于 SDK 源码验证结果（2026-07-21）
> SDK 路径: `<HOME>/quarkpi-sdk-clean`
> 内核版本: Linux 5.10.209
> 工具链: aarch64-linux-gnu-gcc 9.4.0

### 9.1 SDK 驱动能力基线表

| 外设 | 驱动源文件 | defconfig 启用 | DTS 节点 | 状态 |
|------|-----------|----------------|---------|------|
| MPU6050 IIO | ✅ 完整 (inv_mpu6050/) | ❌ 未启用 | ❌ 未配置 | 需开 CONFIG + 补 DTS |
| AP3216C | ✅ iio/light 有 | ❌ 未启用 | ❌ 未配置 | 按计划自研 |
| SHT30 | ❌ sht31.c 不存在 | ❌ | ❌ | **需 backport 主线** |
| IMX415 | ✅ imx415.c | ✅ =y (rockchip_linux_defconfig) | ✅ 已配置3路 | **零工作量** |
| I2C 控制器 | ✅ i2c-rk3x.c | ✅ =y | ✅ I2C3 已启用 | 就绪 |
| GPIO 控制器 | ✅ gpio-rockchip.c | ⚠️ 仅 gki.config =m | ✅ | 需确认 |
| UART 8250 | ✅ | ✅ =y, 10个UART, DW | ✅ UART5 已启用 | 就绪 |
| gpio-keys | ✅ | ✅ =y | - | 就绪 |
| RKNN/NPU | ✅ rknpu/ | ✅ =y | - | 就绪 |
| RGA | ✅ rga2/ | ✅ VIDEO_ROCKCHIP_RGA=y | - | 就绪 |
| DRM 显示 | ✅ | ✅ =y | - | 就绪 |
| V4L2 CIF/ISP | ✅ | ✅ 全部 =y | - | 就绪 |

### 9.2 关键发现

#### 发现 1：I2C3 和 UART5 已经启用

在 `rk3588s-atk-40-pins.dtsi` 中：

```dts
&i2c3 {
    pinctrl-0 = <&i2c3m1_xfer>;
    status = "okay";          // 已启用
};

&uart5 {
    pinctrl-0 = <&uart5m1_xfer>;
    status = "okay";          // 已启用
};
```

**结论**：MPU6050 和 GPS 的总线已经就绪，不需要再改设备树启用总线。

#### 发现 2：IMX415 DTS 已配置3路摄像头

`rk3588s-atk-camera.dtsi` 和 `rk3588-atk-cameras.dtsi` 中已配置3路 imx415：

| 摄像头 | I2C 地址 | CSI PHY | facing | 用途 |
|--------|---------|---------|--------|------|
| camera1 | 0x1a | dcphy0 | back | 后视 |
| camera2 | 0x1a | dcphy1 | front | 前视/倒车 |
| camera3 | 0x1a | dphy0 | back | 备用 |

**结论**：Phase 2 的"第二摄像头"任务可能只需要应用层双路采集，DTS 层已就绪。

#### 发现 3：MPU6050 可以零代码升级

```
驱动源文件: drivers/iio/imu/inv_mpu6050/  (完整 12 个文件)
包含: inv_mpu_core.c, inv_mpu_i2c.c, inv_mpu_trigger.c, inv_mpu_ring.c
Kconfig: 自动 select IIO_BUFFER + IIO_TRIGGERED_BUFFER
```

只需两步：
1. defconfig 加 `CONFIG_INV_MPU6050_IIO=y`
2. DTS 的 `&i2c3` 节点下加 mpu6050 子节点

#### 发现 4：SHT30 需 backport

SDK 没有 sht31.c，但 IIO humidity 目录有备选驱动：
- si7020.c (Sensirion SHT2x)
- htu21.c (±0.2℃/±2%RH)
- hdc100x.c, hts221.c

**决策**：从主线内核 backport sht31.c，展示 backport 能力。

#### 发现 5：Buildroot 依赖包未启用

Buildroot 有 QuarkPi 专用 defconfig (`rockchip_atk_quarkpi_ca2_defconfig`)，
但 Qt5/ffmpeg/alsa-lib/librga 都未启用，需要在 menuconfig 中配置。

#### 发现 6：U-Boot defconfig 未找到

SDK 的 U-Boot 版本 2017，没有 QuarkPi 专用 defconfig，需要确认编译方式。

### 9.3 待办清单

#### 第2周：内核 + U-Boot

**1. 启用 MPU6050 IIO 驱动**

```bash
# 在 defconfig 中添加
CONFIG_INV_MPU6050_IIO=y
CONFIG_INV_MPU6050_I2C=y
```

或通过 menuconfig：
```
Device Drivers → IIO support → Inertial Measurement Units → Invensense MPU6050
```

**2. 添加 MPU6050 DTS 节点**

在 `rk3588s-atk-40-pins.dtsi` 或新文件中添加：

```dts
&i2c3 {
    pinctrl-0 = <&i2c3m1_xfer>;
    status = "okay";

    mpu6050: mpu6050@68 {
        compatible = "invensense,mpu6050";
        reg = <0x68>;
        interrupt-parent = <&gpio0>;  // 根据 PCB 确认
        interrupts = <RK_PC0 IRQ_TYPE_EDGE_FALLING>;  // 根据 PCB 确认
        pinctrl-names = "default";
        pinctrl-0 = <&mpu6050_int_pin>;  // 需要在 pinctrl 中定义
    };
};
```

注意：当前 MPU6050 读出 WHO_AM_I=0x70（MPU9250 克隆），需确认 compatible 是否需要改成 `invensense,mpu9250`。

**3. Backport sht31.c**

```bash
# 从主线内核 5.15+ 获取 sht31.c
# 放到 drivers/iio/humidity/sht31.c

# 修改 drivers/iio/humidity/Kconfig
# 添加:
# config SHT31
#     tristate "Sensirion SHT3x humidity and temperature sensor"
#     depends on I2C
#     select REGMAP_I2C
#     help
#       ...

# 修改 drivers/iio/humidity/Makefile
# 添加:
# obj-$(CONFIG_SHT31) += sht31.o

# 在 defconfig 中添加
CONFIG_SHT31=y

# 在 DTS 中添加
&i2c3 {  // 或其他 I2C 总线
    sht30: sht30@44 {
        compatible = "sensirion,sht3x";
        reg = <0x44>;
    };
};
```

**4. 确认 GPIO_ROCKCHIP 在主 defconfig 中启用**

```bash
# 检查
grep "GPIO_ROCKCHIP" arch/arm64/configs/rockchip_linux_defconfig

# 如果没有，添加
CONFIG_GPIO_ROCKCHIP=y
```

**5. 编译内核**

```bash
cd ~/quarkpi-sdk-clean/kernel
make ARCH=arm64 rockchip_linux_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8
```

**6. 确认 U-Boot 编译方式**

```bash
cd ~/quarkpi-sdk-clean/u-boot
# 查找 QuarkPi 相关配置
grep -r "quarkpi\|ca2\|atk" configs/ 2>/dev/null
# 如果没有，可能需要从 device/rockchip 目录编译
ls ~/quarkpi-sdk-clean/device/rockchip/
```

#### 第3周：Buildroot + 部署

**1. 配置 Buildroot 启用依赖包**

```bash
cd ~/quarkpi-sdk-clean/buildroot
make rockchip_atk_quarkpi_ca2_defconfig
make menuconfig

# 启用以下包:
# Target packages → Graphic libraries and applications → qt5
# Target packages → Multimedia → ffmpeg
# Target packages → Audio and video applications → alsa-lib
# Target packages → Libraries → Hardware handling → librga
```

**2. 编译 rootfs**

```bash
make -j8
```

**3. 烧录 SD 卡**

```bash
cd ~/quarkpi-sdk-clean
sudo ./rkflash.sh  # 或 SDK 提供的烧录脚本
# eMMC 保持出厂镜像不动
```

**4. 验证**

SD 卡启动后：
```bash
# 验证 MPU6050 IIO
ls /sys/bus/iio/devices/
cat /sys/bus/iio/devices/iio:deviceX/in_accel_x_raw

# 验证 SHT30
cat /sys/bus/iio/devices/iio:deviceY/in_humidityrelative_raw

# 验证 IMX415
v4l2-ctl --list-devices

# 验证 I2C/UART/GPIO
ls /dev/i2c-* /dev/ttyS* /dev/gpiochip*

# 基线回归测试
cd /opt/carterminal
git checkout factory-image-software-baseline
./CarTerminal
```

### 9.4 防变砖机制

```
eMMC: 保持出厂镜像完全不动（保险）
SD 卡: 烧录自编译镜像（开发用）

启动逻辑：插卡 → SD 卡启动；拔卡 → eMMC 启动
变砖风险：零（SD 卡随便刷，eMMC 永远是保险）
```

### 9.5 简历价值映射

| 任务 | 简历价值 | 展示能力 |
|------|---------|---------|
| MPU6050 IIO 启用 | 中 | 内核配置 + DTS + IIO 子系统理解 |
| SHT31 backport | 中 | 内核驱动移植能力 |
| AP3216C 自研 IIO 驱动 | 最高 | 独立开发 IIO Sensor Driver |
| Vehicle-IO platform 驱动 | 高 | platform_driver + GPIO IRQ + Input |
| IMX415 双摄 | 中 | V4L2 应用层多路采集 |
| 内核剪裁 | 低 | 内核配置理解 |
| BSP 自编译 | 低 | 嵌入式工程能力 |

---

Last updated: 2026-07-21 (Added Section 9: BSP Build Handbook based on SDK verification)
