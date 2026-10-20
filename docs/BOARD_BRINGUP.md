# Board Bring-up and Week0–7 Runtime Acceptance

## 1. 文档边界

本文汇总已经完成的板端 bring-up 与 Week0–7 最终运行验收。本文件是证据收口，不代表本次文档阶段重新访问或重新测试开发板。

```text
FINAL_KERNEL=Linux 5.10.209-g1165a0250ba0
KERNEL_HEAD=1165a0250ba01ca70b774385df325b8ba2d9f8cd
```

## 2. BSP 与基础启动

最终重建系统已完成 Linux 启动、Buildroot rootfs 挂载、CarTerminal 持久化，以及 Week0–7 Linux 侧最终功能迁移。

```text
WEEK2_3_BOARD_BRINGUP=PASS
```

## 3. SHT30

SHT30 I2C3 client、SHT3x IIO 绑定、温湿度读取、应用配置固化和三传感器联合运行均已通过。

```text
SHT30_RUNTIME=PASS
SHT30_CONFIG_FOUR_LAYER_CONSISTENCY=PASS
```

## 4. MPU6500

正常运行路径已经完成 IIO channels、Data Ready IRQ、trigger、buffer、50 Hz 采样和 CarTerminal IIO buffer 后端。

```text
MPU6500_NORMAL_RUNTIME=PASS
MPU6500_IIO_BUFFER=PASS
MPU6500_50HZ_IRQ=PASS
MPU6500_QT_IIO_BACKEND=PASS
```

system suspend/resume 不属于 Week0–7 阻塞验收。已经执行过一次 deep suspend/resume，系统能通过 RK805 power key 唤醒，但恢复后 MPU6500 出现高频 IRQ / FIFO overflow / interrupt acknowledge failure。

```text
MPU6500_SYSTEM_SUSPEND_RESUME=FAIL_KNOWN
WEEK0_7_BLOCKER=NO
RETEST_NOW=NO
DEFER_TO_WEEK11_12=YES
```

该失败不得写成 PASS，也不在 Week0–7 阶段继续重复 suspend。

## 5. AP3216C

```text
AP3216C_RUNTIME=PASS
AP3216C_RUNTIME_PM=PASS
AP3216C_GPIO_IRQ=PASS
AP3216C_IIO_EVENT=PASS
AP3216C_QT_EVENT_CHAIN=PASS
```

已完成 ALS/PS/IR、Runtime PM、GPIO IRQ、IIO Event、Qt IIO backend、Buildroot 持久化和稳定性验证。

## 6. I2C3 三传感器联合运行

```text
SHT30_JOINT_RUNTIME=PASS
MPU6500_JOINT_RUNTIME=PASS
AP3216C_JOINT_RUNTIME=PASS
I2C3_SHARED_BUS_STABILITY=PASS
CARTERMINAL_SENSOR_OWNERSHIP=PASS
```

历史上出现过 I2C timeout / `SCL hold by slave`，但最终联合 Gate 未再出现该错误，因此历史故障保留为调试记录，不覆盖最终 PASS。

## 7. Vehicle-IO

```text
VEHICLE_IO_THREE_BUTTON_FUNCTIONAL_ACCEPTANCE=PASS
REVERSE_FUNCTION=PASS
ACC_FUNCTION=PASS
BUTTON_BUZZER_FUNCTION=PASS
EVDEV=PASS
QT_BUSINESS_CHAIN=PASS
```

当前一次隔离测试中 BUTTON raw IRQ 增量为 3，而历史冻结值为 2；evdev 和 Qt 业务层没有重复动作，因此保留：

```text
BUTTON_RAW_IRQ_EXACTNESS=HOLD_RAW_IRQ_3_VS_HISTORICAL_2
VEHICLE_IO_FUNCTIONAL_BLOCKER=NO
```

## 8. Camera 边界

Week2–3/历史最终摄像头链已经通过。后续一次 REVERSE 业务测试记录过 `camera start failed` 警告；本次文档阶段不重新测试 Camera，也不把第10周真实倒车摄像头集成提前要求到 Week7。

```text
CAMERA_HISTORICAL_FINAL_RUNTIME=PASS
CAMERA_CURRENT_SESSION_REVERSE_WARNING=RECORDED
NEW_CAMERA_REVALIDATION_IN_THIS_CLOSURE=NO
```

## 9. Week0–7 最终技术判定

```text
WEEK1_CORE=PASS
WEEK2_3_CORE=PASS
WEEK4_CORE=PASS
WEEK5_6_CORE=PASS
WEEK7_CORE=PASS
WEEK0_7_TECHNICAL_STOP_LINE=PASS
```

已知但不阻塞 Week0–7：MPU6500 system suspend/resume 已知失败并延后、BUTTON raw IRQ exactness 非阻塞 HOLD、Camera 当前会话 reverse warning 仅记录。

文档最终收口日期：2026-08-08
