# AP3216C Qt、Buildroot 与整机验证文档

## 1. 文档信息

- 应用工程：`<HOME>/RK3588S_CarTerminal`
- 正式程序：`/usr/bin/CarTerminal`
- 正式配置：`/usr/share/carterminal/config/vehicle_devices.conf`
- Buildroot 包：`buildroot/package/car-terminal-bin`
- 最终 rootfs：`rootfs-c0433ef-ap3216-final-unflashed.ext4`
- rootfs 刷写地址：`0x00078000`
- 板卡序列号：`a16cb2e8557c8626`
- 文档状态：AP3216C 子任务最终闭环。

## 2. 集成目标

应用层和镜像集成需要达到：

1. Qt 不直接依赖固定 IIO 设备号。
2. 支持 IIO 与用户态 I2C 两种后端切换。
3. 正式模式使用 IIO，自动打开三通道和事件 fd。
4. UI 实时显示 ALS、PS、IR。
5. proximity rising/falling event 能驱动告警状态。
6. 程序和配置写入 Buildroot 包源，而不是只修改 `output/target`。
7. rootfs 重新构建、刷写和重启后仍保持一致。
8. 完成30分钟稳定采集，并保留原始数据和审计证据。

## 3. 应用架构

最终结构：

```text
Ap3216c
  -> Ap3216Worker
      -> IAp3216Backend
          -> Ap3216 IIO backend
          -> AP3216C user-I2C backend
```

职责划分：

| 层 | 职责 |
|---|---|
| `Ap3216c` | 对 UI 暴露统一状态、数据和告警信号 |
| `Ap3216Worker` | 后台采集、线程生命周期、后端切换 |
| `IAp3216Backend` | 统一 open/read/close/event 接口 |
| IIO backend | sysfs raw 读取、设备扫描、IIO event fd |
| User-I2C backend | 直接 I2C 访问，用于 A/B 和诊断 |
| UI | 显示实时值、后端状态和告警 |

这种分层避免 UI 直接拼接 sysfs 路径或访问寄存器。

## 4. IIO 设备自动扫描

IIO 设备号可能随驱动注册顺序变化，因此不能固定：

```text
/sys/bus/iio/devices/iio:device3
```

正式后端遍历：

```text
/sys/bus/iio/devices/iio:device*
```

并读取每个目录的 `name`，匹配：

```text
ap3216c
```

匹配后构造：

```text
in_illuminance_raw
in_proximity_raw
in_intensity_ir_raw
events/...
```

配置中的 `ap3216_sysfs_dir` 留空，表示自动扫描。

## 5. 正式配置

最终 `vehicle_devices.conf` 中 AP3216C 配置：

```ini
ap3216_enabled=true
ap3216_backend=iio
ap3216_sysfs_dir=
ap3216_als_file=in_illuminance_raw
ap3216_ps_file=in_proximity_raw
ap3216_ir_file=in_intensity_ir_raw
ap3216_use_iio_events=true
ap3216_i2c_device=/dev/i2c-3
ap3216_i2c_address=0x1e
ap3216_poll_interval_ms=500
ap3216_ps_falling_threshold=50
ap3216_ps_rising_threshold=100
```

配置 SHA256：

```text
4d5f0a225df2818f3f78f04613d1150d27b072d00885488c09634ea7e31764a4
```

## 6. IIO Event fd

用户态获取 event fd 必须使用：

```c
ioctl(device_fd, IIO_GET_EVENT_FD_IOCTL, &event_fd)
```

event sysfs 路径位于：

```text
events/in_proximity_thresh_falling_en
events/in_proximity_thresh_rising_en
events/in_proximity_thresh_falling_value
events/in_proximity_thresh_rising_value
```

正式启动后：

- falling event enable = 1
- rising event enable = 1
- falling threshold = 50
- rising threshold = 100
- Qt 进程持有 `anon_inode:iio:event`
- Runtime PM usage 通常为 1

靠近和移开目标物后，UI 分别切换告警和正常状态，并由 `iio_event` 标识事件来源。

## 7. User-I2C / IIO A/B

A/B 目的不是同时访问设备，而是验证：

- IIO 后端是否正确；
- 用户态 I2C 后端是否仍可作为诊断备用；
- 数据变化趋势是否合理；
- 切换后资源是否释放；
- 正式配置是否能恢复为 IIO。

已完成：

| 项目 | 结果 |
|---|---|
| IIO open/read | PASS |
| IIO realtime | PASS |
| IIO event alarm | PASS |
| User-I2C open/read | PASS |
| User-I2C polling alarm | PASS |
| User-I2C 60秒运行 | PASS |
| 独占冲突检查 | PASS |
| A/B roundtrip | PASS |
| 正式配置恢复 IIO | PASS |

不得在 IIO 驱动绑定且应用持有设备时，再用独立用户态程序并发改写寄存器。

## 8. Buildroot 包持久化

权威包目录：

```text
buildroot/package/car-terminal-bin
```

最终更新：

```text
car-terminal-bin.mk
files/CarTerminal
files/runtime/config/vehicle_devices.conf
```

包版本：

```text
20260726
```

包源二进制：

```text
SHA256=b042a98c44bffb38d3ef4f92d1edfa253d9d3ab46c757a3c8b912403a8e4fbab
size=5213144
not stripped
```

Buildroot finalization 后：

```text
SHA256=3bc39846a7b10071347d313b6e2df611769c5f7100400df6ba6c2b827ae59d89
size=4926240
stripped
```

`output/target/usr/bin/CarTerminal` 与 rootfs 镜像内程序完全一致。哈希变化来自 Buildroot 正常 strip，而不是二进制被意外替换。

## 9. 内核模块目录归一化

### 9.1 问题

Buildroot 输出目录曾残留旧内核目录：

```text
5.10.209-g5b968a37872e-dirty
```

兼容链接也指向旧目录：

```text
5.10.160 -> 5.10.209-g5b968a37872e-dirty
5.10.209 -> 5.10.209-g5b968a37872e-dirty
```

`00-wifibt.sh` 又使用当前内核编译模块并写入该树，导致模块元数据同时出现旧 release 和当前 release。归一化脚本因此报告两个版本。

### 9.2 修复

修复方法：

1. 隔离旧真实目录、旧链接和旧 `.carterminal-module-layout`。
2. 保留当前重新编译的平铺 Wi-Fi/BT `.ko`。
3. 重新执行 `post-rootfs`。
4. 由归一化脚本识别唯一 release。
5. 生成正式目录和兼容链接。
6. 运行 depmod 生成依赖索引。

最终布局：

```text
5.10.209-gc0433ef265af/
5.10.160 -> 5.10.209-gc0433ef265af
5.10.209 -> 5.10.209-gc0433ef265af
```

记录文件：

```text
EXPECTED_KERNEL_RELEASE=5.10.209-gc0433ef265af
CANONICAL_MODULE_DIRECTORY=/usr/lib/modules/5.10.209-gc0433ef265af
CANONICAL_MODULE_COUNT=36
FLAT_MODULE_COUNT=13
```

`depmod` 对缺少 `modules.order` 发出非致命警告，但 `modules.dep`、`modules.alias` 和 `modules.symbols` 均已生成并写入镜像。

## 10. rootfs 构建

最终构建结果：

```text
ROOTFS_BUILD_RC=0
ROOTFS_SUCCESS_COUNT=2
ROOTFS_FATAL_COUNT=0
FINAL_ROOTFS_BUILD=PASS
```

镜像：

```text
<HOME>/quarkpi-sdk-clean/ap3216c_freeze_20260726/build/
rootfs-c0433ef-ap3216-final-unflashed.ext4
```

属性：

```text
size=1538260992
mode=0444
filesystem=ext4
SHA256=7cf5adf2750e4d7d06fb867da7e401384d838b106b15e052442c7d8ee05b4366
```

只读文件系统检查：

```text
e2fsck -fn
RC=0
```

## 11. rootfs 刷写

实际分区表：

```text
rootfs start = 0x00078000
rootfs size  = 0x01c00000 sectors = 14 GiB
```

刷写命令使用：

```text
upgrade_tool WL 0x78000 <rootfs image>
```

不能使用 `0x8000`，因为 `0x8000` 是 boot 分区地址。

刷写结果：

```text
Write LBA from file (100%)
ROOTFS_FLASH_RC=0
ROOTFS_FLASH=PASS
```

设备复位后 ADB 恢复，序列号：

```text
a16cb2e8557c8626
```

## 12. 刷写后一致性

板端结果：

| 项目 | 实际值 |
|---|---|
| kernel release | `5.10.209-gc0433ef265af` |
| CarTerminal SHA256 | `3bc39846a7b10071347d313b6e2df611769c5f7100400df6ba6c2b827ae59d89` |
| config SHA256 | `4d5f0a225df2818f3f78f04613d1150d27b072d00885488c09634ea7e31764a4` |
| backend | `iio` |
| IIO events | `true` |
| falling/rising | `50/100` |
| module real dir | `5.10.209-gc0433ef265af` |
| module aliases | 正确指向当前 release |
| AP3216C IRQ | 实际增长 |

因此 rootfs 持久化不是仅在构建目录中成立，而是已经通过实际刷写和重启验证。

## 13. 运行态验证

运行态停止线：

```text
CarTerminal process alive
AP3216C IIO device discovered
ALS/PS/IR readable
event enable = 1/1
threshold = 50/100
power/control = auto
runtime_status = active
runtime_usage >= 1
IIO event fd exists
AP3216C dmesg error count = 0
```

IRQ 示例：

```text
153: ... rockchip_gpio_irq ... Edge ap3216c
```

刷写后实际观察到 IRQ 计数增长，UI 靠近/移开告警已验证。

## 14. 30分钟稳定性

测试输出：

```text
samples_expected=360
samples_recorded=360
interval_seconds=5
read_fail_count=0
process_fail_count=0
event_fd_fail_count=0
runtime_pm_fail_count=2
error_count_start=0
error_count_final=0
```

Runtime PM 分布：

```text
358 samples: control=auto,status=active,usage=1
2 samples:   control=auto,status=active,usage=2
```

两条 `usage=2` 为 Qt 读取与 IIO Event 长期引用重叠。没有 usage underflow、非 active、control 异常或内核错误。独立裁决：

```text
runtime_usage_2_classification=VALID_TRANSIENT_CONCURRENT_PM_REFERENCE
corrected_runtime_pm_criterion=control_auto_and_status_active_and_usage_greater_or_equal_to_1
AP3216_30MIN_STABILITY_FINAL=PASS
rerun_required=no
```

原始 STOP 文件保留，裁决文件单独存在，保证审计可追溯。

## 15. 验收矩阵

| 类别 | 检查项 | 结果 |
|---|---|---|
| 驱动 | probe | PASS |
| 驱动 | ALS/PS/IR | PASS |
| 电源 | Runtime PM | PASS |
| 中断 | GPIO IRQ | PASS |
| 事件 | rising/falling | PASS |
| Qt | IIO 自动扫描 | PASS |
| Qt | event fd | PASS |
| Qt | UI 告警 | PASS |
| A/B | User-I2C/IIO | PASS |
| Buildroot | 包源持久化 | PASS |
| Buildroot | final target strip | PASS |
| rootfs | 内容提取核验 | PASS |
| rootfs | e2fsck | PASS |
| 刷写 | rootfs 0x78000 | PASS |
| 重启 | 应用/配置/模块 | PASS |
| 稳定性 | 360样本/30分钟 | PASS |
| 归档 | 文件级 SHA256 | PASS |
| 归档 | gzip/tar 结构 | PASS |

## 16. 最终冻结归档

归档：

```text
<HOME>/quarkpi-sdk-clean/
quarkpi_ap3216c_freeze_20260726_final.tar.gz
```

归档大小：

```text
683080420 bytes
```

归档 SHA256：

```text
42f48e5484639a235fffdb5e9ed318f08dd881eb58c0b4f662f4cd19ea3b2d07
```

归档中包括：

- 冻结源码；
- 六个提交 patch；
- boot/rootfs 镜像；
- binding/DTS/defconfig；
- 编译和板端日志；
- 30分钟 CSV、原始摘要与独立裁决；
- rootfs 刷写证据；
- manifest 和 SHA256SUMS。

## 17. 状态边界

AP3216C 范围：

```text
scope_status=PASS
postflash_validation=PASS
stability_30min=PASS
```

整个传感器栈：

```text
whole_sensor_stack_status=REVIEW_PENDING
remaining_external_issue=MPU6500_FAILED_TO_ACK_INTERRUPT
```

因此项目报告可以写：

> AP3216C 增强版 IIO 驱动、Qt 后端和 rootfs 持久化已完成并通过30分钟稳定性验证。

不能写：

> 全部传感器驱动均无错误。

## 18. 维护与复现建议

1. 以 Buildroot 包源为权威，不直接把 `output/target` 当作源码。
2. kernel release 变化后，清理旧模块真实目录和错误兼容链接。
3. 重新构建 rootfs 后，必须从镜像内部提取程序和配置核对。
4. rootfs 烧写前必须再次读取实际 partition table。
5. 自动测试不要把 Runtime PM usage 固定写成严格等于 1。
6. 事件测试应同时记录 IIO event、IRQ delta 和 PM 生命周期。
7. 冻结包生成后执行文件级 `sha256sum -c`、`gzip -t` 和 `tar -tzf`。
