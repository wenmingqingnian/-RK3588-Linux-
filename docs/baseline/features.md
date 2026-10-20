# 出厂镜像软件基线功能启用状态表

> 第0阶段封板基线快照，对应 PROJECT_ROADMAP.md §四 第0阶段"产物"清单。
>
> 创建：2026-07-21
> 生成方式：从 `config/*.conf` 当前默认值预填，板端验收时核对一致后归档。

---

## 一、模块启用状态总览

| 模块 | 配置文件 | 配置项 | 默认值 | 验收时是否启用 | 备注 |
|---|---|---|---|---|---|
| 摄像头 IMX415 | `config/camera_settings.conf` | `camera_enabled` | `true` | 启用 | /dev/video33 NV12 800x600 |
| RKNN 检测 | `config/rknn_settings.conf` | `rknn_enabled` | `true` | 启用 | conf=0.25 nms=0.45 6Hz |
| RTSP 推流 | `config/rtsp_settings.conf` | `rtsp_enabled` | `true` | 启用 | rtsp://board-ip:8554/live |
| MP4 录像 | `config/recording_settings.conf` | `recording_enabled` | `true` | 启用 | 复用 RTSP 编码器输出，零额外开销；60s 分片 FIFO 清理 |
| GPS 串口 | `config/vehicle_devices.conf` | `gps_enabled` | `true` | 启用 | /dev/ttyS5 9600 8N1 |
| GPS 模拟 | `config/vehicle_devices.conf` | `gps_simulation_enabled` | `false` | 关闭 | 与 gps_enabled 互斥 |
| 蜂鸣器 | `config/vehicle_devices.conf` | `buzzer_enabled` | `true` | 启用 | GPIO4_A3 active-low |
| MPU6050 IMU | `config/vehicle_devices.conf` | `mpu6050_enabled` | `false` | 验收 MPU6050 时临时开启 | I2C3 @ 0x68 |
| DHT11 温湿度 | `config/vehicle_devices.conf` | `dht11_enabled` | `false` | 关闭（IIO 节点未确认） | 后续驱动升级时启用 |
| AP3216C 光感 | `config/vehicle_devices.conf` | `ap3216_enabled` | `false` | 关闭（驱动未验收） | 后续自研 IIO 驱动时启用 |
| 按键输入 | `config/vehicle_devices.conf` | `key_input_enabled` | `false` | 关闭 | 后续 vehicle-io platform 驱动时启用 |
| 百度地图 | `config/map_settings.conf` | `map_enabled` | `true` | 启用 | WGS84 坐标 |
| 地图标记 | `config/map_settings.conf` | `map_markers_enabled` | `true` | 启用 | |
| 天气 | `config/weather_settings.conf` | `weather_enabled` | `false` | 关闭 | 非主链路 |
| 语音总开关 | `config/voice_settings.conf` | `voice_enabled` | `true` | 启用 | |
| 固定指令直连 | `config/voice_settings.conf` | `direct_command_enabled` | `true` | 启用 | 双路径之一 |
| ASR（离线） | `config/asr_settings_offline.conf` | `asr_enabled` | `true` | 启用 | sherpa-onnx zipformer |
| TTS | `config/voice_settings.conf` | `tts_enabled` | `true` | 启用 | SummerTTS |
| TTS 反馈 | `config/voice_settings.conf` | `tts_feedback_for_command` | `true` | 启用 | 命中后播报"已为您打开XX" |
| LLM | `config/voice_settings.conf` | `llm_enabled` | `false` | **禁止启用** | NPU 驱动 v0.9.6 SIGBUS，待 v0.9.7+ |
| KWS 唤醒词 | `config/kws_settings.conf` | `kws_enabled` | `false` | 关闭（KWS 模型未部署） | 部署模型后改为 true |

---

## 二、板端验收时配置调整清单

第0阶段验收 11 项主链路时，需对以下配置项临时调整（验收完成后恢复或保留）：

### 2.1 验收 MPU6050（第 9 项）

```diff
- mpu6050_enabled=false
+ mpu6050_enabled=true
```

前置条件：
- I2C3 总线已通过 dtbo overlay 启用（`rk3588s-i2c3-m1.dtbo`）
- `i2cdetect -y 3` 能看到 0x68
- 用户已加入 `i2c` 组

### 2.2 验收 MP4 录像（第 7 项）

`recording_enabled` 已默认为 `true`，无需临时调整。录像随 RTSP 启动自动开始，
60s 分片 + max_segments=10 FIFO 清理，无需手动管理磁盘占用。

### 2.3 验收 KWS（可选，非第0阶段必做项）

KWS 模型未部署前 `kws_enabled=false`，VoiceManager 自动回退到 legacy 连续 ASR 流程。
如需在第0阶段提前验收 KWS，需：
1. 下载 `sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01` 模型
2. 修改 `config/kws_settings.conf` 路径指向模型目录
3. 设 `kws_enabled=true`

---

## 三、不在第0阶段验收范围

以下项不参与第0阶段封板（按 PROJECT_ROADMAP.md §四 第0阶段"本周不做项"）：

- ❌ 自由对话 LLM（`llm_enabled` 必须保持 `false`）
- ❌ 扩展语音语义（KWS 部署后可做但不阻塞第0阶段）
- ❌ 双路 RTSP / RKNN
- ❌ GUI 美化
- ❌ Monitor 上帝对象重构

---

## 四、验收完成后的归档动作

1. 30 分钟稳定性测试日志（`logs/soak_C_30min_*.log` + CSV）
2. 启动/退出 3 次日志（`logs/baseline_verify_*.log`）
3. 设备节点清单（`docs/baseline/devices.txt`，由 `baseline_verify.sh` 自动生成）
4. 本表（`docs/baseline/features.md`）
5. git tag：
   ```bash
   git tag factory-image-software-baseline
   git push origin factory-image-software-baseline
   ```

---

Last updated: 2026-07-21（MP4 录像 recording_enabled 默认值从 false 改为 true；
退出崩溃修复后启动/退出 3 次验证通过；板端验收完成后按本表核对一致并打 git tag）
