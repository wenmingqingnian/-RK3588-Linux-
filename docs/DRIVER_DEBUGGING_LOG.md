# Driver Debugging Log

## 1. SHT30 / sht31.c backport 与 CRC8 修复

目标系统为 Rockchip vendor Linux 5.10.209。项目使用 SHT30，并通过标准 Linux IIO 路径接入。历史实现以主线 5.15+ `drivers/iio/humidity/sht31.c` 为 backport 基础，对 vendor 5.10 API 和构建环境做最小适配。

已确认的关键改动：

1. 引入 `sht31.c` 并接入 Kconfig / Makefile；
2. Kconfig 添加 `select CRC8`；
3. 16-bit SHT3x 命令发送路径由 `i2c_smbus_write_word_data()` 调整为 `i2c_master_send()`；
4. probe 初始化阶段增加 `crc8_populate_msb(sht31_crc8_table, SHT31_CRC8_POLYNOMIAL)`。

修复前读取曾出现：

```text
Input/output error
```

根因是 CRC8 lookup table 未按 SHT31 多项式完成初始化，导致有效传感器返回值被错误判为 CRC 校验失败。补充 `crc8_populate_msb()` 后温湿度 IIO 读取恢复。

```text
SHT30_BACKPORT=PASS
SHT30_CRC8_FIX=PASS
SHT30_RUNTIME=PASS
```

## 2. SHT30 配置持久化闭环

```ini
dht11_enabled=true
dht11_sysfs_dir=
dht11_poll_interval_ms=2000
```

```text
CONFIG_SHA256=4d5f0a225df2818f3f78f04613d1150d27b072d00885488c09634ea7e31764a4
SOURCE_CONFIG=PASS
BUILDROOT_PACKAGE_CONFIG=PASS
BUILDROOT_TARGET_CONFIG=PASS
FINAL_ROOTFS_CONFIG=PASS
ROOTFS_SHA256=254acb1120be1741cab1a0b5d255480fc8916dc311e57d2457f7f18b85fe6c7d
ROOTFS_SIZE=1538260992
```

因此 Roadmap 中“SHT30 配置只是板端临时修复”的历史状态已经失效。

## 3. MPU6500 system suspend/resume 已知问题

正常 50 Hz IIO trigger/buffer 与 CarTerminal 运行路径已经通过。一次 deep suspend/resume 后出现 `fifo overflow reset` 与 `failed to ack interrupt`，同时观察到远高于正常 50 Hz 的 IRQ 增长。

当前主要假设是 deep suspend 后 MPU6500 硬件配置可能丢失，而 resume 路径没有在 FIFO/Data Ready IRQ 恢复前完整重建采样配置；但电源保持关系尚未完全证明，因此该假设不是已闭合根因。

```text
MPU6500_NORMAL_RUNTIME=PASS
MPU6500_SYSTEM_SUSPEND_RESUME=FAIL_KNOWN
ROOT_CAUSE_FULLY_CLOSED=NO
WEEK0_7_BLOCKER=NO
DEFER_TO_WEEK11_12=YES
```

不在 Week0–7 阶段继续重复 suspend。

## 4. 文档边界

本日志只记录已有证据支持的事实，不把推测写成已证明根因，不把历史中间状态写成当前最终状态，也不把本次未重新执行的测试写成“本次已验证”。

文档最终收口日期：2026-08-08
