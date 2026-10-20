# AP3216C IIO 驱动移植与实现文档

## 1. 文档信息

- 驱动文件：
  - `kernel/drivers/iio/light/ap3216c.c`
  - `kernel/drivers/iio/light/ap3216c.h`
- 目标平台：QuarkPi CA2 / RK3588S
- 内核：Linux `5.10.209-gc0433ef265af`
- 驱动形态：自研、built-in、IIO 子系统
- 已实现增强项：Runtime PM、GPIO IRQ、IIO Event、Qt IIO 后端适配。
- 文档状态：最终冻结版本。

## 2. 移植背景

项目早期可以通过用户态 I2C 直接访问 AP3216C，但这种方式存在以下限制：

- 用户态自行管理寄存器和时序，无法复用内核电源管理。
- 不能自然接入 IIO sysfs、IIO Event 和设备自动枚举。
- GPIO 中断、阈值事件和并发访问缺乏统一所有权。
- Qt 应用与硬件寄存器强耦合。
- 多进程或多后端并发时容易产生总线占用冲突。
- rootfs 更新后，用户态脚本和权限配置容易漂移。

因此将 AP3216C 迁移为 IIO 驱动，形成：

```text
AP3216C 硬件
  -> I2C/regmap
  -> AP3216C IIO 驱动
  -> IIO sysfs + IIO Event fd
  -> Qt IIO backend
  -> UI 显示与告警
```

用户态 I2C 后端仍保留用于 A/B 验证和故障隔离，但正式配置固定为 IIO。

## 3. 驱动目标与非目标

### 3.1 目标

1. 完成 AP3216C probe、初始化和三通道读取。
2. 将 ALS、PS、IR 映射为 IIO channel。
3. 使用 Runtime PM 管理设备启停。
4. 使用 GPIO IRQ 接收 AP3216C 中断。
5. 将 PS 阈值中断转换为 IIO threshold event。
6. 支持用户态配置 rising/falling 阈值和事件使能。
7. 保持错误路径、remove 路径和 PM 引用平衡。
8. 允许 Qt 自动扫描设备并持有 IIO event fd。

### 3.2 非目标

- 不实现未被当前需求验证的 buffer/triggered-buffer。
- 不声称支持所有 AP3216C 兼容芯片。
- 不把未确认的芯片 ID 固定值作为唯一身份依据。
- 不在驱动中硬编码板级 GPIO。
- 不使用自定义 ioctl 替代标准 IIO ABI。

## 4. 文件结构

### 4.1 `ap3216c.h`

头文件负责集中定义：

- 寄存器地址；
- 位掩码和字段；
- 工作模式；
- 阈值相关常量；
- 驱动内部共享结构所需声明。

寄存器定义、bit 位、量程、阈值格式和中断状态位必须以最终 datasheet 与板端读写验证为准。本文不复制完整寄存器表，避免文档与冻结源码发生二次漂移。

### 4.2 `ap3216c.c`

主文件负责：

- I2C driver 和 OF match；
- regmap 配置；
- probe/remove；
- 设备初始化；
- IIO channel 与 `read_raw`；
- Runtime PM callbacks；
- IRQ handler；
- IIO Event read/write config；
- 阈值读写；
- 锁、状态和错误处理。

## 5. Probe 流程

最终 probe 逻辑应遵循以下顺序：

```text
分配 iio_dev 与私有数据
  -> 初始化 regmap
  -> 初始化 mutex/状态
  -> 读取并确认设备可通信
  -> 复位或设置已验证的初始工作模式
  -> 注册 IIO channel/info
  -> 获取可选 IRQ
  -> 请求 threaded IRQ
  -> 初始化 Runtime PM
  -> 注册 IIO device
```

设计要点：

- 板级地址和 IRQ 由设备树提供。
- 驱动不假设固定 IIO 设备号。
- 初始化失败必须返回明确错误码。
- probe 中已获取的资源使用 devm 机制或严格逆序释放。
- Runtime PM 启用后，错误路径必须避免 usage counter 泄漏。
- 没有 IRQ 时，基础三通道读取仍可独立工作；增强版正式板级配置包含 IRQ。

## 6. IIO 通道建模

最终驱动提供三个读取通道：

| 物理量 | IIO 语义 | sysfs 文件 |
|---|---|---|
| 环境光 ALS | illuminance | `in_illuminance_raw` |
| 接近 PS | proximity | `in_proximity_raw` |
| 红外 IR | intensity/IR | `in_intensity_ir_raw` |

采用 raw ABI 的原因：

- 当前停止线要求真实原始数据，不要求在内核中提供复杂标定。
- 原始值可保持与 datasheet 寄存器输出一致。
- Qt 可根据应用需要显示、阈值判断或后续换算。
- 避免在未完成光学标定前提供误导性的物理单位。

## 7. `read_raw` 实现

`read_raw` 的基本流程：

```text
校验 mask 和 channel
  -> pm_runtime_resume_and_get()
  -> 加锁
  -> 读取对应寄存器
  -> 按 datasheet 合并字段
  -> 解锁
  -> pm_runtime_mark_last_busy()
  -> pm_runtime_put_autosuspend()
```

要求：

- ALS、PS、IR 的多字节值必须按照设备定义合并。
- 不能在锁内进行不必要的长时间睡眠。
- 任一寄存器读取失败都应返回底层错误码。
- PM 引用必须在所有成功和失败路径中配平。
- IIO Event 已启用时，设备可能已有一个长期 PM 引用；一次 `read_raw` 会使 `runtime_usage` 短暂从 1 变为 2，这是正常并发引用。

## 8. Runtime PM

### 8.1 设计目的

Runtime PM 用于：

- 空闲时降低 AP3216C 功耗；
- 读取时自动唤醒；
- IIO Event 开启期间保持设备工作；
- 关闭全部事件后允许 autosuspend。

### 8.2 引用模型

正式行为：

```text
无读取、无事件：
runtime_usage=0，允许 suspended

一次 read_raw：
临时获取引用，读取后释放

启用 IIO Event：
持有长期引用，runtime_usage 通常为 1

IIO Event + 同时 read_raw：
runtime_usage 可短暂为 2

关闭 IIO Event：
释放长期引用，最终回到 usage=0 并可 suspended
```

因此不能把 `runtime_usage == 1` 当作运行期恒等条件。稳定性判据应为：

```text
事件启用期间：
control=auto
status=active
usage>=1
```

### 8.3 已验证行为

- `power/control=auto`
- 事件启用时设备保持 active。
- 事件关闭后 PM 引用释放。
- autosuspend 生效。
- 30分钟测试中 358 个样本 `usage=1`，2 个样本 `usage=2`；两次均为 `control=auto,status=active`，判定为正常瞬时并发引用。
- 未出现 usage 低于 1、非 active 或新增内核错误。

## 9. GPIO IRQ

### 9.1 设计

IRQ 由 DTS 提供，驱动使用 threaded IRQ 处理可睡眠的 I2C 访问。核心原则：

- 顶半部不进行 I2C 读取。
- 线程化 handler 读取中断状态和当前 PS 值。
- 中断状态确认和清除严格依据器件寄存器定义。
- 事件方向依据当前值、阈值和状态判定。
- IIO timestamp 使用标准接口生成。

### 9.2 板端结果

最终中断号：

```text
IRQ 153
```

`/proc/interrupts` 中设备名为 `ap3216c`，靠近/移开目标物时计数增长。刷写最终 rootfs 后，IRQ 仍可触发，证明 DTS、驱动、rootfs 和应用集成没有破坏中断链路。

## 10. IIO Event

### 10.1 ABI

驱动支持 proximity threshold event：

- rising event；
- falling event；
- rising/falling enable；
- rising/falling threshold value。

正式阈值：

```text
falling=50
rising=100
```

对应 sysfs：

```text
events/in_proximity_thresh_falling_en
events/in_proximity_thresh_rising_en
events/in_proximity_thresh_falling_value
events/in_proximity_thresh_rising_value
```

### 10.2 PM 与事件状态

事件第一次启用时获取长期 PM 引用；最后一个事件关闭时释放引用。必须防止：

- 多次 enable 重复增加 usage；
- 单边事件关闭误释放另一边仍需要的引用；
- remove 时残留引用；
- IRQ 到达时设备处于 suspended。

### 10.3 用户态 event fd

Qt 后端通过标准 IIO ioctl 获取 event fd：

```c
ioctl(iio_device_fd, IIO_GET_EVENT_FD_IOCTL, &event_fd)
```

早期把 event fd 当成普通路径或错误 ioctl 处理会导致事件链失败；最终实现已修复并通过板端验证。

## 11. 并发与锁

驱动至少需要保护以下共享状态：

- 寄存器读写序列；
- rising/falling enable 状态；
- 阈值缓存或读写；
- 事件 PM 引用是否已持有；
- IRQ 与 sysfs 配置的并发。

基本规则：

1. 不在持锁状态下调用可能形成反向依赖的 PM API。
2. 对同一寄存器的 read-modify-write 必须串行化。
3. IRQ handler 与 `write_event_config` 不能产生状态撕裂。
4. PM 引用状态与事件 enable 状态应在同一受控路径更新。
5. remove 前先阻止新的中断和用户访问，再释放 PM 状态。

## 12. 错误处理

错误分类：

| 错误 | 处理 |
|---|---|
| I2C/regmap 访问失败 | 返回底层错误，记录受控日志 |
| 无效 channel/mask | `-EINVAL` |
| IRQ 请求失败 | 正式增强配置下 probe 失败，避免伪装成事件可用 |
| PM resume 失败 | 不继续寄存器访问 |
| 阈值写入失败 | 不更新软件状态 |
| event fd 用户态打开失败 | Qt 后端报告事件不可用，不伪造事件 |
| 设备被用户态 I2C 独占 | A/B 测试必须避免与 IIO 驱动并发访问 |

最终 AP3216C dmesg 错误计数为 0。MPU6500 的错误不应归入 AP3216C 驱动。

## 13. 内核集成

关联文件：

```text
drivers/iio/light/ap3216c.c
drivers/iio/light/ap3216c.h
drivers/iio/light/Kconfig
drivers/iio/light/Makefile
Documentation/devicetree/bindings/iio/light/liteon,ap3216c.yaml
arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts
arch/arm64/configs/rockchip_linux_defconfig
```

提交序列中 AP3216C 对应：

```text
f5949e9d0  dt-bindings: iio: light: add Lite-On AP3216C
2b61a2739  iio: light: add AP3216C sensor driver
ffce73d94  arm64: dts: rockchip: enable QuarkPi CA2 sensors and UART5
c0433ef26  arm64: defconfig: enable QuarkPi CA2 sensor drivers
```

最终分支：

```text
ap3216c-integration-20260726
```

## 14. 编译、烧录与板端验证

### 14.1 编译结果

- kernel build：PASS
- boot image：PASS
- worktree：最终 clean
- kernel release：无 `-dirty`
- boot SHA256：`3253eb0b4130fe3a715e3592af760c043a4240f610ac659d301b3d43e1de9ca8`

### 14.2 板端验证矩阵

| 项目 | 结果 |
|---|---|
| probe | PASS |
| ALS raw | PASS |
| PS raw | PASS |
| IR raw | PASS |
| Runtime PM auto | PASS |
| autosuspend | PASS |
| GPIO IRQ | PASS |
| rising event | PASS |
| falling event | PASS |
| event enable/disable | PASS |
| PM 引用释放 | PASS |
| dmesg AP3216C error | 0 |

### 14.3 最终事件回归

最终回归产生：

```text
4 rising + 4 falling = 8 IIO events
IRQ delta = 8
```

事件数与 IRQ 增量精确匹配。事件关闭后设备返回 suspended、usage=0。

## 15. 30分钟稳定性

测试配置：

```text
360 samples
5 seconds/sample
1800 seconds
```

原始结果：

```text
read_fail_count=0
process_fail_count=0
event_fd_fail_count=0
runtime_pm_fail_count=2
error_count_start=0
error_count_final=0
```

两次 PM “失败”对应：

```text
sample 141: auto, active, usage=2
sample 356: auto, active, usage=2
```

原脚本错误要求 usage 必须严格等于 1。经独立审计，正确结论为：

```text
AP3216_30MIN_STABILITY_FINAL=PASS
rerun_required=no
```

原始 STOP 摘要保留，独立裁决文件用于说明测试脚本假阳性，未删除或篡改原始证据。

## 16. 已知限制与后续维护

1. 当前提供 raw 数据，未提供经过实验标定的 lux 或距离物理量。
2. 当前事件只覆盖项目所需 proximity threshold。
3. 修改阈值格式、INT 极性或状态清除方法前，必须重新核对 datasheet。
4. 修改 PM 引用逻辑后，必须复测 enable/disable、并发 read_raw 和 autosuspend。
5. IIO 设备编号不固定，用户态必须按 `name=ap3216c` 扫描。
6. 用户态 I2C 后端只用于 A/B 和诊断，正式系统使用 IIO。
7. AP3216C 的完成不消除 MPU6500 独立问题。

## 17. 最终结论

自研 AP3216C IIO 驱动已完成：

```text
基础读取 + Runtime PM + GPIO IRQ + IIO Event + 正式 DTS/defconfig
```

并已通过 boot、rootfs、Qt、刷写后验证和30分钟稳定性测试。该驱动可作为当前 QuarkPi CA2 项目的冻结基线。
