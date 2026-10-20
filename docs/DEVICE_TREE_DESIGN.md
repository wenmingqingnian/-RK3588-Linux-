# DEVICE_TREE_DESIGN.md

## 1. 文档目的与证据边界

本文记录 RK3588S CarTerminal 项目第 4 周传感器接入涉及的设备树设计，包括 I2C3、MPU6500、SHT30（SHT3x 驱动）和 MPU6500 中断引脚。

本文遵循以下事实边界：

- 板级设备树文件为：

  ```text
  <HOME>/quarkpi-sdk-clean/kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts
  ```

- 已由源码、生成配置、板端 sysfs/IIO 枚举和运行日志确认的内容写为确定事实。
- “与出厂 DTS 的 diff”部分记录**已确认的语义变更**。由于当前文档生成环境没有保存一份可逐字比对的出厂 DTS 文件副本，因此不把人工重构内容冒充为原始 `git diff`。
- 精确逐行补丁应在 SDK 工作区内使用本文给出的审计命令生成并归档。

本文覆盖的设备树修改范围是第 4 周已验证的传感器接入，不对项目中未提供证据的其他历史设备树修改作推断。

---

## 2. 设计目标

本次设备树修改的目标是：

1. 在 QuarkPi-CA2 板级 DTS 中启用 I2C3；
2. 将 I2C3 复用到开发板实际引出的 M1 引脚组；
3. 在 I2C3 下声明：
   - I2C 地址 `0x68` 的 MPU6500；
   - I2C 地址 `0x44` 的 SHT30；
4. 将 MPU6500 的 Data Ready 中断接入 GPIO3_C1；
5. 使 Linux 内核分别绑定：
   - `inv_mpu6050` IIO 驱动；
   - `sht31` IIO 湿度/温度驱动；
6. 让应用通过标准 IIO ABI 使用传感器，而不是依赖未声明的用户态总线探测。

---

## 3. 为什么直接修改板级 DTS

### 3.1 修改位置

本项目直接在以下板级文件中增加 I2C3 和传感器节点：

```text
kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts
```

没有为了本次改动额外 include：

```text
rk3588s-atk-40-pins.dtsi
```

### 3.2 设计理由

本次需求只涉及一组明确的硬件资源：

- I2C3 M1 的 SCL/SDA；
- GPIO3_C1 的 MPU6500 中断；
- 两个固定 I2C 从设备。

直接在最终板级 DTS 中增加最小覆盖具有以下优点：

1. **变更范围可控**  
   只启用本项目实际使用的控制器和子节点，不引入完整 40-pin 扩展配置中的其他外设状态或 pinmux 选择。

2. **有效配置可直接审计**  
   查看 `rk3588s-atk-quarkpi-ca2.dts` 即可确认 I2C3、两个从设备和中断绑定，不必继续追踪额外 include 的覆盖顺序。

3. **减少 pinmux 冲突来源**  
   GPIO 和复用功能由一个板级变更集中声明，便于检查 GPIO3_C1 是否被 UART5_TX_M1 等其他功能占用。

4. **符合板级差异归属**  
   RK3588S SoC 级 `.dtsi` 不应承载特定开发板外接传感器。传感器接线属于 QuarkPi-CA2 项目的板级硬件事实。

这不是否定 `rk3588s-atk-40-pins.dtsi` 的一般用途，而是本项目采用“最小、显式、可审计”的配置策略。

---

## 4. I2C3 总线设计

### 4.1 有效配置

项目使用的 I2C3 配置核心为：

```dts
&i2c3 {
	status = "okay";
	pinctrl-names = "default";
	pinctrl-0 = <&i2c3m1_xfer>;

	/* MPU6500 与 SHT30 子节点，见后文 */
};
```

### 4.2 为什么选择 `i2c3m1_xfer`

RK3588S 的同一个 I2C 控制器可以有多组 pinmux。`i2c3m1_xfer` 表示 I2C3 的 M1 引脚组。

QuarkPi-CA2 硬件接口资料中，扩展接口对应的信号名称为：

```text
I2C3_SCL_M1
I2C3_SDA_M1
```

因此选择：

```dts
pinctrl-0 = <&i2c3m1_xfer>;
```

是由实际 PCB/连接器布线决定的，而不是任意选择。改成 I2C3 的其他 mux 组，即使控制器状态为 `okay`，也不会把 SCL/SDA 输出到本项目接线所在的引脚。

### 4.3 总线地址分配

| 设备 | 7 位 I2C 地址 | Linux client 名称 |
|---|---:|---|
| MPU6500 | `0x68` | `3-0068` |
| SHT30 | `0x44` | `3-0044` |

两个地址不同，可共享 I2C3，不存在地址冲突。

---

## 5. MPU6500 子节点

### 5.1 项目配置

项目的 MPU6500 节点设计如下：

```dts
mpu6500@68 {
	compatible = "invensense,mpu6500";
	reg = <0x68>;

	interrupt-parent = <&gpio3>;
	interrupts = <RK_PC1 IRQ_TYPE_EDGE_RISING>;
};
```

> 注：上述片段记录本项目已采用的绑定语义。最终提交前仍应使用第 9.2 节命令从当前 SDK 工作区导出精确逐行 diff，防止文档与后续代码修改脱节。

### 5.2 属性说明

#### `compatible = "invensense,mpu6500"`

该字符串用于匹配 Linux `inv_mpu6050` 驱动中的 MPU6500 设备类型。驱动名称沿用 `mpu6050` 家族命名，并不表示板上芯片是 MPU6050。

板端最终枚举名称已确认为：

```text
/sys/bus/iio/devices/iio:device2/name = mpu6500
```

#### `reg = <0x68>`

表示 MPU6500 在 I2C3 上使用 7 位地址 `0x68`。板端对应 client 为：

```text
/sys/bus/i2c/devices/3-0068
```

#### `interrupt-parent = <&gpio3>`

中断信号由 GPIO3 控制器接收。

#### `interrupts = <RK_PC1 IRQ_TYPE_EDGE_RISING>`

- `RK_PC1` 对应 GPIO3_C1；
- 使用上升沿触发 Data Ready 事件；
- 板端 `/proc/interrupts` 中该中断显示为 `Edge`，handler 名称为 `inv_mpu`；
- 已验证中断随 50 Hz 采样持续增长。

中断极性必须与 MPU6500 INT 输出配置和实际电气连接一致。不得仅为了消除 probe 问题随意改成电平触发或相反边沿。

---

## 6. SHT30 子节点

### 6.1 项目配置

```dts
sht30@44 {
	compatible = "sensirion,sht3x";
	reg = <0x44>;
};
```

### 6.2 属性说明

#### `compatible = "sensirion,sht3x"`

该 compatible 由 Linux SHT3x 系列驱动匹配。项目内核启用的是 `CONFIG_SHT31`，其驱动支持 SHT30/SHT31/SHT35 系列。

设备树节点名使用 `sht30@44` 描述实际模块和地址；驱动枚举名为：

```text
sht3x
```

二者并不矛盾。

#### `reg = <0x44>`

表示 SHT30 的 7 位 I2C 地址为 `0x44`。

板端已验证：

```text
/sys/bus/iio/devices/iio:device1/name = sht3x
```

并且已实际读取温度和相对湿度工程值，不是只完成空节点枚举。

---

## 7. GPIO3_C1 与 J11 Pin 7

项目接线关系为：

| 层级 | 名称 |
|---|---|
| MPU6500 模块 | `INT` |
| QuarkPi-CA2 扩展口 | J11 Pin 7 |
| 板级通用标识 | `GPIO07` |
| RK3588S GPIO | `GPIO3_C1` |
| DTS 中断单元 | `RK_PC1`，父控制器 `gpio3` |

即：

```text
MPU6500 INT
    → J11 Pin 7 / GPIO07
    → SoC GPIO3_C1
    → gpio3 interrupt controller
    → inv_mpu interrupt handler
```

硬件接口资料同时显示 GPIO3_C1 可复用为 `UART5_TX_M1`。因此本项目使用该引脚作为 MPU6500 中断时，不应再启用占用同一 pad 的 UART5_TX_M1 pinmux。

---

## 8. 最终板级 DTS 结构摘要

以下片段用于说明本项目的有效结构：

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

不要在另一个 overlay 或 dtsi 中重复声明同地址节点，否则可能出现节点合并结果不透明、重复 client 或 pinctrl 覆盖问题。

---

## 9. 与出厂 DTS 的差异

### 9.1 已确认的语义差异

相对于未接入这两个传感器的出厂板级配置，本项目完成了以下变更：

1. 启用 `&i2c3`；
2. 指定 I2C3 使用 `i2c3m1_xfer`；
3. 增加 `mpu6500@68`：
   - `compatible = "invensense,mpu6500"`；
   - `reg = <0x68>`；
   - GPIO3_C1 上升沿中断；
4. 增加 `sht30@44`：
   - `compatible = "sensirion,sht3x"`；
   - `reg = <0x44>`；
5. 没有新增 `#include "rk3588s-atk-40-pins.dtsi"`。

可将语义变更概括为：

```diff
+ &i2c3 {
+     status = "okay";
+     pinctrl-names = "default";
+     pinctrl-0 = <&i2c3m1_xfer>;
+
+     mpu6500@68 {
+         compatible = "invensense,mpu6500";
+         reg = <0x68>;
+         interrupt-parent = <&gpio3>;
+         interrupts = <RK_PC1 IRQ_TYPE_EDGE_RISING>;
+     };
+
+     sht30@44 {
+         compatible = "sensirion,sht3x";
+         reg = <0x44>;
+     };
+ };
```

该代码块是**语义差异摘要**，不是伪装成由 Git 直接导出的逐字补丁。

### 9.2 生成真实逐行 diff

在 SDK 仓库中执行：

```bash
SDK=<HOME>/quarkpi-sdk-clean
DTS=kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-quarkpi-ca2.dts

git -C "$SDK" status --short -- "$DTS"
git -C "$SDK" diff -- "$DTS"
git -C "$SDK" diff --check -- "$DTS"
```

归档：

```bash
EVIDENCE=<HOME>/bsp-evidence/task-3.5/device-tree-design
mkdir -p "$EVIDENCE"

git -C "$SDK" diff -- "$DTS" \
	> "$EVIDENCE/rk3588s-atk-quarkpi-ca2.dts.diff"

sha256sum "$EVIDENCE/rk3588s-atk-quarkpi-ca2.dts.diff" \
	> "$EVIDENCE/rk3588s-atk-quarkpi-ca2.dts.diff.sha256"
```

如果 SDK 目录不是 Git 仓库，应使用保留的出厂 DTS 副本执行：

```bash
diff -u \
	rk3588s-atk-quarkpi-ca2.factory.dts \
	rk3588s-atk-quarkpi-ca2.dts
```

---

## 10. 构建验证

### 10.1 配置检查

```bash
SDK=<HOME>/quarkpi-sdk-clean

grep -E \
	'CONFIG_INV_MPU6050_IIO|CONFIG_INV_MPU6050_I2C|CONFIG_SHT31' \
	"$SDK/kernel/.config"

grep -E \
	'CONFIG_INV_MPU6050_IIO|CONFIG_INV_MPU6050_I2C|CONFIG_SHT31' \
	"$SDK/kernel/include/generated/autoconf.h"
```

已验证的生成配置包括：

```text
CONFIG_INV_MPU6050_IIO=1
CONFIG_INV_MPU6050_I2C=1
CONFIG_SHT31=1
```

### 10.2 DTS/DTB 构建

按 SDK 的现有构建入口编译内核：

```bash
cd <HOME>/quarkpi-sdk-clean
./build.sh kernel
```

构建后检查最终展开的临时 DTS 或反编译 DTB，确认节点没有被其他 include 覆盖：

```bash
grep -RInE \
	'i2c3|mpu6500@68|invensense,mpu6500|sht30@44|sensirion,sht3x' \
	kernel/arch/arm64/boot/dts/rockchip
```

---

## 11. 板端验证方法

### 11.1 设备树节点存在性

使用 `find` 避免依赖 `/proc/device-tree` 中控制器节点的绝对路径：

```bash
find /proc/device-tree \
	\( -name 'mpu6500@68' -o -name 'sht30@44' \) \
	-print
```

读取 compatible：

```bash
for node in $(find /proc/device-tree \
	\( -name 'mpu6500@68' -o -name 'sht30@44' \)); do
	echo "NODE=$node"
	tr '\0' '\n' < "$node/compatible"
done
```

### 11.2 I2C client 枚举

```bash
ls -ld \
	/sys/bus/i2c/devices/3-0068 \
	/sys/bus/i2c/devices/3-0044
```

MPU6500 驱动绑定：

```bash
readlink -f /sys/bus/i2c/devices/3-0068/driver
```

预期：

```text
/sys/bus/i2c/drivers/inv-mpu6050-i2c
```

### 11.3 IIO 设备枚举

```bash
for dev in /sys/bus/iio/devices/iio:device*; do
	[ -f "$dev/name" ] || continue
	printf '%s: ' "$dev"
	cat "$dev/name"
done
```

本项目已验证的名称为：

```text
sht3x
mpu6500
```

不要把 `iio:device1` 或 `iio:device2` 编号写死到业务逻辑中。编号会随 probe 顺序变化，应按 `name` 查找。

### 11.4 MPU6500 中断

```bash
grep -E 'inv_mpu|inv-mpu' /proc/interrupts
```

项目验收中已观察到：

- 中断 handler：`inv_mpu`；
- 类型：`Edge`；
- 50 Hz 运行时中断计数持续增长；
- 受控测试中的 IRQ 频率约为 `50.0165 Hz`。

### 11.5 IIO trigger 与 buffer

查找 MPU6500：

```bash
MPU_IIO="$(
	for dev in /sys/bus/iio/devices/iio:device*; do
		[ -f "$dev/name" ] || continue
		[ "$(cat "$dev/name")" = "mpu6500" ] || continue
		echo "$dev"
		break
	done
)"

echo "MPU_IIO=$MPU_IIO"
cat "$MPU_IIO/sampling_frequency"
cat "$MPU_IIO/trigger/current_trigger"
cat "$MPU_IIO/buffer/enable"
```

应用正常运行时已验证：

```text
sampling_frequency = 50
current_trigger    = mpu6500-dev2
buffer/enable      = 1
```

其中 trigger 名称中的设备编号同样可能随枚举顺序变化。

---

## 12. 资源所有权与回退原则

### 正常生产状态

```text
DTS 声明 MPU6500
→ inv_mpu6050 内核驱动绑定 3-0068
→ IIO trigger/buffer 工作
→ CarTerminal 打开 /dev/iio:deviceX
```

此时应用不得再使用 `I2C_SLAVE_FORCE` 直接抢占 `0x68`。

### 用户态 I2C 诊断状态

仅在 A/B 或驱动诊断时：

1. 停止 CarTerminal；
2. 确认 IIO buffer 已关闭；
3. 解绑 `inv-mpu6050-i2c`；
4. 启动 `user_i2c` 后端；
5. 测试结束后停止应用；
6. 恢复配置并重新绑定内核驱动。

不得让内核驱动和用户态寄存器访问同时控制同一个 MPU6500。

---

## 13. 已验证结论

截至第 4 周验收：

- I2C3 M1 引脚组工作正常；
- MPU6500 在 `0x68` 完成内核驱动绑定；
- SHT30 在 `0x44` 完成 SHT3x IIO 枚举；
- GPIO3_C1 中断有效；
- MPU6500 trigger、buffer 和 50 Hz 采样有效；
- 应用已通过 IIO 字符设备持续读取数据；
- 设备树方案不依赖额外 include `rk3588s-atk-40-pins.dtsi`。
