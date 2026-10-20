# MPU6500_IIO_PORTING.md

## 1. 文档目的

本文记录 RK3588S CarTerminal 项目从用户态 I2C 直接访问 MPU6500，迁移到 Linux IIO 子系统的完整过程，覆盖：

- 迁移动机；
- 内核配置；
- WHO_AM_I 与芯片识别；
- DTS 绑定；
- IIO channel、trigger 和 buffer 验证；
- 应用层 `IImuSampleSource` 抽象与双后端；
- 受控 A/B 验收；
- 生产方案选择；
- 面试说明要点。

本文只采用已由源码、板端日志和最终证据包确认的数据。A/B结果属于一次受控工程验收，不扩展为统计显著性结论。

---

## 2. 背景：为什么从用户态 I2C 升级到 IIO

### 2.1 原实现

原应用通过：

```text
/dev/i2c-3
```

执行：

1. `open()` 打开 I2C adapter；
2. `ioctl(I2C_SLAVE, 0x68)` 选择地址；
3. 使用 SMBus API 写 MPU 寄存器；
4. 从 `ACCEL_XOUT_H`（`0x3B`）开始批量读取 14 字节；
5. 在应用线程中按 20 ms 定时器轮询。

该实现可以工作，但设备控制权、寄存器初始化、采样节奏和错误恢复均由应用承担。

### 2.2 用户态直读在当前 BSP 中的问题

当 DTS 已声明 MPU6500、内核 `inv_mpu6050` 驱动已绑定 `3-0068` 后，继续由应用直接控制同一地址会带来以下问题：

- 内核驱动与应用可能争用设备所有权；
- 应用绕开 IIO 的 channel、scale、trigger 和 buffer 管理；
- 每个应用都需要重复实现寄存器配置和数据换算；
- 采样依赖用户态 timer 和调度；
- 无法自然使用 Data Ready IRQ 驱动的 triggered buffer；
- 使用 `I2C_SLAVE_FORCE` 会隐藏所有权冲突，存在破坏内核驱动状态的风险。

因此生产路径迁移为：

```text
MPU6500 Data Ready IRQ
→ inv_mpu6050
→ IIO trigger
→ IIO kernel buffer
→ /dev/iio:deviceX
→ Mpu6050IioBufferSource
→ Mpu6050Worker
```

旧 `user_i2c` 后端保留，用于回退、驱动诊断和 A/B 验证。

---

## 3. 内核配置

项目内核生成配置中已确认：

```text
CONFIG_INV_MPU6050_IIO=1
CONFIG_INV_MPU6050_I2C=1
```

对应职责：

| 配置 | 作用 |
|---|---|
| `CONFIG_INV_MPU6050_IIO` | 启用 InvenSense MPU 系列 IIO 核心驱动 |
| `CONFIG_INV_MPU6050_I2C` | 启用 I2C transport，使 DTS 中 I2C client 可绑定核心驱动 |

项目同时启用了：

```text
CONFIG_SHT31=1
```

用于同一 I2C3 总线上的 SHT30/SHT3x。

验证命令：

```bash
SDK=<HOME>/quarkpi-sdk-clean

grep -E \
	'CONFIG_INV_MPU6050_IIO|CONFIG_INV_MPU6050_I2C|CONFIG_SHT31' \
	"$SDK/kernel/.config"

grep -E \
	'CONFIG_INV_MPU6050_IIO|CONFIG_INV_MPU6050_I2C|CONFIG_SHT31' \
	"$SDK/kernel/include/generated/autoconf.h"
```

本项目使用 built-in 配置，板端不依赖手工 `modprobe` 才能完成 probe。

---

## 4. WHO_AM_I 与芯片识别

### 4.1 实际识别结果

本项目板上 IMU 的 WHO_AM_I 读取值为：

```text
0x70
```

结合：

- 实际模块；
- DTS 的 `compatible = "invensense,mpu6500"`；
- `inv_mpu6050` 驱动 probe；
- IIO 枚举名 `mpu6500`；

本项目将该设备识别为 MPU6500。

### 4.2 为什么不能只根据类名判断芯片

Linux 驱动和应用中的部分文件、类名沿用历史名称：

```text
inv_mpu6050
Mpu6050Device
Mpu6050Worker
```

这些是兼容家族或旧代码命名，不表示实际芯片一定是 MPU6050。

芯片识别应综合：

1. 板上 BOM/模块；
2. WHO_AM_I；
3. DTS compatible；
4. 驱动匹配结果；
5. IIO `name`。

### 4.3 对旧注释的说明

旧用户态代码曾把 `0x70` 描述为 MPU9250 legacy/clone 兼容值。该注释不能作为本板芯片识别的最终依据。当前项目的板端驱动与 IIO 枚举已经明确落在 MPU6500 设备类型。

如后续继续保留旧后端，建议把注释修正为“本项目 MPU6500 的 WHO_AM_I 为 `0x70`”，避免面试或维护时混淆 MPU6500 与 MPU9250。

---

## 5. DTS 绑定

完整设备树 rationale 见：

```text
docs/DEVICE_TREE_DESIGN.md
```

核心绑定为：

```dts
&i2c3 {
	status = "okay";
	pinctrl-names = "default";
	pinctrl-0 = <&i2c3m1_xfer>;

	mpu6500@68 {
		compatible = "invensense,mpu6500";
		reg = <0x68>;
		interrupt-parent = <&gpio3>;
		interrupts = <RK_PC1 IRQ_TYPE_EDGE_RISING>;
	};

	sht30@44 {
		compatible = "sensirion,sht3x";
		reg = <0x44>;
	};
};
```

硬件路径：

```text
I2C3 M1 SCL/SDA → MPU6500 0x68
GPIO3_C1        → MPU6500 INT
```

板端绑定路径已确认为：

```text
/sys/devices/platform/feab0000.i2c/i2c-3/3-0068
/sys/bus/i2c/devices/3-0068
/sys/bus/i2c/drivers/inv-mpu6050-i2c
```

IIO 设备路径位于该 I2C client 下。

---

## 6. IIO 设备与通道验证

### 6.1 按名称查找设备

不能把 `iio:device2` 永久写死，因为 IIO 编号取决于 probe 顺序。

```bash
for dev in /sys/bus/iio/devices/iio:device*; do
	[ -f "$dev/name" ] || continue
	printf '%s: ' "$dev"
	cat "$dev/name"
done
```

本次验收环境中：

```text
iio:device1 → sht3x
iio:device2 → mpu6500
```

应用实现按：

```text
/sys/bus/iio/devices/iio:device*/name
```

扫描 `name == "mpu6500"`，再推导对应的 `/dev/iio:deviceX`。

### 6.2 已验证通道

MPU6500 triggered buffer 已验证启用以下逻辑通道：

- 加速度 X/Y/Z；
- 角速度 X/Y/Z；
- 温度；
- 时间戳。

典型 scan element 名称为：

```text
in_accel_x_en
in_accel_y_en
in_accel_z_en
in_anglvel_x_en
in_anglvel_y_en
in_anglvel_z_en
in_temp_en
in_timestamp_en
```

实际可用名称应以当前设备的 `scan_elements/` 为准：

```bash
MPU_IIO=/sys/bus/iio/devices/iio:device2

find "$MPU_IIO/scan_elements" \
	-maxdepth 1 -type f -printf '%f\n' |
	sort
```

### 6.3 scale 与工程单位

IIO ABI 提供 raw、scale，以及部分通道所需的 offset。应用后端读取 sysfs 标定参数，并把内核 buffer 中的原始值转换为项目统一的工程单位：

| 数据 | 应用层单位 |
|---|---|
| 加速度 | `m/s²` |
| 角速度 | `deg/s` |
| 温度 | `°C` |

`Mpu6050Worker` 和上层 GUI 不需要知道数据来自 I2C 寄存器还是 IIO buffer。

---

## 7. Trigger 与 Buffer 验证

### 7.1 已验证运行状态

正式运行时已确认：

```text
sampling_frequency = 50
trigger/current_trigger = mpu6500-dev2
buffer/length = 100
buffer/enable = 1
```

CarTerminal 打开的文件为：

```text
/dev/iio:device2
```

设备编号只对应本次启动环境，不是接口契约。

### 7.2 独立 buffer 验证

独立 IIO buffer 测试中：

- 目标频率：50 Hz；
- 采集时间：约 4 秒；
- MPU IRQ 增量：201；
- buffer 文件大小：4776 bytes；
- 加速度、角速度、温度和时间戳 scan channel 均已启用。

该结果说明数据不是只通过 sysfs 单次读取，而是已经进入 triggered buffer。

### 7.3 应用运行验证

CarTerminal 使用 IIO 后端运行时：

- 应用持有 `/dev/iio:device2` FD；
- 5 秒 MPU IRQ 增量为 250/251；
- 日志出现：

  ```text
  using IIO buffer backend
  source opened (backend=iio_buffer)
  device became available
  ```

### 7.4 常用检查命令

```bash
MPU_IIO="$(
	for dev in /sys/bus/iio/devices/iio:device*; do
		[ -f "$dev/name" ] || continue
		[ "$(cat "$dev/name")" = "mpu6500" ] || continue
		echo "$dev"
		break
	done
)"

cat "$MPU_IIO/name"
cat "$MPU_IIO/sampling_frequency"
cat "$MPU_IIO/trigger/current_trigger"
cat "$MPU_IIO/buffer/length"
cat "$MPU_IIO/buffer/enable"

grep -E 'inv_mpu|inv-mpu' /proc/interrupts
```

---

## 8. 应用层抽象

### 8.1 `IImuSampleSource`

新增接口：

```text
device/iimusamplesource.h
```

核心方法：

```cpp
class IImuSampleSource
{
public:
	virtual ~IImuSampleSource() = default;
	virtual bool open(std::string *errorMessage) = 0;
	virtual void close() = 0;
	virtual bool isOpen() const = 0;
	virtual Mpu6050Reading readSample(std::string *errorMessage) = 0;
};
```

抽象的作用是让 Worker 只依赖统一的样本语义，而不依赖具体总线接口。

### 8.2 两个后端

#### `user_i2c`

实现类：

```text
Mpu6050Device
```

路径：

```text
/dev/i2c-3
```

主要行为：

- 普通 `I2C_SLAVE`，不是 `I2C_SLAVE_FORCE`；
- WHO_AM_I 校验；
- 配置采样率、DLPF、加速度和陀螺仪量程；
- 从 `0x3B` 起读取 14 字节；
- 应用自行换算工程值。

#### `iio_buffer`

实现类：

```text
Mpu6050IioBufferSource
```

主要行为：

1. 扫描 IIO device；
2. 按 `name == "mpu6500"` 定位 sysfs；
3. 读取 scale/offset；
4. 配置 scan elements；
5. 设置 trigger、sampling frequency 和 buffer；
6. 打开 `/dev/iio:deviceX`；
7. 按 scan layout 解析连续样本；
8. 输出统一的 `Mpu6050Reading`。

### 8.3 Worker

`Mpu6050Worker`：

- 持有 `std::unique_ptr<IImuSampleSource>`；
- 由配置选择后端；
- 默认 timer 周期为 20 ms；
- 读取成功后进行姿态估计；
- 使用 `std::chrono::steady_clock` 统计应用层交付间隔；
- 发生连续错误时关闭 source，并在后续 timer 中重试。

### 8.4 配置项

生产配置：

```ini
mpu6050_enabled=true
mpu6050_poll_interval_ms=20
mpu6050_backend=iio_buffer
```

支持：

```ini
mpu6050_backend=user_i2c
mpu6050_backend=iio_buffer
```

最终生产默认值为：

```ini
mpu6050_backend=iio_buffer
```

---

## 9. 安全的双后端切换

### 9.1 为什么需要解绑

`user_i2c` 后端和内核驱动不能同时控制地址 `0x68`。正式 A/B 测试采用以下顺序：

```text
停止 CarTerminal
→ 确认 IIO buffer/enable = 0
→ 从 inv-mpu6050-i2c 解绑 3-0068
→ 配置 backend=user_i2c
→ 启动应用并测试
→ 停止应用
→ 恢复 backend=iio_buffer
→ 重新绑定 3-0068
→ 启动应用
```

### 9.2 已验证的用户态路径

预检中已确认：

```text
FD=/dev/i2c-3
open_ok=1
open_err=0
samples=501
read_err=0
warning_error_lines=0
```

说明在内核驱动安全解绑后，普通 `I2C_SLAVE` 可以使用，不需要 `I2C_SLAVE_FORCE`。

### 9.3 禁止事项

- 内核驱动绑定时使用 `I2C_SLAVE_FORCE`；
- IIO buffer 仍开启时解绑驱动；
- `user_i2c` 正在运行时重新绑定内核驱动；
- 仅修改配置文件而不处理设备所有权；
- 测试结束后忘记恢复生产后端。

---

## 10. A/B 测试方法

### 10.1 控制条件

每个后端进行一次正式受控测试：

- 目标频率：50 Hz；
- 预热：约 10 秒；
- 稳态观察：约 120 秒；
- 相同 CarTerminal 二进制；
- 相同 INFO 日志级别；
- 禁用每样本 DEBUG 日志；
- 同一 GUI/业务运行环境；
- 记录：
  - 样本数；
  - 有效采样率；
  - read errors；
  - 应用层 delivery interval；
  - missed period；
  - Worker 线程 CPU；
  - 整进程 CPU；
  - RSS；
  - IIO 测试中的 IRQ。

### 10.2 指标语义

#### `interval_*` / `jitter_max_ms`

这是应用层成功样本交付时间间隔，由 Worker 的 `steady_clock` 计算。

它受以下因素影响：

- Qt timer；
- 线程调度；
- GUI 和其他业务负载；
- source read 的阻塞行为。

它**不是 MPU6500 硬件采样时钟抖动**。

#### `missed_periods`

表示应用层交付间隔超过统计规则阈值的事件，不等同于内核 buffer 丢包数量。

#### `read_mean_us`

两个后端的语义不同：

- `user_i2c`：主动完成一轮 I2C/SMBus 寄存器读取；
- `iio_buffer`：`read()` 可能阻塞等待下一批 triggered-buffer 数据。

因此不能用 `read_mean_us` 直接判定哪个后端“读取更快”。

---

## 11. A/B 结果

### 11.1 正式 120 秒结果

| 指标 | `iio_buffer` | `user_i2c` |
|---|---:|---:|
| 受控观察时间 | 120.164 s | 120.160 s |
| 成功样本增量 | 6009 | 6008 |
| 有效频率 | 50.006658 Hz | 50.000000 Hz |
| 读取错误增量 | 0 | 0 |
| 应用层 missed-period 事件 | 0 | 1 |
| 累计 interval mean | 19.997 ms | 20.000 ms |
| 累计 interval std | 0.326 ms | 0.386 ms |
| 累计 interval min | 18.350 ms | 6.955 ms |
| 累计 interval max | 21.462 ms | 32.261 ms |
| 累计最大间隔偏差 | 1.650 ms | 13.045 ms |
| Worker CPU | 0.723761% | 0.825649% |
| 整个 CarTerminal CPU | 139.548446% | 139.925062% |
| 平均 RSS | 186.607552 MiB | 184.789095 MiB |

说明：

- 样本增量、有效频率和 Worker CPU 是按起止累计值的差计算；
- interval/jitter 行使用结束时累计统计，包含约 10 秒预热；
- 两个后端 `read_error_delta=0`；
- IIO 测试中 MPU IRQ 频率约为 `50.016521 Hz`。

### 11.2 CPU 差异

```text
user_i2c Worker CPU  = 0.825649%
iio_buffer Worker CPU = 0.723761%
绝对差               = 0.101888 个百分点
相对降低             = 12.340%
```

该结果支持“本轮 IIO Worker CPU 更低”的工程结论。

不应写成“整机 CPU 降低 12.34%”，因为：

- 12.34%只针对 MPU Worker；
- 整个 CarTerminal 的 CPU 差异仅约 0.377 个百分点；
- GUI、视频、地图、天气、音频等线程占据了绝大多数整进程 CPU。

### 11.3 抖动与丢样表述

本轮中：

- IIO：0 次 missed-period；
- user_i2c：1 次 missed-period；
- 两者读取错误均为 0；
- 两者有效频率均约为 50 Hz。

若按成功样本数仅作事件频率描述：

```text
IIO missed-period event / sample ≈ 0 / 6009 = 0
user_i2c ≈ 1 / 6008 = 0.0166%
```

该比例不是硬件“丢样率”。现有证据没有显示硬件样本丢失；它只显示 `user_i2c` 本轮发生一次应用层交付超期。

### 11.4 内存

IIO 平均 RSS 比 user_i2c 高约 1.82 MiB。单次测试无法把该差异归因于 MPU 后端，因此：

- 不宣称 IIO 节省内存；
- 也不据此认定 IIO 有显著内存回归；
- 需要多次随机交叉测试才能建立统计结论。

---

## 12. 生产方案决定

### 12.1 最终选择

```ini
mpu6050_backend=iio_buffer
```

### 12.2 选择理由

1. 设备所有权由内核统一管理；
2. 使用 Data Ready IRQ、IIO trigger 和 kernel buffer；
3. 应用不再直接管理 MPU6500 寄存器；
4. scale/channel/buffer 遵循 Linux IIO ABI；
5. 本轮功能正确，读取错误为 0；
6. 本轮 Worker CPU 较低；
7. 本轮应用层交付时序略稳定；
8. 便于以后接入标准 IIO 工具和统一传感器框架。

### 12.3 保留 user_i2c 的理由

`user_i2c` 仍保留为：

- 驱动未加载时的诊断路径；
- IIO 与原始寄存器值对照；
- A/B 回归；
- 硬件 bring-up；
- 内核问题定位。

它不是正常生产状态下与 IIO 并行运行的第二控制者。

---

## 13. 证据目录

### 13.1 内核与应用验收

```text
<HOME>/bsp-evidence/task-3.5/mpu6500-final-acceptance-20260725-000642
```

### 13.2 应用补丁审计

```text
<HOME>/bsp-evidence/task-3.5/mpu6500-patch-audit-20260725-001759
```

应用补丁涉及：

```text
CarTerminal.pro
config/vehicle_devices.conf
device/iimusamplesource.h
device/mpu6050_iiobuffersource.cpp
device/mpu6050_iiobuffersource.h
device/mpu6050device.h
device/mpu6050worker.cpp
device/mpu6050worker.h
device/vehicledeviceconfig.cpp
device/vehicledeviceconfig.h
```

### 13.3 A/B 最终证据

```text
<HOME>/bsp-evidence/task-3.5/mpu6500-ab-instrumentation-20260725-090652/mpu6500-ab-final-acceptance
```

压缩归档：

```text
mpu6500-ab-final-acceptance.tar.gz
mpu6500-ab-final-acceptance.tar.gz.sha256
```

该归档已通过 SHA-256 校验。

---

## 14. 常见问题排查

### 14.1 IIO 设备不存在

```bash
grep -E 'INV_MPU6050' /proc/config.gz 2>/dev/null
readlink -f /sys/bus/i2c/devices/3-0068/driver
dmesg | grep -iE 'mpu|invensense|iio'
```

检查：

- DTS 是否进入实际启动 DTB；
- I2C3 pinctrl 是否为 M1；
- `3-0068` 是否存在；
- compatible 是否匹配；
- WHO_AM_I 是否可读。

### 14.2 有 IIO sysfs，但 buffer 无数据

检查：

```bash
cat "$MPU_IIO/trigger/current_trigger"
cat "$MPU_IIO/buffer/enable"
grep -E 'inv_mpu|inv-mpu' /proc/interrupts
```

重点确认：

- GPIO3_C1 pinmux 未被 UART5_TX_M1 占用；
- INT 接线正确；
- IRQ 计数增长；
- scan element 已启用；
- sample layout 与应用解析一致。

### 14.3 `user_i2c` 返回 busy

先确认内核驱动是否仍绑定：

```bash
readlink -f /sys/bus/i2c/devices/3-0068/driver
```

不要直接改用 `I2C_SLAVE_FORCE`。应停止应用和 buffer，再受控解绑驱动。

### 14.4 IIO 编号变化

不要依赖：

```text
iio:device2
```

应扫描：

```text
/sys/bus/iio/devices/iio:device*/name
```

---

## 15. 面试话术要点

### 15.1 30 秒版本

> 项目原来由 Qt 应用通过 `/dev/i2c-3` 直接轮询 MPU6500。我把设备正式声明到板级 DTS，启用 `inv_mpu6050` 的 I2C/IIO 驱动，并接入 GPIO3_C1 Data Ready 中断。内核通过 IIO trigger 和 buffer 输出加速度、角速度、温度和时间戳。应用层增加 `IImuSampleSource` 抽象，保留 user_i2c 回退后端，生产默认切换到 iio_buffer。两种后端各做了约 120 秒受控测试，都稳定达到 50 Hz、读取错误为 0；IIO 的 Worker CPU 在本轮低约 12.34%，并且没有出现应用层 missed-period。

### 15.2 为什么不用用户态一直读 I2C

> 用户态直读适合早期 bring-up，但设备已经由内核驱动声明后，应用再直接访问会造成所有权冲突，也绕开 IIO 的 trigger、buffer、scale 和错误管理。生产系统中应由内核控制硬件，应用消费稳定 ABI。

### 15.3 为什么要保留双后端

> 抽象不是为了同时控制设备，而是为了迁移可回退和可验证。相同 Worker 与上层逻辑下，只替换数据源，可以进行 A/B；驱动异常时也能在受控解绑后使用原始 I2C 路径定位问题。

### 15.4 如何证明不是只写了 DTS

> 验证链包括：`3-0068` client、驱动 symlink、`name=mpu6500` IIO 枚举、Data Ready IRQ 增长、trigger、buffer、字符设备 FD、连续样本和 120 秒 A/B。独立 buffer 测试约 4 秒得到 4776 字节，IRQ 增量 201；应用 5 秒测试 IRQ 增量约 251。

### 15.5 如何解释 WHO_AM_I

> 实际板上读取到 `0x70`，结合 BOM、`invensense,mpu6500` binding、驱动匹配和 IIO 枚举，确认设备是 MPU6500。不能因为驱动或类名叫 mpu6050 就判断芯片型号。

### 15.6 如何解释“抖动”

> 文档里的 interval 和 jitter 是 Worker 成功交付样本的应用层间隔，包含 Qt timer 和 Linux 调度影响，不是传感器晶振或硬件采样时钟抖动。硬件侧证据是 50 Hz trigger/IRQ 和 buffer 数据。

### 15.7 如何解释 CPU 结果

> IIO Worker CPU 从约 0.826% 降到约 0.724%，相对降低 12.34%。但整进程 CPU 基本相同，所以不能说整机 CPU 降低 12%。这是一次工程验收结果，不是统计显著性实验。

---

## 16. 最终结论

第 4 周 MPU6500 IIO 迁移已经完成：

- 内核配置有效；
- WHO_AM_I 和设备类型确认；
- DTS 与 GPIO IRQ 有效；
- IIO channel、trigger、buffer 有效；
- 应用双后端抽象完成；
- user_i2c 安全回退路径完成；
- 120 秒 A/B 验收完成；
- 生产默认后端冻结为 `iio_buffer`；
- 最终证据包和 SHA-256 归档完成。
