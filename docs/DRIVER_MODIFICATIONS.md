# BSP 驱动修改记录（面试讲解用）

> 本文档详细记录第2-3周 BSP 编译中所有驱动修改内容，
> 包括修改原因、技术决策、具体代码和验证方法，
> 用于面试时清晰讲解"在这个项目中做了哪些驱动层工作"。
>
> 创建：2026-07-22

---

## 一、修改总览

| 序号 | 修改项 | 文件 | 类型 | 难度 |
|------|--------|------|------|------|
| 1 | Backport sht31.c | `drivers/iio/humidity/sht31.c` | 新增文件 | 中 |
| 2 | SHT31 Kconfig | `drivers/iio/humidity/Kconfig` | 修改 | 低 |
| 3 | SHT31 Makefile | `drivers/iio/humidity/Makefile` | 修改 | 低 |
| 4 | 内核 defconfig | `arch/arm64/configs/rockchip_linux_defconfig` | 修改 | 低 |
| 5 | 设备树 DTS | `arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts` | 修改 | 中 |

**修改范围**：内核驱动层 + 设备树，不涉及 U-Boot 和 Buildroot。

---

## 二、修改一：Backport sht31.c 驱动

### 2.1 修改原因

项目原计划使用 DHT11 温湿度传感器，但 DHT11 是单总线协议，时序要求 us 级，用户态 GPIO 轮询抖动大、精度差（温度±2℃，湿度±5%RH，采样率 0.5Hz）。

决定改用 SHT30（I2C 接口，精度温度±0.2℃、湿度±2%RH，最高 10Hz）。

但 SDK 内核 5.10 的 `drivers/iio/humidity/` 目录下**没有 sht31.c**（有 DHT11、SI7020、HTU21 等，唯独缺 SHT3x），需要从主线内核 backport。

### 2.2 技术决策

| 决策点 | 选择 | 原因 |
|--------|------|------|
| 从哪个版本 backport | Linux v5.10 | 与 SDK 内核版本一致，避免 API 不兼容 |
| 放在哪个子系统 | IIO（`drivers/iio/humidity/`） | SHT3x 是传感器，IIO 是标准框架，sysfs 标准接口 |
| 用 triggered buffer 还是 sysfs | sysfs 单次读取（single-shot） | 项目只需周期性读取温湿度，不需要连续高速采集，简化实现 |
| 编译方式 | `=y`（编入内核） | 调试阶段方便，不用加载模块 |

### 2.3 具体修改

#### 新增文件：`drivers/iio/humidity/sht31.c`（260 行）

从 Linux v5.10 主线获取，核心实现：

```c
/* 驱动核心结构 */
struct sht31_data {
    struct i2c_client *client;
    struct mutex iio_lock;
    int temperature;  /* millidegrees centigrade */
    int humidity;      /* milli-percent */
};

/* IIO 通道定义 - 两个通道：温度 + 湿度 */
static const struct iio_chan_spec sht31_channels[] = {
    {
        .type = IIO_TEMP,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
                               BIT(IIO_CHAN_INFO_SCALE),
    },
    {
        .type = IIO_HUMIDITYRELATIVE,
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
                              BIT(IIO_CHAN_INFO_SCALE),
    },
};

/* probe 函数 - 设备树匹配后调用 */
static int sht31_probe(struct i2c_client *client, ...)
{
    /* 1. 检查 I2C 功能 */
    i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_WRITE_WORD_DATA);

    /* 2. 分配 IIO 设备 */
    indio_dev = devm_iio_device_alloc(dev, sizeof(*data));

    /* 3. 配置 IIO 通道 */
    indio_dev->channels = sht31_channels;
    indio_dev->num_channels = ARRAY_SIZE(sht31_channels);

    /* 4. 软复位 */
    i2c_smbus_write_word_data(client, SHT31_SOFT_RESET, 0);

    /* 5. 注册 IIO 设备 */
    devm_iio_device_register(dev, indio_dev);
}

/* 设备树匹配表 */
static const struct of_device_id sht31_of_match[] = {
    { .compatible = "sensirion,sht3x" },  /* 匹配 DTS 中的 compatible */
    { .compatible = "sensirion,sts3x" },
    { }
};

/* I2C 驱动结构 */
static struct i2c_driver sht31_driver = {
    .driver = {
        .name = "sht31",
        .of_match_table = sht31_of_match,
    },
    .probe = sht31_probe,
    .id_table = sht31_id,
};
module_i2c_driver(sht31_driver);
```

#### 修改文件：`drivers/iio/humidity/Kconfig`

在 SI7020 配置项后、`endmenu` 前添加：

```kconfig
config SHT31
	tristate "Sensirion SHT3x relative humidity and temperature sensor"
	depends on I2C
	select CRC8
	help
	  If you say yes here you get support for the Sensirion
	  SHT30/SHT31/SHT35 relative humidity and temperature sensors.

	  This driver can also be built as a module. If so, the module
	  will be called sht31.
```

> **踩坑记录**：Kconfig 必须 `select CRC8`，否则链接报错 `undefined reference to 'crc8'`。
> sht31.c 用 `DECLARE_CRC8_TABLE` + `crc8()` 做 SHT3x 数据帧的 CRC-8 校验（多项式 0x31，初值 0xFF），
> 这些符号定义在 `lib/crc8.c` 中，必须由 `CONFIG_CRC8=y` 启用。

#### 修改文件：`drivers/iio/humidity/Makefile`

在 `obj-$(CONFIG_SI7020) += si7020.o` 后添加：

```makefile
obj-$(CONFIG_SHT31) += sht31.o
```

### 2.4 踩坑记录（backport 实战）

backport 过程中遇到两个典型编译/链接错误，反映了内核驱动开发的底层细节：

#### 坑 1：`-Werror=overflow` —— SMBUS API 误用

**错误现象**：
```
drivers/iio/humidity/sht31.c:18:38: error: unsigned conversion from 'int' to 'u8' changes value
from '9216' to '0' [-Werror=overflow]
#define SHT31_MEASURE_SINGLESHOT_HPM 0x2400
drivers/iio/humidity/sht31.c:76:7: note: in expansion of macro 'SHT31_MEASURE_SINGLESHOT_HPM'
```

**根因分析**：
原版 sht31.c 调用 `i2c_smbus_write_word_data(client, SHT31_MEASURE_SINGLESHOT_HPM, 0)`，
该 API 原型为：
```c
s32 i2c_smbus_write_word_data(struct i2c_client *client, u8 command, u16 value);
```
- 第二个参数 `command` 是 **u8**（范围 0-255）
- 但 SHT3x 命令 `SHT31_MEASURE_SINGLESHOT_HPM = 0x2400 = 9216`、`SHT31_SOFT_RESET = 0x30A2 = 12450` 都是 **u16**
- 16 位命令塞进 u8 参数会截断，GCC `-Werror=overflow` 把 warning 升级为 error

**修复方案**：新增 `sht31_write_command()` 辅助函数，改用 `i2c_master_send()` 发送 2 字节命令：
```c
static int sht31_write_command(struct i2c_client *client, u16 cmd)
{
	u8 tx[2];
	tx[0] = cmd >> 8;
	tx[1] = cmd & 0xff;
	return i2c_master_send(client, tx, sizeof(tx));
}
```

并相应修改 `sht31_read_values()` 和 `sht31_probe()` 中的调用，同时把 `i2c_check_functionality` 检查项
从 `I2C_FUNC_SMBUS_WRITE_WORD_DATA` 改为 `I2C_FUNC_I2C`（因为改用了原始 I2C 传输）。

**技术要点**：
- SHT3x 命令是 16 位裸命令（非标准 SMBus 协议），不能用 SMBus wrapper，必须用 raw I2C 传输
- 展示了对 I2C SMBUS vs I2C 原始协议区别的理解

#### 坑 2：`undefined reference to 'crc8'` —— Kconfig select 依赖遗漏

**错误现象**：
```
ld: drivers/iio/humidity/sht31.o: in function `sht31_read_values':
sht31.c:108: undefined reference to `crc8'
sht31.c:113: undefined reference to `crc8'
```

**根因分析**：
sht31.c 调用 `crc8()` 函数对 SHT3x 数据帧做 CRC-8 校验（多项式 0x31，初值 0xFF）：
```c
DECLARE_CRC8_TABLE(sht31_crc8_table);
if (crc8(sht31_crc8_table, buf, 2, SHT31_CRC8_INIT) != buf[2]) { ... }
```
但 `crc8()` 实现在 `lib/crc8.c` 中，只有 `CONFIG_CRC8=y` 时才编入内核。
我们最初写的 Kconfig 漏掉了 `select CRC8`，导致链接器找不到符号。

**修复方案**：在 `drivers/iio/humidity/Kconfig` 的 SHT31 配置块中添加 `select CRC8`：
```kconfig
config SHT31
	tristate "Sensirion SHT3x relative humidity and temperature sensor"
	depends on I2C
	select CRC8          # ← 必加，否则链接报错
	help
	  ...
```

`select` 会让 Kconfig 系统自动把 `CONFIG_CRC8` 设为 `y`，无需手动改 defconfig。

**技术要点**：
- `select` 与 `depends on` 的区别：`select` 强制启用依赖（适合驱动自带的底层库），`depends on` 只检查（需用户手动启用）
- `DECLARE_CRC8_TABLE` 是宏，会在编译期生成 CRC 查找表，但 `crc8()` 函数本身在 `lib/crc8.c`
- 展示了对内核 Kconfig 依赖管理机制的理解

#### 坑 3（仅警告）：`override: reassigning to symbol IIO`

**警告现象**：
```
arch/arm64/configs/rockchip_linux_defconfig:699:warning: override: reassigning to symbol IIO
```

**原因**：SDK 默认 defconfig 中 `CONFIG_IIO` 在多处出现（一次在主文件，一次在末尾我追加的），
Kconfig 检测到重复赋值发出 warning，但取最后一次赋值，不影响功能。
属于无害告警，可在后续清理时合并到原位置。

#### 坑 4：SHT30 probe `error 2` —— i2c_master_send 返回值约定

**错误现象**：
```
sht31 3-0044: failed to reset device
sht31: probe of 3-0044 failed with error 2
```

**根因分析**：
我自写的 `sht31_write_command()` 辅助函数直接 `return i2c_master_send(...)`：
```c
static int sht31_write_command(struct i2c_client *client, u16 cmd)
{
    u8 tx[2];
    tx[0] = cmd >> 8;
    tx[1] = cmd & 0xff;
    return i2c_master_send(client, tx, sizeof(tx));  // ← 返回 2（字节数）
}
```
`i2c_master_send` 成功时返回**发送的字节数（2）**，不是 0。
而内核 API 约定：**成功返回 0，失败返回负错误码**。
调用方 `if (ret) { ... return ret; }` 把 2 当成错误，probe 返回 errno 2 = ENOENT。

**修复方案**：转换返回值到内核惯例（0 成功，负数失败）：
```c
static int sht31_write_command(struct i2c_client *client, u16 cmd)
{
    u8 tx[2];
    int ret;
    tx[0] = cmd >> 8;
    tx[1] = cmd & 0xff;
    ret = i2c_master_send(client, tx, sizeof(tx));
    if (ret < 0)
        return ret;
    return ret == sizeof(tx) ? 0 : -EIO;
}
```

**技术要点**：
- I2C SMBUS API（如 `i2c_smbus_read_byte_data`）成功返回 0 或正值数据
- I2C master API（如 `i2c_master_send`）成功返回字节数，**不是 0**
- 自写 wrapper 必须转换到内核惯例：0 成功 / 负数失败
- 这个 bug 展示了对内核 API 返回值约定的深入理解

#### 坑 5：MPU6500 probe `error -22` —— 驱动强制要求 IRQ

**错误现象**：
```
inv-mpu6050-i2c 3-0068: Could not find IRQ 0
inv-mpu6050-i2c: probe of 3-0068 failed with error -22
```

**根因分析**：
inv_mpu6050 驱动在 `inv_mpu_core_probe()`（`inv_mpu_core.c:1460`）中强制检查 IRQ：
```c
desc = irq_get_irq_data(irq);
if (!desc) {
    dev_err(dev, "Could not find IRQ %d\n", irq);
    return -EINVAL;  // ← -22 的出处
}
```
DTS 中没声明 `interrupts` → `client->irq = 0` → `irq_get_irq_data(0)` 返回 NULL → 直接返回 -EINVAL。
这个驱动**不支持无中断模式**，必须配中断引脚。

**修复方案**：DTS 中添加完整的中断声明 + pinctrl 配置：
```dts
&i2c3 {
    imu@68 {
        compatible = "invensense,mpu6500";
        reg = <0x68>;
        interrupt-parent = <&gpio3>;
        interrupts = <RK_PC1 IRQ_TYPE_EDGE_FALLING>;
        pinctrl-names = "default";
        pinctrl-0 = <&mpu6500_int_pin>;
    };
};

&pinctrl {
    mpu6500 {
        mpu6500_int_pin: mpu6500-int-pin {
            rockchip,pins = <3 RK_PC1 RK_FUNC_GPIO &pcfg_pull_up>;
        };
    };
};
```

**技术要点**：
- inv_mpu6050 是 triggered buffer 驱动，必须用中断触发数据采集，无中断无法工作
- `interrupt-parent = <&gpio3>` 指定 GPIO 控制器
- `interrupts = <RK_PC1 IRQ_TYPE_EDGE_FALLING>`：GPIO3_C1，下降沿触发（MPU INT 低电平有效）
- `&pcfg_pull_up`：上拉电阻，确保 INT 空闲时为高电平
- `RK_PC1` 是 Rockchip 宏，展开为 pin 17（C 组第 1 位）
- 这个修复展示了设备树 GPIO 中断配置 + pinctrl 复用组配置

#### 坑 6：SHT30 读数 `Input/output error` —— CRC 表未初始化

**错误现象**：
```
$ cat /sys/bus/iio/devices/iio:device1/in_temp_raw
cat: '...': Input/output error
dmesg:
  sht31 3-0044: measure cmd ret=0       ← 命令发送成功
  sht31 3-0044: recv ret=6              ← 接收 6 字节成功
  sht31 3-0044: buf=6c e9 29 a0 a2 e0   ← 数据帧完整
  sht31 3-0044: temp CRC fail           ← CRC 校验失败
```

**根因分析**：
sht31.c 声明 CRC 表但**从未调用 `crc8_populate_msb()` 填充**：
```c
DECLARE_CRC8_TABLE(sht31_crc8_table);  // 只是声明一个 256 字节空表（全 0）
// probe 中没有调用 crc8_populate_msb()！
```
`crc8()` 函数用全 0 的查找表计算，结果永远是 0，与传感器返回的 CRC 字节
（如 `0x29`、`0x4b`）不匹配，判定为 CRC 失败，返回 `-EIO`。

**修复方案**：在 `sht31_probe` 中添加 CRC 表初始化：
```c
crc8_populate_msb(sht31_crc8_table, SHT31_CRC8_POLYNOMIAL);
```

**技术要点**：
- `DECLARE_CRC8_TABLE` 只是声明存储空间，**不会自动填充查找表**
- 必须调用 `crc8_populate_msb()` 或 `crc8_populate_lsb()` 初始化
- SHT3x 用 MSB first 的 CRC-8（多项式 0x31），所以用 `crc8_populate_msb`
- 这个 bug 展示了对内核 CRC8 库 API 使用约定的理解

#### 坑 7：SHT30 湿度读数永远为 0 —— IIO ABI 单位换算 bug

**错误现象**：
```
$ cat /sys/bus/iio/devices/iio:device1/in_humidityrelative_raw
0   ← 永远是 0
```
但 dmesg 显示数据帧 `buf=6c e9 29 a0 a2 e0`，湿度原始值 `0xa0a2 = 41122`，
按 datasheet 公式 `41122 / 65536 * 100 = 62.7%RH`，应该是 63 才对。

**根因分析**：
原版 sht31.c 在 `sht31_read_values` 中已经把湿度换算成毫百分比：
```c
data->humidity = be16_to_cpu(values[1]);
data->humidity = DIV_ROUND_CLOSEST(data->humidity * 100, (1 << 16));
// data->humidity = 63（毫百分比 / 100 = 百分比）
```
但 `read_raw` 中又除了一次 100，且 scale 配置不匹配：
```c
case IIO_HUMIDITYRELATIVE:
    *val = data->humidity / 100;  // ← 63 / 100 = 0（整数除法丢精度）
    break;
case IIO_CHAN_INFO_SCALE:
    *val = 100;  // ← scale=100，但 raw=0，实际值=0%（错误）
```
温度也有类似问题，`data->temperature` 单位是毫度，但 `read_raw` 中 `(data->temperature + 45000) / 175`
直接返回"度"的近似值，配合 `scale=175` 换算反而错误。

**修复方案**：修正 `read_raw` 中湿度和温度的处理逻辑，让 raw 值直接是实际值：
```c
case IIO_TEMP:
    *val = (data->temperature + 45000) / 175;  // 返回"度"（保留原版逻辑）
    break;
case IIO_HUMIDITYRELATIVE:
    *val = data->humidity;  // ← 不再除 100，直接返回毫百分比 / 100 = 百分比
    break;

case IIO_CHAN_INFO_SCALE:
    case IIO_TEMP:        *val = 175; break;  // 温度 scale（保持原版）
    case IIO_HUMIDITYRELATIVE: *val = 1; break;  // ← 湿度 scale=1（raw 直接是 %RH）
```

**技术要点**：
- IIO ABI 约定：`实际值 = raw × scale`（或 `raw × scale / 10^scale_factor`）
- 原版代码在 `read_values` 中做了一次换算，`read_raw` 中又做了一次，导致重复换算
- 修复后：温度 raw=428, scale=175 → `428 × 175 / 1000 - 45 = 29.9°C`
- 修复后：湿度 raw=63, scale=1 → `63 × 1 = 63%RH`
- 这个 bug 展示了对 IIO ABI（`raw × scale` 换算公式）的理解

### 2.5 面试讲解要点

> "SDK 的内核 5.10 里没有 SHT3x 驱动，我从主线 v5.10 backport 了 sht31.c，
> 放到 drivers/iio/humidity/ 目录，修改了 Kconfig 和 Makefile 让它参与编译。
> 选 IIO 子系统而不是 hwmon，因为 IIO 是 Linux 传感器的标准框架，
> 通过 sysfs 标准接口暴露数据（in_temp_raw / in_humidityrelative_raw），
> 应用层读取方式和 MPU6500 一致，统一了传感器接入架构。
>
> backport 过程踩了五个坑：
> 第一，原版代码用 `i2c_smbus_write_word_data()` 把 16 位 SHT3x 命令塞进 u8 参数，
> 触发 `-Werror=overflow`。SHT3x 命令是非标准 SMBus 协议，我改用 `i2c_master_send()`
> 发 2 字节裸命令，并相应调整 `i2c_check_functionality` 检查项。
> 第二，链接报 `undefined reference to 'crc8'`，因为 Kconfig 漏写了 `select CRC8`。
> sht31.c 用 `DECLARE_CRC8_TABLE` + `crc8()` 做 SHT3x 数据帧的 CRC-8 校验，
> 这个函数在 lib/crc8.c，必须由 CONFIG_CRC8=y 启用。
> 加上 `select CRC8` 后 Kconfig 自动启用依赖，链接通过。
> 第三，自写的 `sht31_write_command` 直接返回 `i2c_master_send` 的结果，
> 但该 API 成功返回字节数（2）不是 0，被调用方当成错误，probe 返回 ENOENT。
> 修复为检查 `ret == sizeof(tx) ? 0 : -EIO`，符合内核 API 约定。
> 第四，CRC 校验失败因为 `DECLARE_CRC8_TABLE` 只声明空表，
> 必须在 probe 中调用 `crc8_populate_msb(sht31_crc8_table, 0x31)` 填充查找表。
> 第五，湿度读数永远为 0，因为 `read_raw` 中重复除了一次 100，
> 且 IIO ABI 的 `raw × scale` 换算公式配置错误，修正 scale=1 让 raw 直接是 %RH。
>
> MPU6500 驱动 probe 时报 `-EINVAL`，因为 inv_mpu6050 是 triggered buffer 驱动，
> 在 `inv_mpu_core.c:1460` 强制检查 `client->irq`，无中断直接返回错误。
> 修复方式是在 DTS 中给 imu@68 添加 `interrupt-parent = <&gpio3>` 和
> `interrupts = <RK_PC1 IRQ_TYPE_EDGE_FALLING>`，并在 pinctrl 中配置 GPIO3_C1 为
> 上拉输入中断引脚。MPU6500 的 INT 是低电平有效，所以用下降沿触发 + 上拉电阻。
>
> 这些实战经历让我更深入理解了内核 Kconfig 依赖管理（select vs depends on）、
> I2C SMBUS vs 原始 I2C 协议区别、内核 API 返回值约定、CRC8 库使用约定、
> IIO ABI 换算公式、设备树 GPIO 中断配置，以及 Linux 传感器驱动的完整构建链。"

---

## 三、修改二：内核 defconfig

### 3.1 修改原因

SDK 默认 defconfig 没有启用项目需要的传感器驱动和 IIO 基础设施。

### 3.2 具体修改

在 `arch/arm64/configs/rockchip_linux_defconfig` 末尾添加：

```bash
# === CarTerminal Project: Sensor Drivers ===
CONFIG_IIO=y                          # IIO 子系统核心
CONFIG_IIO_BUFFER=y                   # IIO 缓冲区支持
CONFIG_IIO_TRIGGERED_BUFFER=y         # IIO 触发缓冲（MPU6500 需要）
CONFIG_REGMAP=y                       # 寄存器访问抽象层
CONFIG_REGMAP_I2C=y                   # I2C regmap（inv_mpu6050 依赖）
CONFIG_INV_MPU6050_IIO=y             # MPU6500 IIO 驱动（主线已有）
CONFIG_INV_MPU6050_I2C=y             # MPU6500 I2C 传输层
CONFIG_SHT31=y                       # SHT30 驱动（本次 backport）
CONFIG_GPIO_ROCKCHIP=y               # RK3588S GPIO 控制器
```

### 3.3 依赖关系

```
CONFIG_IIO (核心)
  ├── CONFIG_IIO_BUFFER (MPU6500 triggered buffer 依赖)
  │   └── CONFIG_IIO_TRIGGERED_BUFFER
  ├── CONFIG_REGMAP → CONFIG_REGMAP_I2C (inv_mpu6050 寄存器访问依赖)
  ├── CONFIG_INV_MPU6050_IIO → CONFIG_INV_MPU6050_I2C
  └── CONFIG_SHT31
CONFIG_GPIO_ROCKCHIP (独立)
```

### 3.4 面试讲解要点

> "我在 defconfig 中启用了 IIO 子系统和 7 个 CONFIG 项。
> 重点理解依赖链：inv_mpu6050 驱动依赖 regmap_i2c 做寄存器访问，
> 依赖 iio_triggered_buffer 做中断驱动的数据采集。
> sht31 我选了 =y 编入内核而不是 =m 模块，调试阶段不需要手动 insmod。
> GPIO_ROCKCHIP 是 RK3588S 的 GPIO 控制器驱动，蜂鸣器和按键都依赖它。"

---

## 四、修改三：设备树 DTS

### 4.1 修改原因

1. SDK 默认 DTS 中 I2C3 和 UART5 的 status 是 `disabled`，板端没有 `/dev/i2c-3` 和 `/dev/ttyS5`
2. 需要在 I2C3 总线下添加 MPU6500 和 SHT30 的设备子节点，让内核驱动 probe 匹配

### 4.2 技术决策

| 决策点 | 选择 | 原因 |
|--------|------|------|
| 是否 include 40-pins.dtsi | 不 include | 该文件还启用 SPI0 等不需要的外设，带副作用 |
| I2C3/UART5 怎么启用 | 直接在板级 DTS 写 `&i2c3 { status = "okay"; }` | 精确控制，只启用需要的 |
| MPU6500 的 compatible | `invensense,mpu6500` | 实测 WHO_AM_I=0x70 对应 MPU6500，不是 MPU6050(0x68) 也不是 MPU9250(0x71) |
| MPU6500 中断引脚 | 已声明 GPIO3_C1，下降沿触发 | inv_mpu6050 驱动强制要求 IRQ，无中断会 probe 失败 -EINVAL |
| MPU6500 中断触发类型 | `IRQ_TYPE_EDGE_FALLING` + `pcfg_pull_up` | MPU INT 低电平有效，下降沿触发，上拉电阻保证空闲高电平 |
| 三传感器是否共用 I2C3 | 是 | 地址不冲突（0x1E/0x44/0x68），减少总线数量 |

### 4.3 具体修改

文件：`arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts`

在 DTS 末尾添加（含 MPU6500 中断配置）：

```dts
&i2c3 {
	pinctrl-names = "default";
	pinctrl-0 = <&i2c3m1_xfer>;     /* I2C3 M1 复用组 */
	status = "okay";

	imu@68 {
		compatible = "invensense,mpu6500";
		reg = <0x68>;
		interrupt-parent = <&gpio3>;
		interrupts = <RK_PC1 IRQ_TYPE_EDGE_FALLING>;  /* GPIO3_C1, 下降沿 */
		pinctrl-names = "default";
		pinctrl-0 = <&mpu6500_int_pin>;
	};

	sht30@44 {
		compatible = "sensirion,sht3x";
		reg = <0x44>;
	};
};

&uart5 {
	pinctrl-names = "default";
	pinctrl-0 = <&uart5m1_xfer>;     /* UART5 M1 复用组 */
	status = "okay";
};

/* MPU6500 INT 引脚配置（GPIO3_C1 上拉输入） */
&pinctrl {
	mpu6500 {
		mpu6500_int_pin: mpu6500-int-pin {
			rockchip,pins = <3 RK_PC1 RK_FUNC_GPIO &pcfg_pull_up>;
		};
	};
};
```

### 4.4 硬件接线表

| 设备 | J11 引脚 | RK3588S GPIO | 总线/地址 |
|------|---------|-------------|----------|
| MPU6500 SDA | Pin 3 | GPIO3_C0 | I2C3, 0x68 |
| MPU6500 SCL | Pin 5 | GPIO3_B7 | I2C3 |
| MPU6500 INT | Pin 7（已接） | GPIO3_C1 | IRQ 下降沿 |
| SHT30 SDA | Pin 3 | GPIO3_C0 | I2C3, 0x44 |
| SHT30 SCL | Pin 5 | GPIO3_B7 | I2C3 |
| AP3216C SDA | Pin 3 | GPIO3_C0 | I2C3, 0x1E |
| AP3216C SCL | Pin 5 | GPIO3_B7 | I2C3 |
| GPS TXD | Pin 10 | GPIO3_C5 | UART5_RX |
| GPS RXD | Pin 8 | GPIO3_C4 | UART5_TX |

### 4.5 后续待添加

**AP3216C 子节点**（第5-6周自研驱动时添加）：

```dts
ap3216c@1e {
    compatible = "liteon,ap3216c";
    reg = <0x1e>;
};
```

### 4.6 面试讲解要点

> "设备树修改是最核心的部分。SDK 默认 DTS 中 I2C3 和 UART5 都是 disabled，
> 我在板级 DTS 中精确启用这两个控制器，而不是 include 整个 40-pins.dtsi，
> 因为那个文件还启用了 SPI0 等不需要的外设。
>
> MPU6500 的 compatible 我写成 invensense,mpu6500 而不是 mpu6050，
> 因为实测 WHO_AM_I 寄存器返回 0x70，TDK 资料确认这是 MPU6500 的身份值。
> MPU6050 是 0x68（和 I2C 地址相同但含义不同），MPU9250 是 0x71。
>
> 中断引脚我没写，因为 INT 还没实际接线。设备树里写了 interrupt-parent
> 但 GPIO 没接，会触发虚假中断。等硬件接好 GPIO3_C1 后再加 pinctrl 定义。
>
> 三个传感器（MPU6500/SHT30/AP3216C）共用 I2C3，地址分别是
> 0x68/0x44/0x1E，互不冲突。UART5 用 M1 复用组，TX 是 GPIO3_C4，RX 是 GPIO3_C5，
> GPS 交叉连接。"

---

## 五、架构选择：内核 IIO vs 用户态 I2C

### 5.1 冲突说明

项目原有 MPU6050 用户态驱动（`device/mpu6050device.cpp`）直接 `open("/dev/i2c-3")` + `ioctl(I2C_SLAVE, 0x68)` 读写寄存器。

启用 `CONFIG_INV_MPU6050_IIO=y` + DTS 添加 `imu@68` 后，内核 inv_mpu6050 驱动会绑定 0x68 地址。此时：
- `i2cdetect` 显示 `UU`（被内核占用）
- 用户态 `ioctl(I2C_SLAVE, 0x68)` 返回 `EBUSY`

### 5.2 选择方案A：内核 IIO 驱动

| 方案 | 优点 | 缺点 |
|------|------|------|
| A. 内核 IIO | 中断驱动、标准 ABI、简历价值高 | 需改应用层代码 |
| B. 用户态 I2C | 不用改应用层 | 无中断、轮询抖动、无简历价值 |

**选择方案A**。后续第4周将 CarTerminal 从 `/dev/i2c-3` 改为读取 `/sys/bus/iio/devices/`。

### 5.3 面试讲解要点

> "这里有个架构冲突：原来项目用用户态 I2C 直接操作 /dev/i2c-3，
> 启用内核 IIO 驱动后内核会绑定 0x68 地址，用户态会得到 EBUSY。
> 我选择了内核 IIO 方案，因为：
> 1. 中断驱动采样比轮询更精确
> 2. IIO 的 triggered buffer 有内核时间戳
> 3. 标准 ABI（sysfs）让应用层代码更干净
> 4. 展示了内核驱动开发能力
> 后续需要改应用层从 /dev/i2c-3 改为读 /sys/bus/iio/devices/。"

---

## 六、编译方式

### 6.1 编译命令

```bash
cd ~/quarkpi-sdk-clean
./build.sh kernel
```

**不用** `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j8`，因为厂商 SDK 通过 `build.sh` 自动注入板级配置、工具链和 DTB 目标。

### 6.2 编译验证

```bash
# 1. 内核 Image 生成
ls -la kernel/arch/arm64/boot/Image

# 2. DTB 生成
ls -la kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dtb

# 3. MPU6050 驱动编译进内核
ls kernel/drivers/iio/imu/inv_mpu6050/*.o

# 4. SHT31 驱动编译成功
ls kernel/drivers/iio/humidity/sht31.o

# 5. boot.img 更新
ls -la kernel/boot.img
```

---

## 七、板端验证（编译烧录后）

### 7.1 I2C3 启用验证

```bash
# 板端执行
ls /dev/i2c-3                        # 预期：存在
i2cdetect -y 3                       # 预期：0x68 显示 UU（内核占用），0x44 显示 0x44
```

### 7.2 MPU6500 IIO 验证

```bash
ls /sys/bus/iio/devices/             # 预期：iio:device0 + iio:device1
cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw
cat /sys/bus/iio/devices/iio:device0/in_accel_y_raw
cat /sys/bus/iio/devices/iio:device0/in_accel_z_raw
cat /sys/bus/iio/devices/iio:device0/in_anglvel_x_raw
cat /sys/bus/iio/devices/iio:device0/in_temp_raw
```

### 7.3 SHT30 IIO 验证

```bash
cat /sys/bus/iio/devices/iio:device1/in_temp_raw
cat /sys/bus/iio/devices/iio:device1/in_humidityrelative_raw
```

### 7.4 UART5 / GPS 验证

```bash
ls /dev/ttyS5                        # 预期：存在
dmesg | grep -iE 'feb80000|ttyS5'    # 预期：ttyS5 is a 16550A
cat /dev/ttyS5 &                      # 读取 GPS NMEA 报文
```

---

## 八、文件变更清单

| 文件路径 | 操作 | 行数 |
|---------|------|------|
| `drivers/iio/humidity/sht31.c` | 新增 | 260 |
| `drivers/iio/humidity/Kconfig` | 修改 | +10 |
| `drivers/iio/humidity/Makefile` | 修改 | +1 |
| `arch/arm64/configs/rockchip_linux_defconfig` | 修改 | +10 |
| `arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts` | 修改 | +20 |
| `arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts.orig` | 新增（备份） | - |
| `arch/arm64/configs/rockchip_linux_defconfig.orig` | 新增（备份） | - |

---

## 九、面试一句话总结

> "在 RK3588S QuarkPi-CA2 平台上，我从主线内核 v5.10 backport 了 SHT3x 温湿度传感器的
> IIO 驱动到 SDK 的 5.10 内核，修改了 Kconfig 和 Makefile 让它参与编译。
> 在 defconfig 中启用了 IIO 子系统、inv_mpu6050、sht31 等 7 个 CONFIG 项。
> 在设备树中启用了 I2C3 和 UART5 控制器，添加了 MPU6500 和 SHT30 的设备子节点。
> 技术要点包括：理解 IIO 子系统的 of_match_table 设备树匹配、I2C regmap 寄存器访问、
> IIO triggered buffer 中断驱动采样、设备树 pinctrl 复用组配置。
> 最关键的技术决策是发现 WHO_AM_I=0x70 对应 MPU6500 而非 MPU6050，
> 以及内核 IIO 驱动绑定后用户态 I2C 访问会冲突的架构选择。
>
> backport sht31.c 过程踩了两个坑：一是 16 位 SHT3x 命令塞进 u8 的 SMBus 参数触发 -Werror=overflow，
> 改用 i2c_master_send 发 2 字节裸命令；二是 Kconfig 漏写 select CRC8 导致链接时找不到 crc8 符号，
> 加上 select 后 Kconfig 自动启用依赖。
> 这些实战经历让我更深入理解了内核 Kconfig 依赖管理（select vs depends on）、
> I2C SMBUS vs 原始 I2C 协议区别、以及 Linux 传感器驱动的完整构建链。
>
> 编译结果：./build.sh kernel 成功，boot.img 已生成，包含 MPU6500 + SHT30 + IIO 子系统驱动。"
