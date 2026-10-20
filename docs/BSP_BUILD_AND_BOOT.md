# BSP Build and Boot

## 1. 文档目的

本文是 RK3588S CarTerminal 项目 Week2–3 BSP 阶段的最终收口记录。本文件不重新执行构建或刷写，而是记录已经完成并验收的 BSP 构建、最终构件和当前重建基线。

## 2. 当前权威构建树

- SDK：`<HOME>/quarkpi-sdk-vendor-rebuild`
- Board defconfig：`alientek_quarkpi_ca2_defconfig`
- Kernel tree：`<HOME>/quarkpi-sdk-vendor-rebuild/kernel`
- Kernel HEAD：`1165a0250ba01ca70b774385df325b8ba2d9f8cd`
- U-Boot tree：`<HOME>/quarkpi-sdk-vendor-rebuild/u-boot`
- U-Boot HEAD：`15be675219c9efe8c6cd8c7b47b1d1c77710ff2c`

当前基线采用纯厂商 BSP 重新建立，再迁移 Week0–7 最终已经验证的 Linux 侧功能；不以历史中间开发目录作为最终交付源。

## 3. Week2–3 BSP 目标闭环

已经完成并形成后续 Week4–7 基线的核心链路包括：U-Boot、Kernel、DTB、Buildroot rootfs、基础 I2C/UART/GPIO/Video、MPU6500/SHT30 IIO 以及 CarTerminal 自编译系统运行。

最终判定：

```text
WEEK2_3_BSP_BUILD=PASS
WEEK2_3_KERNEL=PASS
WEEK2_3_DTB=PASS
WEEK2_3_ROOTFS=PASS
WEEK2_3_APPLICATION_BASELINE=PASS
```

## 4. 当前最终 RootFS

```text
PATH=<HOME>/quarkpi-sdk-vendor-rebuild/buildroot/output/rockchip_atk_quarkpi_ca2/images/rootfs.ext2
SIZE=1538260992
SHA256=254acb1120be1741cab1a0b5d255480fc8916dc311e57d2457f7f18b85fe6c7d
```

此前已完成只读镜像检查，`output/firmware/rootfs.img`、Buildroot rootfs 构件与 `output/update/Image/rootfs.img` 对应最终 rootfs 身份一致。

## 5. 当前 update.img

```text
PATH=<HOME>/quarkpi-sdk-vendor-rebuild/output/update/Image/update.img
SIZE=1647876682
SHA256=ef83a1e7a52b78419da89555d48d778cf561a3c91202b6c5c2ed0e213d286c55
```

该身份由当前最终重建树直接计算。本次文档收口不重新运行打包，也不重新刷写开发板。

## 6. SHT30 配置持久化

以下四层已经闭环：项目源码配置、Buildroot package、Buildroot target、最终 rootfs 内 `/usr/share/carterminal/config/vehicle_devices.conf`。

```ini
dht11_enabled=true
dht11_sysfs_dir=
dht11_poll_interval_ms=2000
```

```text
CONFIG_SHA256=4d5f0a225df2818f3f78f04613d1150d27b072d00885488c09634ea7e31764a4
```

这里保留 `dht11_*` 历史配置键名称，但实际设备为 SHT30，内核使用 SHT3x/SHT31 IIO 驱动。

## 7. 构建和部署边界

- 使用已验证构件，不把旧 stale update.img 当作最终镜像；
- 最终镜像用 SHA256 追踪；
- 当前 Week0–7 文档收口不执行 build、clean、flash、reset、reboot 或 suspend。

## 8. 结论

```text
WEEK2_3_FINAL_ARTIFACT_IDENTITY=PASS
WEEK2_3_DOCUMENTATION=PASS
```

文档最终收口日期：2026-08-08
