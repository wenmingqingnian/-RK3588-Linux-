# 第7周 Vehicle-IO冻结归档

## 最终状态

| 验收项 | 状态 |
|---|---|
| Vehicle-IO内核驱动 | PASS |
| REVERSE输入 | PASS |
| ACC输入 | PASS |
| BUTTON输入 | PASS |
| BUTTON触发200 ms蜂鸣 | PASS |
| REVERSE释放后不自动返回 | PASS |
| 三路IRQ计数验证 | PASS |
| Qt应用自动扫描input设备 | PASS |
| 正式程序持久化 | PASS |
| Buildroot软件包同步 | PASS |
| 冷启动自动恢复 | PASS |
| 上电阶段蜂鸣器静音 | PASS |
| Vehicle-IO专项错误检查 | PASS |

## 归档结构

- `VEHICLE_IO_DRIVER_DESIGN.md`：内核驱动与设备树设计。
- `VEHICLE_IO_QT_INTEGRATION.md`：Qt读取与业务层适配。
- `WIRING_AND_BUZZER_PULLUP.md`：硬件接线和上电误鸣修复。
- `VEHICLE_IO_VERIFICATION.md`：编译、IRQ、运行及冷启动验收。
- `source-snapshot/`：应用、内核和Buildroot相关源码冻结副本。
- `evidence/`：构建、日志、IRQ、冷启动、SHA256和人工观察证据。

## 最终二进制

最终AArch64程序SHA256：

    e013f586f9e5a36fd18d1d5cb25d6003ba2231cead788ea47f030fcf17eb2539

该值已在以下三个位置核对一致：

1. 应用交叉编译产物；
2. Buildroot软件包中的CarTerminal；
3. 板端`/usr/bin/CarTerminal`。

## 边界说明

本归档仅冻结第7周Vehicle-IO相关交付。工作区内其他模块的未提交修改，不因本归档而被认定为完成或通过验收。
