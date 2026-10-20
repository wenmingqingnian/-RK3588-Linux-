# 项目路线（Project Roadmap）

> 基于"RK3588S AI 车载多媒体座舱终端"项目蓝图与现有进度，面向嵌入式 Linux 驱动岗位秋招
> 制定的 12 周阶段化任务执行顺序。
>
> 创建：2026-07-20
> 重写：2026-07-21（采用"软件基线先行 + 驱动应用交替集成"的工程化方案）
> 核心原则：每项独立可验证，前后不冲突，每项完成后立即板端验收 + 写入 MODULE_STATUS.md + 同步更新简历

---

## 一、项目现状盘点

### 已完成（作为驱动开发的验收基线，保留）

| 模块 | 状态 | 简历价值 |
|---|---|---|
| GPS（ATGM336H + UART5 + 设备树 overlay） | ✓ 硬件验收 | GNSS 数据接入 |
| 单路 IMX415 + V4L2 + RGA + MPP + RTSP | ✓ 硬件验收 | 视频全链路（驱动验收平台） |
| sherpa-onnx ASR + SummerTTS + KWS | ✓ 板端可用 | 语音交互（保留现状） |
| 百度地图 + WGS84 | ✓ 板端可用 | 地图集成 |
| Qt5 GUI（多页面） | ✓ 板端可用 | 驱动数据展示终端 |
| buzzer | ✓ 板端可用 | 用户态 GPIO 控制基线 |
| DHT11 | ❌ 已放弃（单总线时序脆弱） | 替换为 SHT30（标准 I2C） |
| SHT30 | 待引入 | 主线 IIO 驱动 sht31.c + DTS 使能 |
| AP3216C | △ 用户态适配完成，硬件/内核驱动未验收 | 自研 IIO 驱动目标 |
| FrameBufferPool + ThreadPool + VLOG | ✓ 板端可用 | 基础设施 |
| 应用层缓存 IDR + forceIDR 机制 | ✓ 板端可用 | 多线程设计 |
| RKNN 检测联动 + ZMQ 优雅退出 | ✓ 板端可用 | 端侧 AI 接入 |
| MPU6500 用户态 I2C 驱动 | ✓ 板端可用 | I2C 协议理解（待升级为 IIO） |
| 视频录像（FFmpeg muxer） | ✓ 板端可用 | 多媒体闭环 |

### 新计划定位

> 现有 Qt GUI、IMX415、RKNN、MPP、RTSP、ASR/TTS **全部作为驱动验收平台保留**，不再扩展功能。
> 主线工作切换为：**自编译 BSP + 内核驱动开发 + 设备树 + 内核剪裁 + 异构通信**。

---

## 二、12 周计划总览

| 阶段 | 周次 | 主要任务 | 软件是否参与 |
|------|------|---------|------------|
| **P0 前置** | 第1周 | 出厂镜像软件基线封板 | 软件主任务 |
| **P0** | 第2-3周 | 自编译 BSP（U-Boot/Kernel/DTB/RootFS/SD 启动） | 用基线软件回归 |
| **P1** | 第4周 | MPU6500 IIO 适配 | Qt 改 IIO Buffer 读取（引入 IMU 专用数据源接口） |
| **P1** | 第5-6周 | 自研 AP3216C IIO 驱动 | 环境页面、阈值告警 |
| **P1** | 第7周 | vehicle-io platform/Input 驱动 | 倒车/ACC 事件接入 Qt |
| **P2** | 第8-9周 | 第二种 Sensor V4L2 驱动移植 | 复用并扩展现有 FrameBroker/MediaPipeline |
| **P2** | 第10周 | 异构双摄 + 倒车切换 | 软件重点集成 |
| **P4** | 第11周 | 驱动回归、故障注入、性能与长期稳定性 | 软件与内核统一验收 |
| **P4** | 第12周 | 8小时测试、文档、简历与面试材料 | 项目收口 |
| **P3（机动）** | 仅提前 ≥5 个工作日时 | AMP RPMsg 最小闭环 | 只做 RTOS 启动 + ping/pong，不挤占 P4 |

---

## 三、停止线（递进式求职底线）

| 截止时间 | 必须达到 | 简历可写 |
|---------|---------|---------|
| 第6周末 | 自编译 BSP + MPU6500 IIO 适配 + AP3216C 自研驱动 | Linux I2C/IIO 驱动闭环 |
| 第7周末 | 上面 + Vehicle-IO platform/Input | 加 Platform/GPIO IRQ/Input |
| 第9周末 | 上面 + 第二种 Sensor 单路出图 | 加 V4L2/Media Graph |
| 第10周末 | 上面 + 双摄 + 倒车链路 | 加端到端集成 |
| 第12周末 | 上面 + 稳定性 + 性能 + 文档 | 完整秋招项目；AMP仅按实际完成情况加分 |

---

## 四、详细周次分解

### 第0阶段：出厂镜像软件基线封板（第1周，5天）

**严格不超 5 天，不做架构抽象（YAGNI）。**

#### 主链路验证清单

| 序号 | 验证项 | 命令/方法 | 通过标准 |
|------|--------|----------|---------|
| 1 | IMX415 真实采集 | `v4l2-ctl -d /dev/video33 --stream-mmap --stream-count=30` | 无错误 |
| 2 | Qt 显示 | 启动 CarTerminal，Monitor 页 | 画面正常 |
| 3 | RGA 转换 | 日志 `RGA convert OK` | 无失败 |
| 4 | RKNN 推理 | 日志 `detect: N objects` | 真实检测框 |
| 5 | MPP 编码 | 日志 `MPP encode OK` | 无失败 |
| 6 | RTSP 推流 | VLC 拉流 `rtsp://board-ip:8554/live` | 1s 内出图 |
| 7 | MP4 录像 | `record/` 目录生成 mp4 | ffprobe 能解析 |
| 8 | GPS 串口 | `cat /dev/ttyS5` | NMEA 报文 |
| 9 | MPU6500 用户态 | Qt 姿态页 | pitch/roll 有变化 |
| 10 | ASR 固定指令 | "打开监控" | GUI 切换 |
| 11 | 启动/退出 | 启动+正常退出 3 次 | 无崩溃/泄漏 |

#### 验收测试

单路 IMX415 + RKNN + RTSP + Qt 连续运行 30 分钟：
- 无崩溃
- 无新增、可复现的 ERROR/WARN 刷屏；已知无害告警单独列入基线清单
- 正常退出；单独使用 `valgrind --error-exitcode=99` 检查可执行的软件路径，不用其结果评价硬件加速性能

#### 产物

```bash
git tag factory-image-software-baseline
git push origin factory-image-software-baseline
```

保留：
- 当前可执行文件 `~/RK3588S_CarTerminal/CarTerminal`
- 当前配置文件 `config/*.conf`
- 30 分钟运行日志 `logs/baseline-*.log`
- 出厂系统设备节点清单 `docs/baseline/devices.txt`
- 各功能启用状态表 `docs/baseline/features.md`
- 第二种 Sensor 的采购与硬件可行性表：接口定义、1.8V 电平、电源、MCLK、RESET/PWDN、lane mapping、公开 datasheet/驱动/IQ 文件

#### 本周不做项

- 不做自由对话 LLM
- 不扩展语音语义
- 不做双路 RTSP/RKNN
- 不美化 GUI
- 不重构 Monitor 上帝对象
- 不追求所有模块同时启用

#### 风险止损

如果第5天仍未跑通主链路，提交一个明确标注的 WIP/partial-baseline commit 并记录未通过项，不依赖易遗忘的 `git stash`。不能让基线封板拖死后续驱动开发。

---

### 第2-3周：自编译 BSP 镜像（2周）

**目标：自己的 U-Boot + Kernel + DTB + RootFS 跑起来，能 SSH 登录、加载自定义 .ko，并完成基线软件回归测试。**

**SDK 基线（2026-07-21 验证）**：
- 路径：`<HOME>/quarkpi-sdk-clean`
- 内核版本：Linux 5.10.209
- 工具链：aarch64-linux-gnu-gcc 9.4.0
- 已就绪：I2C3/UART5 已启用、IMX415 三路 DTS 已配置、RKNN/RGA/DRM/V4L2 驱动全部 =y
- 需改动：MPU6500 IIO 驱动未启用、SHT30 驱动不存在、Buildroot 依赖包未启用

#### 第2周任务：内核 + U-Boot

> **核心原则：先编译原始镜像 → 再修改 → 重新编译**
>
> 绝对不要直接改 SDK 再编译。正确流程是两步走：
> 1. 第1步：用 SDK 原始配置编译一次，验证编译环境和 SDK 完整性
> 2. 第2步：在原始镜像验证通过后，再修改 defconfig/DTS/源码，重新编译
>
> 这样做的目的：编译失败时能立刻定位是 SDK 问题还是你的修改问题，避免排查无门。

##### 任务 2.0：原始 SDK 编译验证（最先做，不改任何配置）

**优先级**：P0（所有后续任务的前置条件）
**目的**：验证 SDK 完整性 + 工具链 + 烧录流程，得到基准镜像
**预计耗时**：1-2 小时（首次编译）
**产出**：可启动的原始 SD 卡镜像

```bash
cd ~/quarkpi-sdk-clean

# 不改任何东西，直接用 SDK 默认配置编译
./build.sh kernel      # 编译内核
./build.sh uboot       # 编译 U-Boot
./build.sh buildroot   # 编译 rootfs
./build.sh firmware    # 打包镜像

# 烧录到 SD 卡（eMMC 保持不动）
sudo ./rkflash.sh
```

**验证标准**：
- ✅ 内核编译无错误 → `Image` + `rk3588s-atk-quarkpi-ca2.dtb` 生成
- ✅ U-Boot 编译无错误 → `uboot.img` + `idbloader.img` 生成
- ✅ Buildroot 编译无错误 → `rootfs.ext4` 生成
- ✅ SD 卡烧录成功 → 板端能启动到 login prompt
- ✅ SSH 能登录
- ✅ `ls /dev/i2c-* /dev/ttyS* /dev/video* /dev/gpiochip*` 基础设备存在

**失败处理**：
- 如果原始 SDK 编译失败 → SDK 本身有问题，联系供应商或换 SDK 版本
- 如果烧录失败 → 检查 SD 卡、烧录工具、启动开关
- 如果启动失败 → 检查 U-Boot bootcmd、内核 cmdline、rootfs 挂载

**关键产出**：得到一个"基准镜像"，后续所有修改都以此为对比基础。

##### 任务 2.1：确认 PCB 原理图（阻塞项，可与 2.0 并行）

**优先级**：P0（阻塞 DTS 修改）
**产出**：3 个 GPIO/I2C 信息记录

- 查 ATK QuarkPi-CA2 原理图
- 确认 MPU6500 中断引脚 GPIO 编号
- 确认 AP3216C 接的 I2C 总线和地址
- 确认 SHT30 接的 I2C 总线和地址

##### 任务 2.2：修改内核 defconfig

**优先级**：P0
**依赖**：任务 2.3 完成（SHT31 CONFIG 依赖源文件存在）
**产出**：defconfig diff 补丁

修改 `arch/arm64/configs/rockchip_linux_defconfig`：

```
# 必加（启用 MPU6500 IIO 驱动）
CONFIG_INV_MPU6050_IIO=y
CONFIG_INV_MPU6050_I2C=y

# 必加（启用 SHT30 驱动，依赖 2.3 backport）
CONFIG_SHT31=y

# 必确认（日志显示仅 gki.config =m，需在主 defconfig 中设为 =y）
CONFIG_GPIO_ROCKCHIP=y

# 必确认（IIO 基础设施，通常已被 select，但要验证）
CONFIG_IIO=y
CONFIG_IIO_BUFFER=y
CONFIG_IIO_TRIGGERED_BUFFER=y
CONFIG_REGMAP=y
CONFIG_REGMAP_I2C=y
```

##### 任务 2.3：Backport sht31.c 驱动

**优先级**：P0（可与 2.1 并行，独立任务）
**产出**：3 个文件改动

- 从主线内核 5.15+ 获取 `sht31.c` 源文件
- 放到 `drivers/iio/humidity/sht31.c`
- 修改 `drivers/iio/humidity/Kconfig` 添加：
  ```
  config SHT31
      tristate "Sensirion SHT3x humidity and temperature sensor"
      depends on I2C
      select REGMAP_I2C
      help
        ...
  ```
- 修改 `drivers/iio/humidity/Makefile` 添加：
  ```
  obj-$(CONFIG_SHT31) += sht31.o
  ```
- 验证：编译不报错

##### 任务 2.4：修改设备树 DTS

**优先级**：P0
**产出**：DTS diff 补丁

**硬件实测修正（2026-07-22）**：

| 问题 | 原计划 | 修正后 |
|------|--------|--------|
| compatible 写错 | `invensense,mpu6050` | `invensense,mpu6500`（WHO_AM_I=0x70 对应 MPU6500） |
| INT 未接线 | 写了占位中断 GPIO0_C0 | 不写 interrupts，只写最小节点 |
| 40-pins.dtsi 带副作用 | `#include "rk3588s-atk-40-pins.dtsi"` | 不 include 整个文件（会顺带启用 SPI0 等），直接在板级 DTS 中只启用 I2C3/UART5 |
| 内核 IIO 与用户态冲突 | 未提及 | 选择方案A（内核 IIO），后续改 CarTerminal 读 IIO，不再直接操作 /dev/i2c-3 |

**2.4.1**：修改 `rk3588s-atk-quarkpi-ca2.dts`，在文件末尾添加 I2C3/UART5 启用 + 传感器子节点：

```dts
/* 不 include rk3588s-atk-40-pins.dtsi（会带 SPI0 等副作用） */
/* 直接在板级 DTS 中只启用所需的 I2C3 和 UART5 */

&i2c3 {
	pinctrl-names = "default";
	pinctrl-0 = <&i2c3m1_xfer>;
	status = "okay";

	imu@68 {
		compatible = "invensense,mpu6500";
		reg = <0x68>;
		/* INT 尚未接线，暂不声明 interrupts */
		/* 后续 INT 接到 GPIO3_C1 后添加：
		 * interrupt-parent = <&gpio3>;
		 * interrupts = <RK_PC1 IRQ_TYPE_EDGE_RISING>;
		 */
	};

	sht30@44 {
		compatible = "sensirion,sht3x";
		reg = <0x44>;
	};
};

&uart5 {
	pinctrl-names = "default";
	pinctrl-0 = <&uart5m1_xfer>;
	status = "okay";
};
```

**验证记录**：
- `invensense,mpu6500` 在 `drivers/iio/imu/inv_mpu6050/inv_mpu_i2c.c:200` 已确认
- `sensirion,sht3x` 在 `drivers/iio/humidity/sht31.c:242` 已确认
- `rk3588s-atk-40-pins.dtsi` 未被任何 dtsi include（grep 确认）
- sht31.c 是 IIO 驱动（devm_iio_device_alloc + devm_iio_device_register），不是 hwmon

**2.4.2**：AP3216C 子节点（外接，第5-6周自研驱动时添加）

QuarkPi-CA2 无板载 AP3216C，需外接到 I2C3（与 MPU6500/SHT30 共用总线，地址不冲突）：

| 设备 | I2C3 地址 | 接线 |
|------|---------|------|
| MPU6500 | 0x68 | SDA→Pin3, SCL→Pin5 |
| SHT30 | 0x44 | SDA→Pin3, SCL→Pin5 |
| AP3216C | 0x1E | SDA→Pin3, SCL→Pin5 |

```dts
ap3216c@1e {
    compatible = "liteon,ap3216c";  /* 添加前确认 SDK 驱动接受此 compatible */
    reg = <0x1e>;
};
```

**2.4.3**：MPU6500 中断引脚（待 INT 实际接到 GPIO3_C1 后添加）

推荐接线：MPU INT → J11 Pin 7 → GPIO3_C1

接好后添加：
```dts
/* imu@68 节点内添加 */
interrupt-parent = <&gpio3>;
interrupts = <RK_PC1 IRQ_TYPE_EDGE_RISING>;

/* pinctrl 添加 */
&pinctrl {
    mpu6500 {
        mpu6500_int_pin: mpu6500-int-pin {
            rockchip,pins = <3 RK_PC1 RK_FUNC_GPIO &pcfg_pull_none>;
        };
    };
};
```

**2.4.4**：UART5 / GPS 接线确认

| 信号 | J11 引脚 | RK3588S GPIO | 方向 |
|------|---------|-------------|------|
| UART5_TX_M1 | Pin 8 | GPIO3_C4 | 板→GPS |
| UART5_RX_M1 | Pin 10 | GPIO3_C5 | GPS→板 |

GPS 交叉连接：GPS_TXD→Pin10(RX), GPS_RXD→Pin8(TX), GND→GND

`/dev/ttyS5` 不存在的原因：DTS 中 `serial@feb80000` 的 status 为 `disabled`。
启用 `&uart5 { status = "okay"; }` 后验证：
```bash
tr -d '\0' </proc/device-tree/serial@feb80000/status  # 预期: okay
ls -l /dev/ttyS5                                        # 预期: 存在
dmesg | grep -iE 'feb80000|ttyS5'                      # 预期: ttyS5 is a 16550A
```

**架构选择**：方案A - 内核 IIO 驱动

启用 `CONFIG_INV_MPU6050_IIO=y` + DTS 添加 `imu@68` 后，内核 inv_mpu6050 驱动会绑定 0x68 地址。
此时 `i2cdetect` 显示 `UU`，CarTerminal 的用户态 `/dev/i2c-3` 直接访问将得到 `EBUSY`。
后续第4周需将 CarTerminal 从 `/dev/i2c-3` 改为读取 `/sys/bus/iio/devices/`。

##### 任务 2.5：重新编译内核（修改后）

**优先级**：P1
**依赖**：任务 2.0（原始编译验证通过）+ 2.2 + 2.3 + 2.4 完成
**产出**：`arch/arm64/boot/Image` + `.dtb`（带传感器驱动的新内核）

**编译方式修正**：优先使用 `./build.sh kernel`，不用通用 `make`。
厂商 SDK 通过 build.sh 注入板级配置、工具链和 DTB 目标。

```bash
cd ~/quarkpi-sdk-clean

# 用 SDK 脚本编译（自动处理 defconfig + DTS + 工具链）
./build.sh kernel
```

验证：
- 生成 `arch/arm64/boot/Image` 和 `rk3588s-atk-quarkpi-ca2.dtb`
- `ls drivers/iio/imu/inv_mpu6050/*.o` 确认 MPU6500 编译进内核
- `ls drivers/iio/humidity/sht31.o` 确认 sht31 编译成功
- `ls -la kernel/boot.img` 确认 boot.img 更新
- 对比 2.0 的原始镜像，确认差异只有你改的部分

##### 任务 2.6：U-Boot 启动链研究（仅研究，不重编译）

**优先级**：P2（降级：本次只改内核/DTS/驱动，不需要重编译 U-Boot）
**产出**：U-Boot 启动链文档

本次只修改内核、驱动和 DTS，原则上不需要重新编译 U-Boot。
U-Boot 保持 2.0 原始编译的版本即可。

如需研究启动链（SD 卡 vs eMMC 启动优先级）：

```bash
cd ~/quarkpi-sdk-clean
ls device/rockchip/
cat device/rockchip/common/Makefile | head -50
find . -name "*quarkpi*" -o -name "*ca2*" 2>/dev/null | grep -i uboot
ls device/rockchip/rk3588/
cat device/rockchip/rk3588/rk3588_make.sh 2>/dev/null
```

预期：SDK 应该有板级编译脚本，通过 `./build.sh uboot` 触发，会自动选择对应 defconfig。

##### 任务 2.7：U-Boot 重编译（本次跳过）

**优先级**：P3（降级为可选）
**状态**：本次跳过，不重编译 U-Boot

本次 BSP 修改只涉及内核 defconfig + DTS + sht31.c backport，不影响 U-Boot。
U-Boot 保持 2.0 原始编译的版本即可，I2C3/UART5/MPU6500/SHT30 的启用不需要 U-Boot 参与。

仅在以下场景需要重编译 U-Boot：
- 需要修改启动顺序（SD 卡 vs eMMC 优先级）
- 需要修改 bootargs
- 需要修改 U-Boot 环境变量

#### 第3周任务：Buildroot + 部署

##### 任务 3.1：配置 Buildroot

**优先级**：P0（可与第2周任务并行）
**产出**：更新后的 `rockchip_atk_quarkpi_ca2_defconfig`

```bash
cd ~/quarkpi-sdk-clean/buildroot
make rockchip_atk_quarkpi_ca2_defconfig
make menuconfig
```

需要启用的包：

```
# 1. Qt5（项目 GUI 依赖）
# Target packages → Graphic libraries and applications → qt5
BR2_PACKAGE_QT5=y
BR2_PACKAGE_QT5BASE=y
BR2_PACKAGE_QT5BASE_GUI=y
BR2_PACKAGE_QT5BASE_WIDGETS=y
BR2_PACKAGE_QT5BASE_NETWORK=y
BR2_PACKAGE_QT5BASE_SQL=y

# 2. ffmpeg（MP4 录像依赖）
# Target packages → Multimedia → ffmpeg
BR2_PACKAGE_FFMPEG=y

# 3. alsa-lib（KWS/ASR 音频依赖）
# Target packages → Audio and video applications → alsa-lib
BR2_PACKAGE_ALSA_LIB=y
BR2_PACKAGE_ALSA_UTILS=y

# 4. librga（图像硬件加速依赖）
# Target packages → Libraries → Hardware handling → librga
BR2_PACKAGE_LIBRGA=y

# 5. libzmq（TTS 通信依赖，待确认）
BR2_PACKAGE_ZEROMQ=y
```

##### 任务 3.2：编译 rootfs

**优先级**：P0
**依赖**：任务 3.1 完成
**产出**：`rootfs.ext4`

```bash
cd ~/quarkpi-sdk-clean/buildroot
make -j8
```

##### 任务 3.3：打包镜像

**优先级**：P0
**依赖**：任务 2.5 + 2.7 + 3.2 完成
**产出**：`output/firmware/` 下的完整镜像文件

```bash
cd ~/quarkpi-sdk-clean
./build.sh firmware   # 或 SDK 提供的打包脚本
```

预期产出：
- `idbloader.img`（SPL）
- `uboot.img`（U-Boot）
- `boot.img`（Kernel + DTB + initramfs）
- `rootfs.img`（根文件系统）

##### 任务 3.4：烧录 SD 卡

**优先级**：P0
**依赖**：任务 3.3 完成
**产出**：可启动的 SD 卡
**防变砖**：eMMC 保持出厂镜像完全不动（保险）

```bash
cd ~/quarkpi-sdk-clean
sudo ./rkflash.sh   # 或 SDK 提供的烧录脚本
# eMMC 保持出厂镜像不动
```

启动逻辑：插卡 → SD 卡启动；拔卡 → eMMC 启动
变砖风险：零（SD 卡随便刷，eMMC 永远是保险）

##### 任务 3.5：板端驱动验证

**优先级**：P1
**依赖**：任务 3.4 完成
**产出**：板端验证日志

SD 卡启动后：

```bash
# 验证 MPU6500 IIO
ls /sys/bus/iio/devices/
cat /sys/bus/iio/devices/iio:deviceX/in_accel_x_raw

# 验证 SHT30
cat /sys/bus/iio/devices/iio:deviceY/in_humidityrelative_raw

# 验证 IMX415
v4l2-ctl --list-devices

# 验证 I2C/UART/GPIO
ls /dev/i2c-* /dev/ttyS* /dev/gpiochip*

# 验证 RKNN/RGA/DRM
ls /dev/rknpu* /dev/rga* /dev/dri/card*

# 编译和加载自定义 .ko
insmod hello.ko
dmesg | tail
```

##### 任务 3.6：基线回归测试

**优先级**：P1
**依赖**：任务 3.5 完成
**产出**：回归测试报告

```bash
# 部署 CarTerminal 到 SD 卡 rootfs
# 在 Ubuntu 上交叉编译
cd ~/RK3588S_CarTerminal
git checkout factory-image-software-baseline
/path/to/qmake-aarch64 CarTerminal.pro
make -j8

# 拷贝到 SD 卡 rootfs
sudo cp CarTerminal /mnt/sd_rootfs/opt/carterminal/
sudo cp -r config/ /mnt/sd_rootfs/opt/carterminal/

# 板端运行
cd /opt/carterminal
./CarTerminal
```

验证项（来自 baseline_verify.sh）：
- IMX415 采集 + RGA + RKNN + MPP + RTSP
- MP4 录像
- GPS（ATGM336H, UART5）
- MPU6500 姿态
- ASR + KWS + TTS（sherpa-onnx）
- 百度地图 + Qt5 GUI
- 启动退出 3 次无崩溃

#### 任务依赖关系

```
2.0 原始SDK编译验证（最先做）
         │
         │  2.1 PCB确认（可与2.0并行）
         │      │
         │      ↓
         ├─→ 2.4 DTS修改 ──→ 2.5 重新编译内核
         │                       │
         │  2.3 Backport sht31 ──┘
         │                       │
         │  2.2 defconfig ────────┤
         │                       ↓
         │  2.6 U-Boot确认 ──→ 2.7 编译U-Boot
         │                       │
         ↓                       ↓
3.1 Buildroot配置 ──→ 3.2 编译rootfs ──→ 3.3 打包 → 3.4 烧录
                                                            │
                                                            ↓
                                               3.5 驱动验证 → 3.6 回归测试
```

#### 可并行任务

- **任务 2.0 原始 SDK 编译验证** 与 **任务 2.1 PCB 确认** 可并行（2.0 在 Ubuntu 跑，2.1 查原理图）
- **任务 2.3 Backport sht31** 与 **任务 2.1 PCB 确认** 可并行
- **任务 3.1 Buildroot 配置** 与 **任务 2.5 编译内核** 可并行
- **任务 2.6 U-Boot 确认** 与 **任务 2.4 DTS 修改** 可并行

#### 内核配置必做项

`make ARCH=arm64 menuconfig` 必须启用（基于 SDK 5.10.209 实际核实）：

```
# IIO 子系统（MPU6500/SHT30/AP3216C 依赖）
CONFIG_IIO=y
CONFIG_IIO_BUFFER=y
CONFIG_IIO_TRIGGERED_BUFFER=y
CONFIG_INV_MPU6050_IIO=y          # 启用 MPU6500（SDK 有完整驱动文件）
CONFIG_INV_MPU6050_I2C=y
CONFIG_SHT31=y                    # 启用 SHT30（需先 backport sht31.c）

# regmap（I2C 寄存器访问）
CONFIG_REGMAP=y
CONFIG_REGMAP_I2C=y

# Input 子系统（第7周 vehicle-io）
CONFIG_INPUT=y
CONFIG_INPUT_MISC=y
CONFIG_KEYBOARD_GPIO=y            # SDK 已启用

# Media/V4L2（IMX415 已就绪，SDK 全部 =y）
CONFIG_MEDIA_SUPPORT=y
CONFIG_MEDIA_CAMERA_SUPPORT=y
CONFIG_V4L2_SUBDEV_API=y
CONFIG_VIDEO_ROCKCHIP_ISP=y
CONFIG_VIDEO_ROCKCHIP_CIF=y
CONFIG_VIDEO_IMX415=y             # SDK 已启用

# GPIO（SDK 仅 gki.config =m，需在主 defconfig 确认 =y）
CONFIG_GPIOLIB=y
CONFIG_GPIO_ROCKCHIP=y

# Rockchip 基础（SDK 已全部 =y）
CONFIG_I2C_RK3X=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_DW=y
CONFIG_ROCKCHIP_RKNPU=y
CONFIG_VIDEO_ROCKCHIP_RGA=y
CONFIG_DRM_ROCKCHIP=y

# 调试（仅 debug defconfig）
CONFIG_DEBUG_INFO=y
CONFIG_KMEMLEAK=y                 # 仅 debug 配置
CONFIG_FTRACE=y
CONFIG_FUNCTION_TRACER=y          # 仅 debug 配置
CONFIG_DEBUG_FS=y
CONFIG_DYNAMIC_DEBUG=y            # 仅 debug 配置

# AMP 机动项
CONFIG_RPMSG=y                    # AMP 机动项需要；不执行 AMP 时可不启用
CONFIG_RPMSG_CHAR=y
```

> 所有符号必须先用 SDK 内核的 `scripts/config`、`menuconfig` 或 `grep Kconfig` 核实；Rockchip 厂商内核可能使用不同的 Media/CSI 符号，禁止照抄不存在的 CONFIG。

维护两套配置：`quarkpi_driver_debug_defconfig` 用于 ftrace/kmemleak/dynamic-debug；`quarkpi_car_release_defconfig` 关闭高开销调试并用于性能和长期稳定性测试。

可裁剪项必须基于实际硬件和应用依赖审计：

```
# 保留 NETWORKING/WIRELESS（若通过板载 Wi-Fi SSH）以及 DRM/SOUND（Qt + 音频依赖）
# BT/NFC/USB_GADGET 仅在确认项目与启动链路均不依赖时裁剪
```

#### 最低验收（第3周末）

**第2周验收（原始 BSP）**：
- ✅ 原始 SDK 编译通过（内核 + U-Boot + rootfs 全部无错误）
- ✅ 原始镜像 SD 卡启动到 login prompt
- ✅ SSH 可登录
- ✅ 基础设备存在（I2C/UART/GPIO/Video）

**第3周验收（修改后 BSP + 驱动）**：
- ✅ 自编译 U-Boot 启动
- ✅ 自编译 Kernel 启动
- ✅ 自编译 DTB 生效（`cat /proc/device-tree/model` 正确）
- ✅ 自编译 RootFS 挂载
- ✅ 能编译和加载自定义 .ko
- ✅ MPU6500 IIO 节点出现（`ls /sys/bus/iio/devices/`）
- ✅ SHT30 IIO 节点出现（`cat in_humidityrelative_raw`）
- ✅ IMX415 三路摄像头可用（`v4l2-ctl --list-devices`）
- ✅ 基线软件 CarTerminal 能跑（回归对比通过）

#### 防变砖机制

```
eMMC: 保持出厂镜像完全不动（保险）
SD 卡: 烧录自编译镜像（开发用）

启动逻辑：插卡 → SD 卡启动；拔卡 → eMMC 启动
变砖风险：零（SD 卡随便刷，eMMC 永远是保险）
```

#### 风险项与缓解

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| PCB 原理图找不到 MPU6500 中断引脚 | 中 | MPU6500 无中断，退化为轮询 | 用用户态 I2C 方式跑，或 GPIO 轮询 |
| sht31.c backport 编译失败 | 低 | SHT30 不能用 | 换 si7020.c（SDK 已有） |
| U-Boot defconfig 找不到 | 中 | 无法编译 U-Boot | 用 SDK 的 build.sh 脚本自动处理 |
| Buildroot Qt5 编译失败 | 中 | 无 GUI | 查 Buildroot Qt5 依赖链 |
| SD 卡启动失败 | 低 | 无法验证 | eMMC 出厂镜像可回退 |

#### 风险止损

如果第3周末 Buildroot rootfs 仍有包依赖问题：
1. 优先使用厂家 board defconfig 生成的最小 rootfs，不自行拼装不受控的运行库集合
2. 必要时降级到 BusyBox initramfs + Dropbear，只用于内核/驱动开发，不宣称完成 Qt 应用回归
3. 如需复用出厂库，必须复制完整依赖并验证 ABI/许可证，不能跨独立根文件系统建立无效软链接
4. 第4周可以开始模块开发，但 CarTerminal 回归状态必须继续标记为 blocked，不能伪装通过

#### 预计产出

| 产出物 | 位置 | 用途 |
|--------|------|------|
| 原始镜像（任务 2.0） | SD 卡 | 基准对比基础 |
| 内核 Image + dtb（修改后） | `kernel/arch/arm64/boot/` | SD 卡 boot 分区 |
| uboot.img + idbloader.img | `u-boot/` | SD 卡 bootloader |
| rootfs.ext4 | `buildroot/output/images/` | SD 卡 root 分区 |
| 完整镜像 | `output/firmware/` | SD 卡整体烧录 |
| defconfig.orig 备份 | `arch/arm64/configs/` | 原始配置回退 |
| DTS diff 补丁 | 项目 docs/ | 记录设备树改动 |
| defconfig diff | 项目 docs/ | 记录内核配置改动 |
| 板端验证日志 | 项目 docs/ | 记录验证结果 |
| 回归测试报告 | 项目 docs/ | 确认基线软件可用 |

#### 本周文档输出

- `docs/BSP_BUILD_AND_BOOT.md`：一键编译/部署流程
- `docs/BOARD_BRINGUP.md`：板端启动日志、设备节点清单对比

---

### 第4周：MPU6500 IIO 适配 + IMU 数据源接口（1周）

**项目从"用户态"跨入"内核驱动"的第一步，也是首次引入应用抽象。**

#### 内核任务

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | `CONFIG_INV_MPU6050_IIO=m` 内核配置 | menuconfig 勾选 |
| 2 | 先读取并记录真实 WHO_AM_I，确认是 MPU6500/MPU9250 后再选择 compatible | 型号证据与 DTS 一致 |
| 3 | 设备树 I²C 子节点 + INT GPIO | 使用经当前内核 binding 核实的 compatible |
| 4 | pinctrl + regulator 描述 | dts 编译通过 |
| 5 | 驱动绑定 + WHO_AM_I 调试 | driver symlink、IIO name/channel 与真实型号一致 |
| 6 | IIO channel 定义 | `iio_info` 枚举到设备 |
| 7 | Data Ready IRQ + IIO trigger | `/sys/bus/iio/devices/iio:deviceX/trigger/current_trigger` 与 `/proc/interrupts` 有证据 |
| 8 | IIO triggered buffer + timestamp | `iio_generic_buffer` 能采样 |
| 9 | suspend/resume | `echo mem > /sys/power/state` 后恢复 |

#### 数据路径（必须画对）

```
MPU6500 → I2C controller driver → inv_mpu6050 I2C client driver
                                  → IIO device + triggered buffer
                                  → Data Ready IRQ → iio_trigger
                                  → /dev/iio:deviceX (char device)
                                  → Qt 应用
```

#### 应用任务（引入 IMU 专用数据源接口）

```
IImuSampleSource（仅抽象 IMU 数据源）
├── Mpu6050UserI2cSource（旧实现迁移）
└── Mpu6050IioBufferSource（新增，连续样本读 /dev/iio:deviceX）
```

sysfs 只负责 name、scale、sampling_frequency、scan_elements、buffer/trigger 配置；连续样本必须从 `/dev/iio:deviceX` 读取，不能用逐轴 sysfs 轮询代替 triggered buffer。

代码位置：
- `device/iimusamplesource.h`（新增接口）
- `device/mpu6050device.cpp` 重构为 `Mpu6050UserI2cSource`
- 新增 `device/mpu6050_iiobuffersource.cpp`

#### A/B 对比验收

| 指标 | UserI2cBackend | IioBackend | 备注 |
|------|---------------|-----------|------|
| 平均采样率 | ___ Hz | ___ Hz | 目标 50Hz |
| 最大采样间隔 | ___ ms | ___ ms | |
| P99 抖动 | ___ ms | ___ ms | |
| 丢样率（30分钟） | ___% | ___% | |
| CPU 占用 | ___% | ___% | `top -p PID` |
| 中断次数/秒 | N/A | ___ | `/proc/interrupts` |

#### 风险止损

如果第5天 IIO trigger/buffer 仍未通：
- **降级为 sysfs 轮询**（`cat /sys/.../in_accel_x_raw`，放弃 trigger）
- 仍能展示"IIO 通道注册 + 标准 ABI"
- trigger/buffer 部分推迟到第12周

#### 本周文档输出

- `docs/DEVICE_TREE_DESIGN.md`：I2C3 + MPU6500 子节点
- `docs/MPU6500_IIO_PORTING.md`：适配全过程 + A/B 数据

---

### 第5-6周：自研 AP3216C IIO 驱动（2周）

**项目最重要的自研驱动成果。依据 datasheet 独立实现，允许参考同类 IIO 框架，但必须记录来源和许可证。**

#### 基础版（必须完成，第5周）

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | `drivers/iio/light/ap3216c.c` | 文件创建 |
| 2 | `i2c_driver` + `of_match_table` | 使用核实后的厂商前缀，如 `compatible="liteon,ap3216c"` |
| 3 | Device Tree binding 文档 | `Documentation/devicetree/bindings/iio/light/liteon,ap3216c.yaml` |
| 4 | regmap 初始化 | 寄存器读写正常 |
| 5 | 身份/总线验证 | 先核实 datasheet 是否存在 chip ID；若无，则用复位默认值或受控写入-读回-恢复验证，不能把 ACK 当身份 |
| 6 | 初始化和复位 | 驱动日志、regmap debugfs 或单元测试证明；驱动绑定时禁止并发 `i2cdump` |
| 7 | `devm_iio_device_alloc()` | 内存分配 |
| 8 | `devm_iio_device_register()` | IIO 注册成功 |
| 9 | ALS 通道 `in_illuminance_raw` | 读到光照值 |
| 10 | IR 通道 `in_intensity_ir_raw` | 读到红外值 |
| 11 | PS 通道 `in_proximity_raw` | 读到距离值 |
| 12 | `read_raw()` + scale | RAW/SCALE/PROCESSED 语义依据 datasheet 和 IIO ABI 决定，不预设 PS 必须用 PROCESSED |
| 13 | Kconfig + Makefile | `make M=drivers/iio/light` 能编 |

#### 增强版（选 2 项，第6周前3天）

按 ROI 排序，**只选前 2 项**：
1. **Runtime PM**（最强简历价值）
2. **GPIO 中断 + IIO Event**（阈值告警依赖）

其余（regcache、错误恢复、I²C 失败统计、integration time、sampling frequency）列入"已知 TODO"，文档中说明。

#### 应用任务（第6周后4天）

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | `Ap3216IioBackend` 实现 | 自动寻找 `name=ap3216c` 的 iio:deviceX |
| 2 | Qt 环境监测页面 | 光照/红外/接近实时显示 |
| 3 | 保留 UserI2cBackend | A/B 切换 |
| 4 | 基础告警（应用层轮询阈值） | PS > 阈值时 UI 提示 |

> **注意**：AP3216C 的阈值告警先在应用层做（轮询 sysfs），不依赖内核 IIO Event。如果第6周选了 IIO Event 增强项，则切换为事件驱动。

#### 第一个停止点（第6周末求职底线）

如果时间紧，做到这些就停止增强：
- ✅ probe 成功
- ✅ 三个 IIO 通道
- ✅ 真实数据
- ✅ 设备树 + Kconfig/Makefile
- ✅ Qt 读取
- ✅ 30 分钟稳定采集

**此时已可作为"独立开发 IIO Sensor Driver"写入简历。**

#### 本周文档输出

- `docs/AP3216C_DRIVER_DESIGN.md`：自研驱动架构 + 寄存器表 + 数据路径

---

### 第7周：Vehicle-IO Platform Driver + Input 子系统（1周）

**板级车辆 IO 驱动，展示 platform_driver + GPIO IRQ + Input 子系统能力。**

#### 内核任务

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | `drivers/input/misc/quarkpi-vehicle-io.c` | 文件创建 |
| 2 | DTS 节点 + `of_match_table` | `compatible="alientek,quarkpi-vehicle-io"`（厂商前缀以 SDK binding 约定为准） |
| 3 | `platform_driver` 注册 | `platform_driver_register` 成功 |
| 4 | `devm_gpiod_get()` 获取 GPIO | gpiod 描述符有效 |
| 5 | `gpiod_to_irq()` 转中断号 | IRQ 号有效 |
| 6 | `threaded_irq` 注册 | 中断触发 handler 调用 |
| 7 | `delayed_work` 软件消抖（20ms） | 抖动不重复触发 |
| 8 | Input subsystem 注册 `input_device` | `/dev/input/eventX` 出现 |
| 9 | 按信号语义上报 Input 事件 | 瞬时按键用 `EV_KEY`；持续状态仅在有合理标准 code 时使用 `EV_SW` |
| 10 | 错误路径 + 资源释放（devm） | rmmod 无报错 |

#### 信号定义

```dts
quarkpi_vehicle_io: vehicle-io {
    compatible = "alientek,quarkpi-vehicle-io";
    reverse-gpios = <&gpio3 RK_PA1 GPIO_ACTIVE_LOW>;
    acc-gpios = <&gpio3 RK_PA2 GPIO_ACTIVE_LOW>;
    button-gpios = <&gpio3 RK_PA3 GPIO_ACTIVE_LOW>;
    debounce-ms = <20>;
    wakeup-source;
};
```

#### 应用任务

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | Qt 监听 `/dev/input/eventX` | 根据驱动设计收到 EV_KEY/EV_SW，并处理初始状态同步 |
| 2 | 倒车模式切换 | REVERSE 信号 → Qt 切换后视画面（第10周接真实摄像头） |
| 3 | ACC 状态显示 | GUI 状态栏更新 |
| 4 | 告警状态 | 按键触发告警 UI |

#### 不做的项（明确）

- ❌ 简单按键不写自研驱动（用 `gpio-keys`）
- ❌ 蜂鸣器不写私有字符设备（用 libgpiod 或 PWM/LED 框架）

#### 增强项（可选，时间够再做）

- `device_init_wakeup(dev, true)` 与 `enable_irq_wake()` 的生命周期成对管理
- 唤醒 IRQ 在 suspend 路径不得被无条件禁用；按 GPIO 控制器和目标电源状态实测
- resume 后重新读取 GPIO 电平并同步 Input 状态

> 如果做，必须成对出现，文档中明确说明。

#### 本周文档输出

- `docs/VEHICLE_IO_PLATFORM_DRIVER.md`：DTS 节点 + 中断拓扑 + Input 事件流

---

### 第8-9周：第二种 Sensor V4L2 驱动移植（2周）

**这是 P2 重点驱动项目。第8周必须出图，否则风险止损。**

#### 第8周前2天：硬件确认

| 序号 | 检查项 | 命令 |
|------|--------|------|
| 1 | Sensor 选型（IMX219 优先，OV5647 次之） | datasheet + 上游驱动确认 |
| 2 | 板端接口针脚定义 | 原理图核对 |
| 3 | 1.8V I²C/控制信号兼容性 | 万用表测电压 |
| 4 | 电源轨（AVDD/DVDD/DOVDD） | 示波器看上电时序 |
| 5 | MCLK | 示波器看波形 |
| 6 | RESET/PWDN 波形 | 示波器看时序 |
| 7 | 2 Lane / 4 Lane 确认 | 模块规格书 |
| 8 | CSI lane mapping + polarity | 原理图 + datasheet |
| 9 | I²C 地址 | 优先依据 datasheet/原理图；仅在确认总线上无敏感活动设备且 Sensor 未绑定驱动时受控探测 |
| 10 | Rockchip ISP 支持对应 RAW 格式 | BSP 文档 |
| 11 | IQ 文件可用 | 厂家提供 |
| 12 | CSI2 CLK lane continuous/non-continuous mode | QuarkPi-CA2 常见坑 |

#### 第8周后3天：最小驱动

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | I²C 读取 chip ID | 依据 datasheet 用最小、只读的寄存器事务或驱动 probe 日志确认；禁止对活动总线做全寄存器 `i2cdump` |
| 2 | 驱动 `probe()` 完成 | `dmesg \| grep -i sensor` |
| 3 | Sensor subdev 与 bridge 完成异步绑定 | `media-ctl -p` 出现 entity/link；`/dev/v4l-subdevX` 仅在内核导出 subdev node 时要求 |

> **硬止损**：如果第5天仍不能稳定读取 chip ID，**立即停止**，检查硬件，不写 V4L2 代码。

#### 第9周：V4L2 Subdev + Media Graph + 出图

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | `v4l2_subdev` + media entity 注册 | `media-ctl -p` 看到节点 |
| 2 | source pad + async registration | link 建立 |
| 3 | format enumeration + frame size enum | `v4l2-ctl --list-formats-ext` |
| 4 | 一套 mode register list | sensor 配置生效 |
| 5 | `s_stream()` | 流启动 |
| 6 | Runtime PM | suspend 时关电源 |
| 7 | clock/regulator/GPIO 管理 | 资源正确获取 |
| 8 | DTS endpoint + link frequencies + data lanes | dts 编译通过 |
| 9 | DPHY/CSI2/CIF/ISP 连接 | 完整链路通 |
| 10 | 连续采集真实图像 | `v4l2-ctl --stream-mmap --stream-count=300` |

#### 完整链路

```
第二种Sensor → I2C Sensor Driver → V4L2 Subdev → MIPI DPHY → CSI2 Receiver → CIF/ISP → /dev/videoX
```

#### 摄像头增强项（选 1-3 项，不追求全部）

按 ROI 排序：
1. exposure + analogue gain（基础控制）
2. test pattern（调试利器）
3. 水平/垂直翻转（车载场景常见）

其余（VBLANK/HBLANK、pixel rate、link frequency、新增分辨率、stream on/off 循环）列入 TODO。

#### 复用现有 FrameBroker/MediaPipeline

当前仓库已经存在 `media/frame_broker.*`、`media/media_pipeline.*` 及对应测试，本周禁止重新创建同名抽象，任务改为板端验证和双源扩展：
```
IMX415 MediaPipeline ─┐
                     ├→ Qt/RKNN/RTSP 选择与调度
NewSensor Pipeline ──┘
```

#### 第二条停止线（第9周末）

如果第9周结束只完成单路新 Sensor 采集，**也已足够写简历**。双路 GUI、双路 RTSP、双路 RKNN 全部属于可选应用功能，不能阻塞驱动成果收口。

#### 本周文档输出

- `docs/CAMERA_SENSOR_PORTING.md`：选型 + 硬件确认 + 寄存器 + Media Graph 拓扑图

---

### 第10周：异构双摄 + 倒车影像集成（1周）

**软件集成重点周，把前几周驱动成果串成完整故事。**

#### 软件任务

| 序号 | 任务 | 验收 |
|------|------|------|
| 1 | 双路 FrameBroker 调度 | IMX415 + 新 Sensor 同时采集 |
| 2 | 倒车信号 Input Event → Qt 切换后视 | 第7周 vehicle-io + 第8-9周 Sensor 联动 |
| 3 | 单路 RTSP 推流（保留） | VLC 拉流正常 |
| 4 | 双路 RKNN（可选） | 两路检测框 |
| 5 | 双路 RTSP（可选） | 第二路推流 |

#### 技术叙事

```
platform_driver（vehicle-io）
   + Input 子系统
   + V4L2 Sensor Driver（新 Sensor）
   + 双路视频调度（FrameBroker）
   + Qt 应用
   = 完整"倒车影像"链路
```

#### 风险止损

- 双路 RKNN 不通 → 只做单路 RKNN（前视）
- 双路 RTSP 不通 → 只做单路 RTSP（前视）
- 倒车切换延迟 > 500ms → 接受，文档说明

#### 本周文档输出

- 更新 `docs/ARCHITECTURE.md`：双摄数据流图

---

### 第11周：稳定性、故障注入与性能（必做）

> AMP 不占用本周固定工期。只有第10周末所有不能删除项均已验收、文档草稿齐全且整体进度至少提前 5 个工作日时，才从机动时间执行后述 AMP 最小闭环。

#### 测试任务

| 类别 | 测试项 | 工具 | 通过标准 |
|------|--------|------|---------|
| 驱动 | insmod/rmmod 100 次 | shell 脚本 | 无 leak |
| 驱动 | bind/unbind | sysfs | 设备正确重建 |
| I2C | 错误恢复 | 可控断电/解绑、驱动错误注入、适配器 `recover_bus`（若支持） | 驱动返回明确错误且能够恢复；禁止在活动摄像头/PMIC总线上用 `i2cdetect -r` 冒充恢复 |
| Sensor | stream on/off 100 次 | v4l2-ctl | 无 leak |
| Sensor | 断电重连 | 物理 | 恢复采集 |
| 系统 | suspend/resume 50 次 | `rtcwake` | 后恢复 |
| 系统 | 中断风暴检查 | `/proc/interrupts` | 无异常增长 |
| 内存 | 新增 dmesg warning/oops | 与启动基线做差分 | 无本项目引入的新告警/oops |
| 内存 | kmemleak | debug 内核中先 clear 再 scan | 无可归因于本项目驱动的未引用对象；记录并排除基线误报 |
| 稳定性 | 30 分钟运行 | 应用 + 驱动全开 | 无崩溃 |
| 稳定性 | 2 小时运行 | 同上 | 无累积错误 |
| 稳定性 | 8 小时运行 | release 内核，全链路 | 第12周最终执行，无累积错误 |

#### 性能对比报告

核心表格：

| 指标 | 用户态 I2C | IIO 内核驱动 | 实测差异与原因 |
|------|-----------|-------------|---------------|
| MPU6500 采样率 | | | |
| MPU6500 CPU 占用 | | | |
| MPU6500 P99 抖动 | | | |
| AP3216C 响应延迟 | | | |

不预设 IIO 一定提高吞吐；报告同时评价标准 ABI、IRQ/Buffer、时间戳、PM、CPU 占用和抖动。

#### AMP RPMsg 机动项（仅进度提前时）

1. 修正已审计出的 SDK AMP 配置问题；
2. RT-Thread 固件编译并在指定小核启动；
3. 确认保留内存和资源无冲突；
4. Linux 与 RTOS 完成 RPMsg ping/pong；
5. 记录消息序号、超时、平均/P99 往返延迟；
6. 连续 10 万条消息无错序。

本轮不承诺 MPU6500 迁移或 Linux 虚拟 IIO；它们作为 12 周之后的扩展。若 AMP 未完成，简历和最终架构不得出现已完成表述。

### 第12周：8小时测试、文档、简历与面试材料

1. 使用 release 配置完成 8 小时联合运行并归档日志；
2. 复核第11周回归和性能数据；
3. 关闭或如实登记所有 blocker；
4. 整理对应周次已写的文档草稿；
5. 根据实际验收结果生成简历，不复制未完成模板；
6. 编写 `INTERVIEW_QA.md` 并准备故障案例、数据路径和权衡说明。

#### 文档整理清单

> **重要**：所有文档应在对应周次同步写草稿，本周只做整理 + INTERVIEW_QA。

```
docs/
├── BSP_BUILD_AND_BOOT.md          ← 第3周写
├── BOARD_BRINGUP.md               ← 第3周写
├── DEVICE_TREE_DESIGN.md          ← 第4周写
├── MPU6500_IIO_PORTING.md         ← 第4周写
├── AP3216C_DRIVER_DESIGN.md       ← 第5-6周写
├── VEHICLE_IO_PLATFORM_DRIVER.md  ← 第7周写
├── CAMERA_SENSOR_PORTING.md       ← 第8-9周写
├── AMP_RPMSG_DESIGN.md            ← 仅执行 AMP 机动项时创建
├── PERFORMANCE_REPORT.md          ← 第12周整理
├── STABILITY_REPORT.md            ← 第12周整理
└── INTERVIEW_QA.md                ← 第12周写
```

每个驱动文档必须包含：
- 架构图
- 数据路径
- 关键结构体
- probe 流程
- 设备树
- 板端日志
- 测试命令
- 故障现象 + 根因分析
- 修复前后对比
- 性能数据
- Git 提交 hash

---

## 五、内核子系统和设备树参与度

### 涉及的内核子系统

| 子系统 | 涉及阶段 | 具体驱动 | 自研/适配 |
|--------|---------|---------|----------|
| IIO | 第4周 | MPU6500（`inv_mpu6050` 主线） | 适配 + trigger/buffer |
| IIO | 第5-6周 | AP3216C（`ap3216c.c`） | **自研** |
| I2C | 第4-6周 | `i2c_driver` + `of_match_table` | 自研 + 适配 |
| Input | 第7周 | Vehicle-IO `input_device` | **自研** |
| Platform 驱动框架 | 第7周 | `platform_driver` + `platform_device` | **自研** |
| GPIO | 第7周 | `devm_gpiod_get` + `gpiod_to_irq` | 使用 |
| 中断子系统 | 第4/7周 | `threaded_irq` + `request_irq` | 使用 |
| V4L2 Subdev | 第8-9周 | 第二种 Sensor | **自研/移植** |
| Media Controller | 第8-9周 | media entity + link | 使用 |
| Regmap | 第5周 | AP3216C 寄存器访问 | 使用 |
| Runtime PM | 第5-9周 | `pm_runtime_*` | 使用 |
| RPMsg | 机动项 | RTOS 启动 + RPMsg ping/pong 测试程序 | 适配；虚拟 IIO 不在本轮承诺范围 |

### 设备树修改

板级 DTS 必须新增的子节点：
- MPU6500（`&i2c3` 下，compatible="invensense,mpu6500"）
- AP3216C（挂载总线由原理图/引脚占用审计决定，使用合法厂商前缀 compatible）
- Vehicle-IO（使用合法厂商前缀 compatible）
- 第二种 Sensor（I²C 总线和 DPHY endpoint 必须依据 QuarkPi-CA2 原理图确认，不预先写死 i2c4/dphy0）

必须自撰的设备树 binding 文档：
- `Documentation/devicetree/bindings/iio/light/liteon,ap3216c.yaml`（厂商前缀核实后定稿）
- `Documentation/devicetree/bindings/input/alientek,quarkpi-vehicle-io.yaml`

### 内核修改

| 修改类型 | 内容 |
|---------|------|
| 新增驱动文件 | `drivers/iio/light/ap3216c.c`、`drivers/input/misc/quarkpi-vehicle-io.c` |
| 新增/修改 Sensor 驱动 | `drivers/media/i2c/imx219.c`（或其他型号） |
| Kconfig 修改 | 新增 CONFIG_AP3216C、CONFIG_QUARKPI_VEHICLE_IO 等 |
| Makefile 修改 | 新增编译规则 |
| DTS 修改 | 板级 .dts + .dtsi |
| defconfig 修改 | 启用新 CONFIG |

### 内核剪裁

第2-3周建立 debug/release 两套 defconfig，启用经当前 SDK 核实的 IIO/I2C/Input/Media/V4L2/调试符号。只裁剪经依赖审计确认未使用的子系统；使用 Wi-Fi SSH 时必须保留 Wireless。

---

## 六、裁剪顺序（时间不足时按此删，越靠前越先删）

1. AMP 传感器迁移（MPU6500 → RTOS）
2. AMP 异常恢复
3. 双路 RKNN
4. 双路 RTSP（**保留单路 RTSP**）
5. 新 Sensor 第二种分辨率
6. AP3216C 高级 IIO Event
7. 语音自由对话
8. GUI 美化

### 不能删除项

- ✅ 软件基线（第0阶段）— 2026-07-21 封板
- ❌ 自编译 SDK 镜像
- ❌ MPU6500 IIO 闭环
- ❌ AP3216C 自研驱动
- ❌ platform/Input 驱动
- ❌ 第二种摄像头至少单路真实采集
- ❌ 稳定性测试与项目文档

---

## 七、其他原功能处理

| 功能 | 新计划 |
|------|--------|
| 现有 Qt GUI | 保留，作为驱动数据展示终端 |
| IMX415 | 保留，作为已知正常的摄像头基线 |
| 第二种 Sensor | P2 重点驱动项目 |
| 双路 RTSP | 可选，不影响主线 |
| 双路 RKNN | 删除必做属性 |
| FFmpeg 录像 | 已完成，停止扩展 |
| ASR/TTS | 保留现状，停止新增功能 |
| RKLLM | 不再为旧出厂 NPU 驱动耗时 |
| DHT11 | **已放弃**：单总线时序脆弱，用户态抖动大 |
| SHT30 | 标准的 I2C 接口 + 主线 IIO 驱动 sht31.c，精度 ±0.2℃ / ±2%RH，替换 DHT11 |
| 按键 | Input/gpio-keys 或 vehicle-io |
| 蜂鸣器 | libgpiod/PWM/LED 框架，不写私有字符设备 |
| PHY | 只有真实协商、时序或丢包故障才修改 |
| LVGL | 不做 |
| 完整重写 U-Boot | 不做 |
| GUI 美化 | 不做 |

---

## 八、最终项目技术结构

```
RK3588S自编译BSP
├── U-Boot / Kernel / DTB / Buildroot
├── MPU6500
│   └── I2C + 主线驱动适配 + IIO Trigger/Buffer
├── AP3216C
│   └── 自研 I2C + Regmap + IIO + IRQ/Event
├── Vehicle IO
│   └── Platform + GPIO IRQ + Input + PM/Wakeup
├── Camera
│   ├── IMX415基线
│   └── 第二种Sensor移植 + V4L2 Subdev + Media Graph
├── AMP（机动项）
│   └── RT-Thread + RPMsg ping/pong；虚拟IIO为后续扩展
└── Qt应用
    └── IIO/Input/V4L2/RTSP/RKNN统一展示
```

三个递进层次：
1. 能编译并启动自己的 BSP
2. 能开发和调试标准 Linux 内核驱动
3. 能处理摄像头等复杂系统问题；进度允许时再增加 AMP 异构通信

---

## 九、最终简历模板（12 周完成后）

> **基于 RK3588S 的 AI 车载多媒体座舱终端**（个人项目，2026.xx-2026.xx）
>
> **嵌入式 BSP 与内核驱动**
> - 自编译 U-Boot + Kernel + DTB + Buildroot rootfs，从 SD 卡启动，建立一键编译部署流程
> - MPU6500 IIO 驱动适配：主线 `inv_mpu6050` + IIO trigger/buffer + Data Ready IRQ + suspend/resume
> - **自研 AP3216C IIO 驱动**：`i2c_driver` + `of_match_table` + regmap + 三通道（ALS/IR/PS）+ Runtime PM
> - **Vehicle-IO platform 驱动**：`platform_driver` + `devm_gpiod_get` + `threaded_irq` + Input 子系统 + 软件消抖
> - **第二种摄像头 Sensor V4L2 Subdev 移植**：I²C 驱动 + Media Graph + DPHY/CSI2/ISP 完整链路
>
> **应用与内核联调**
> - 设计 `IImuSampleSource`，实现用户态 I2C / IIO Buffer 双数据源 A/B 对比，量化采样率、抖动、CPU 占用
> - 倒车影像端到端链路：vehicle-io Input 事件 → Qt 切换 → 第二路 V4L2 Sensor
> - 双路 FrameBroker 调度 + IMX415 基线对照
>
> **硬件加速全链路**：V4L2 → RGA im2d → MPP H.264/H.265 → RTSP/MP4
>
> **稳定性与性能**
> - 8 小时稳定性测试 + kmemleak 零泄漏 + suspend/resume 50 次
> - perf/ftrace/trace-cmd 性能分析，IIO vs 用户态 I2C 性能对比报告
>
> **异构（仅实际完成时保留）**：RK3588S AMP 框架 + RT-Thread + RPMsg 跨核通信，完成往返延迟和 10 万条消息稳定性验证

---

## 十、执行进度跟踪

> 每完成一项，把对应行的"状态"从 `☐ 未开始` 改为 `✅ 完成（日期）`，
> 并在 [MODULE_STATUS.md](./MODULE_STATUS.md) 写入详细验收报告。

| 阶段 | 周次 | 任务 | 状态 | 完成日期 | 验收报告 |
|------|------|------|------|---------|---------|
| P0前置 | 第1周 | 出厂镜像软件基线封板 | ✅ 完成 | 2026-07-21 | [scripts/baseline_verify.sh](../scripts/baseline_verify.sh) / [docs/baseline/features.md](./baseline/features.md) / [docs/baseline/devices.txt](./baseline/devices.txt) |
| P0 | 第2-3周 | 自编译 BSP（U-Boot/Kernel/DTB/RootFS） | ✅ 完成（最终重建复核） | 2026-08-08 | [BSP_BUILD_AND_BOOT.md](./BSP_BUILD_AND_BOOT.md) / [BOARD_BRINGUP.md](./BOARD_BRINGUP.md) |
| P1 | 第4周 | MPU6500 IIO 适配 + IImuSampleSource | ✅ 完成（system suspend/resume 按计划延后） | 2026-08-08 | [MPU6500_IIO_PORTING.md](./MPU6500_IIO_PORTING.md) / [DEVICE_TREE_DESIGN.md](./DEVICE_TREE_DESIGN.md) / [DRIVER_DEBUGGING_LOG.md](./DRIVER_DEBUGGING_LOG.md) |
| P1 | 第5-6周 | 自研 AP3216C IIO 驱动（基础版 + 全部增强项） | ✅ 完成（最终重建复核） | 2026-08-08 | [AP3216C_DEVICE_TREE_DESIGN.md](./AP3216C_DEVICE_TREE_DESIGN.md) / [AP3216C_IIO_PORTING.md](./AP3216C_IIO_PORTING.md) / [AP3216C_INTEGRATION_AND_VALIDATION.md](./AP3216C_INTEGRATION_AND_VALIDATION.md) |
| P1 | 第7周 | Vehicle-IO platform + Input 子系统驱动 | ✅ 完成（最终重建复核） | 2026-08-08 | [vehicle-io-driver/](./vehicle-io-driver/) |
| P2 | 第8-9周 | 第二种 Sensor V4L2 + Media Graph | ☐ 未开始 | - | - |
| P2 | 第10周 | 异构双摄 + 倒车影像集成 | ☐ 未开始 | - | - |
| P4 | 第11周 | 稳定性 + 故障注入 + 性能 | ☐ 未开始 | - | - |
| P4 | 第12周 | 8小时测试 + 文档 + 简历 | ☐ 未开始 | - | - |
| P3机动 | 仅提前≥5个工作日 | AMP RPMsg 最小闭环 | ☐ 条件未满足 | - | - |

**已完成的旧 Phase 1 项（作为驱动验收基线保留）：**

| 旧任务 | 状态 | 完成日期 | 验收报告 |
|--------|------|---------|---------|
| 应用层缓存 IDR | ✅ 完成 | 2026-07-20 | [MODULE_STATUS.md §1bis](./MODULE_STATUS.md) |
| RKNN 检测联动 | ✅ 完成 | 2026-07-20 | [MODULE_STATUS.md](./MODULE_STATUS.md) |
| MPU6500 用户态驱动（基线，待升级为 IIO） | ✅ 完成 | 2026-07-21 | [MODULE_STATUS.md](./MODULE_STATUS.md) |
| 视频录像 FFmpeg | ✅ 完成 | 2026-07-21 | [Mp4Recorder](../media/mp4_recorder.h) |
| AI 语音 KWS+语义 | ✅ 完成 | 2026-07-21 | [VoiceManager](../voice/VoiceManager.cpp) |

---

## 十一、执行原则

1. **严格按顺序**：P0前置 → P0 → P1 → P2 → P4；P3 AMP 仅使用提前量，不得插队
2. **每项板端验收**：不验收不算完成
3. **每项写入 MODULE_STATUS.md**：保留工程证据
4. **每项更新本文件第十节**：状态跟踪
5. **文档每周同步写草稿**：不堆积到第12周
6. **每个驱动阶段加风险止损条件**：超时立即降级或跳过
7. **软件先做基线，驱动应用交替集成**：避免"纯驱动两个月最后才发现应用挂了"
8. **不重复造轮子**：MPU6500 用主线驱动，不写 U-Boot 完整移植
9. **简历叙事一致**：按第九节模板组织简历内容

---

Last updated: 2026-07-21（第0阶段「出厂镜像软件基线封板」✅ 完成。baseline_verify.sh 11 项验证：5 PASS / 1 FAIL（GPS 室内无信号，环境问题）/ 5 MANUAL。核心成果：IMX415 采集 + RGA + RKNN + MPP + RTSP + 启动退出 3 次无崩溃全部通过。同日完成 MP4 录像链路打通 + 摄像头预览卡顿修复 + 退出崩溃修复。下一步：第2-3周 自编译 BSP。）

<!-- WEEK7_VEHICLE_IO_FINAL_FREEZE_V1_BEGIN -->
## 第7周最终冻结记录：Vehicle-IO驱动与应用集成

状态：**已完成并通过最终验收**

已完成内容：

- 自研`quarkpi-vehicle-io`内核input驱动；
- REVERSE、ACC和BUTTON三路低电平有效GPIO输入；
- DTS、binding、Kconfig、Makefile和defconfig集成；
- Qt应用通过设备名称自动扫描`/dev/input/eventX`；
- REVERSE有效时打开Monitor；
- REVERSE释放后不自动关闭Monitor或返回主页面；
- ACC状态变化日志；
- BUTTON触发约200 ms有源蜂鸣器脉冲；
- REVERSE、ACC和BUTTON IRQ增量均验证为2；
- 正式程序持久化至`/usr/bin/CarTerminal`；
- Buildroot软件包正式二进制和配置同步；
- 冷启动后CarTerminal自动运行并连接Vehicle-IO；
- GPIO4_A3静音状态为输出高电平；
- 蜂鸣器SIG增加10 kΩ至3.3 V上拉，消除上电误鸣；
- 冷启动后的BUTTON、ACC和REVERSE端到端回归通过；
- Vehicle-IO和蜂鸣器专项错误检查为空；
- 设计、接线、移植、验证记录及源码证据完成冻结。

最终CarTerminal二进制SHA256：

    e013f586f9e5a36fd18d1d5cb25d6003ba2231cead788ea47f030fcf17eb2539

归档位置：

    docs/vehicle-io-driver/

说明：本结论仅覆盖第7周Vehicle-IO范围，不替代其他模块的独立验收。
<!-- WEEK7_VEHICLE_IO_FINAL_FREEZE_V1_END -->

<!-- WEEK0_7_FINAL_DOCUMENTATION_CLOSURE_V2_BEGIN -->
## Week0–7 最终收口记录（2026-08-08）

本节是 Week0–7 当前最终状态，优先于正文中保留的历史中间进度描述。

| 周次 | 最终状态 | 说明 |
|---|---|---|
| 第1周 | ✅ PASS | 出厂软件基线封板完成 |
| 第2–3周 | ✅ PASS | 最终 BSP / Kernel / DTB / RootFS / CarTerminal 基线闭环 |
| 第4周 | ✅ PASS | MPU6500 正常 IIO/trigger/buffer/50Hz/Qt 完成；system suspend/resume 按原计划延后 |
| 第5–6周 | ✅ PASS | AP3216C 基础项及 Runtime PM、GPIO IRQ、IIO Event、Qt、稳定性闭环 |
| 第7周 | ✅ PASS | Vehicle-IO REVERSE/ACC/BUTTON 功能链闭环 |

SHT30 遗留项已经关闭：源码、Buildroot package、Buildroot target 和最终 rootfs 配置四层一致，配置 SHA256 为 `4d5f0a225df2818f3f78f04613d1150d27b072d00885488c09634ea7e31764a4`，最终 rootfs SHA256 为 `254acb1120be1741cab1a0b5d255480fc8916dc311e57d2457f7f18b85fe6c7d`。SHT30 backport 来源及 CRC8 修复已正式记录到 `docs/DRIVER_DEBUGGING_LOG.md`。

system suspend/resume 不作为 Week0–7 阻塞项。一次 deep suspend/resume 已暴露 MPU6500 resume IRQ/FIFO storm，因此当前状态为：

```text
MPU6500_NORMAL_RUNTIME=PASS
MPU6500_SYSTEM_SUSPEND_RESUME=FAIL_KNOWN
DEFER_TO_WEEK11_12=YES
```

第4–6周历史最终文档已按原 SHA256 恢复；Week7 文档包内部 SHA256 校验保持 PASS。旧 MPU6500 单独 patch-audit 中间目录不再作为最终 blocker；Week0–7 最终源码已经整体冻结。

```text
WEEK0_7_TECHNICAL_STOP_LINE=PASS
WEEK0_7_DOCUMENTATION_CLOSURE=PASS
WEEK0_7_FINAL_STATUS=PASS_WITH_DEFERRED_NONBLOCKING_ITEMS
```

<!-- WEEK0_7_FINAL_DOCUMENTATION_CLOSURE_V2_END -->

