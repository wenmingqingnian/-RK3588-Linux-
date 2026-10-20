# AP3216C 设备树与内核集成设计文档

## 1. 文档信息

- 文档用途：记录 QuarkPi CA2 平台 AP3216C 传感器的设备树、Kconfig、Makefile、defconfig 与 DeviceTree binding 设计依据。
- 适用范围：RK3588S、Linux 5.10.209、I2C3、AP3216C 地址 `0x1e`、INT 接 GPIO4_A1。
- 最终内核提交：`c0433ef265afee8144326186875e509fe8b9fd67`
- 最终内核版本：`5.10.209-gc0433ef265af`
- 文档状态：已按板端实测结果冻结。
- 边界说明：本文仅对 AP3216C 子任务负责；MPU6500 的 `failed to ack interrupt` 属于独立待审查问题。

## 2. 设计目标

设备树设计需要同时满足以下目标：

1. 在 I2C3 总线上以标准 Linux I2C 子节点方式描述 AP3216C。
2. 明确设备地址、GPIO 中断、触发类型和 pinctrl 上拉配置。
3. 允许驱动通过 `devm_regmap_init_i2c()`、Runtime PM、GPIO IRQ 和 IIO Event 完成统一管理。
4. 保证配置能够通过正式 defconfig 固化并进入 boot/rootfs 构建流程。
5. 不依赖用户态手工实例化，不在启动脚本中动态创建设备。
6. 不将未经过 datasheet、SDK 源码或板端实测确认的寄存器值、IRQ 极性或 GPIO 编号写成确定事实。

## 3. 硬件连接与总线拓扑

| 项目 | 最终配置 | 依据 |
|---|---|---|
| SoC | RK3588S | QuarkPi CA2 平台 |
| I2C 控制器 | I2C3 | 板级接线与 SDK DTS |
| I2C 地址 | `0x1e` | AP3216C 器件地址及板端探测 |
| 中断引脚 | GPIO4_A1 | INT 接 J11 Pin 13 |
| IRQ 类型 | falling edge | 板端事件触发实测 |
| pinctrl 偏置 | pull-up | AP3216C INT 电气特性与板端稳定性 |
| IIO 设备名 | `ap3216c` | 驱动注册名 |
| 采集通道 | ALS、PS、IR | 驱动三通道实现与板端真实读取 |

本项目最终接线为：

```text
AP3216C SDA/SCL -> RK3588S I2C3
AP3216C INT     -> J11 Pin 13 -> GPIO4_A1
AP3216C VCC/GND -> 板级供电与地
```

实际 GPIO、电平和触发类型已经通过 `/proc/interrupts` 与 IIO Event 进行板端验证。刷写最终 rootfs 后，AP3216C 对应 IRQ 仍能增长。

## 4. I2C3 总线启用策略

AP3216C 节点直接放置在板级 DTS 的 I2C3 控制器下：

```text
kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts
```

设计原则如下：

- 使用板级 DTS 作为硬件事实的唯一入口。
- 不在用户态使用 `new_device` 临时实例化。
- 不通过额外 include 文件隐藏实际引脚选择。
- 保留 SDK 原有 I2C3 pinctrl 方案，AP3216C 仅增加器件子节点和中断 pinctrl。
- 与 MPU6500、SHT30、UART5 的其他板级变更保持可审计边界，不把整个 DTS diff 错写成 AP3216C 独占修改。

I2C3 控制器需保持：

```text
status = "okay"
pinctrl-0 指向平台已确认的 I2C3 复用组
```

具体 pinctrl 符号以最终 DTS 源码为准，本文不复制可能随 SDK 命名变化的符号名。

## 5. AP3216C 子节点设计

AP3216C 子节点应包含以下语义：

| 属性 | 作用 |
|---|---|
| `compatible = "liteon,ap3216c"` | 与驱动 OF match 表和 binding 对应 |
| `reg = <0x1e>` | 指定 I2C 地址 |
| `interrupt-parent = <&gpio4>` | 指定 GPIO4 控制器 |
| `interrupts = <... IRQ_TYPE_EDGE_FALLING>` | GPIO4_A1，下降沿触发 |
| `pinctrl-names = "default"` | 使用默认中断引脚状态 |
| `pinctrl-0 = <...>` | 指向 AP3216C INT 的 pinctrl 组 |

结构化摘要如下，实际提交以最终 DTS 为准：

```dts
&i2c3 {
    status = "okay";

    ap3216c@1e {
        compatible = "liteon,ap3216c";
        reg = <0x1e>;
        interrupt-parent = <&gpio4>;
        interrupts = <GPIO4_A1 IRQ_TYPE_EDGE_FALLING>;
        pinctrl-names = "default";
        pinctrl-0 = <&ap3216c_int>;
    };
};
```

上段仅表达结构，不替代仓库中的最终源码；GPIO 宏写法和 pinctrl 标签应以内核 DTS 实际定义为准。

## 6. 中断 pinctrl 设计

AP3216C INT 采用 GPIO 输入并启用上拉，原因如下：

1. INT 为外部器件输出，悬空会产生虚假中断。
2. 板端使用下降沿作为有效事件边沿。
3. 上拉保证空闲态稳定在高电平。
4. 驱动只申请 GPIO IRQ，不直接在驱动中修改 SoC pinmux。
5. pinctrl 配置与 IRQ 类型必须共同匹配，不能只修改其中一项。

验证结果：

```text
/proc/interrupts:
153: ... rockchip_gpio_irq ... Edge ap3216c
```

刷写最终 rootfs 后 IRQ 计数仍可增长，说明 DTS、GPIO 控制器、IRQ 映射和驱动请求链路均有效。

## 7. DeviceTree binding

binding 文件：

```text
kernel/Documentation/devicetree/bindings/iio/light/liteon,ap3216c.yaml
```

binding 的职责是：

- 约束 `compatible`、`reg`、`interrupts` 等必要属性。
- 使用通用 I2C 和 IIO binding 语义。
- 防止 DTS 中出现未声明属性或错误属性类型。
- 为后续上游化、静态检查和平台迁移提供结构化接口。

已完成的检查包括：

- YAML 语法检查；
- `dt_doc_validate`；
- binding checkpatch；
- DTS/driver/binding 一致性核验。

binding 通过不等于板端功能通过，因此最终验收仍以 boot 后真实读数、IRQ 和 IIO Event 为准。

## 8. Kconfig、Makefile 与 defconfig

### 8.1 Kconfig

配置项位于：

```text
kernel/drivers/iio/light/Kconfig
```

核心依赖：

```text
config AP3216C
    depends on I2C
    select REGMAP_I2C
```

设计依据：

- AP3216C 通过 I2C 通信，因此必须依赖 `I2C`。
- 驱动使用 regmap I2C 后端，因此选择 `REGMAP_I2C`。
- 传感器作为 IIO light/proximity 类设备纳入 `drivers/iio/light`。

### 8.2 Makefile

对象编译关系位于：

```text
kernel/drivers/iio/light/Makefile
```

最终规则：

```make
obj-$(CONFIG_AP3216C) += ap3216c.o
```

### 8.3 正式 defconfig

正式配置文件：

```text
kernel/arch/arm64/configs/rockchip_linux_defconfig
```

最终配置：

```text
CONFIG_AP3216C=y
```

选择 built-in 而不是模块的原因：

- 传感器在系统启动阶段即可完成 probe。
- 避免 rootfs 模块加载顺序影响基础传感器功能。
- 与当前板级 IIO 集成策略一致。
- boot 镜像即可携带驱动，不依赖额外 `.ko` 部署。

`quarkpi_ca2_csf_diag_defconfig` 不属于正式构建配置，不应作为交付依据，也不应提交本项目变更。

## 9. 设备树变更边界

最终板级 DTS 同时包含 AP3216C、MPU6500、SHT30 和 UART5 相关配置。因此文档和提交审计必须区分：

- AP3216C 专属内容：`ap3216c@1e`、GPIO4_A1 INT pinctrl、中断属性。
- 共用内容：I2C3 控制器启用与 pinctrl。
- 非 AP3216C 内容：MPU6500、SHT30、UART5 节点和引脚。

不能将整个 DTS diff 直接归因于 AP3216C。最终证据包中保留了完整 DTS、提交 patch 和分项验证日志，以便追踪。

## 10. 构建与验证方法

### 10.1 静态检查

```bash
grep -n "CONFIG_AP3216C" \
  kernel/arch/arm64/configs/rockchip_linux_defconfig

grep -n "AP3216C" \
  kernel/drivers/iio/light/Kconfig \
  kernel/drivers/iio/light/Makefile

grep -n "ap3216c@1e" \
  kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts
```

### 10.2 内核构建

正式构建配置：

```text
RK_KERNEL_CFG=rockchip_linux_defconfig
RK_KERNEL_DTS=kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts
```

最终 boot 镜像：

```text
boot-c0433ef-final-unflashed.img
SHA256=3253eb0b4130fe3a715e3592af760c043a4240f610ac659d301b3d43e1de9ca8
```

### 10.3 板端枚举

```bash
for D in /sys/bus/iio/devices/iio:device*; do
    [ -f "$D/name" ] && echo "$D $(cat "$D/name")"
done
```

应出现：

```text
ap3216c
```

### 10.4 三通道读取

```bash
cat "$AP/in_illuminance_raw"
cat "$AP/in_proximity_raw"
cat "$AP/in_intensity_ir_raw"
```

### 10.5 中断验证

```bash
grep -i ap3216c /proc/interrupts
```

靠近和移开目标物后，IRQ 计数应增长。

## 11. 验收结论

| 检查项 | 结果 |
|---|---|
| I2C3 节点启用 | PASS |
| `compatible`/`reg` | PASS |
| GPIO4_A1 中断映射 | PASS |
| 下降沿触发 | PASS |
| pinctrl 上拉 | PASS |
| binding | PASS |
| Kconfig/Makefile | PASS |
| 正式 defconfig | PASS |
| boot 编译与刷写 | PASS |
| 板端 IIO 枚举 | PASS |
| IRQ 实际增长 | PASS |

## 12. 维护约束

后续修改必须遵守：

1. 地址、GPIO、IRQ 极性、阈值格式和寄存器位定义以 datasheet、SDK 源码和板端实测为准。
2. 不用用户态临时实例化替代正式 DTS。
3. 不把诊断 defconfig 当作正式配置。
4. 修改 DTS 后必须重新完成 dtc、boot 编译、板端枚举和 IRQ 验证。
5. 修改 IRQ 极性时必须同步检查 pinctrl、驱动事件逻辑和硬件空闲电平。
6. AP3216C 完成状态不得扩展解释为整个传感器栈全部通过。
