# RK3588S 智能车载终端

基于 RK3588S 的嵌入式 Linux 智能车载终端的**应用层**代码（Qt/C++17）。负责图像采集与编码（MIPI CSI → MPP/RGA → RTSP / H.264 / H.265）、目标检测（RKNN / YOLOv5）、GUI（Qt Widgets）、传感器接入（DHT11 / AP3216C / MPU6050 / GPS / 按键 / 蜂鸣器）、车辆信号输入、语音链路（ASR / KWS / TTS 编排，基于 sherpa-onnx）以及本地存储与网络。

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `main.cpp` / `CarTerminal.pro` | 应用入口与 Qt 工程 |
| `media/` | 相机采集、MPP 编解码、RGA 色彩空间转换、RKNN 推理、RTSP 服务、MP4 录制 |
| `gui/` | Qt 界面（Monitor / Music / Weather / Map / Setting） |
| `device/` | 传感器与车辆外设的设备抽象与 Worker 线程 |
| `voice/` | 语音子系统编排（ASR / KWS / TTS / LLM / ZMQ）；**`voice/tts/` 为第三方 TTS 引擎，未纳入本仓库** |
| `network/` | 地图 / 天气等网络请求 |
| `common/` | 线程池、日志、告警、NMEA 解析等公用组件 |
| `config/` | 运行配置（`.conf`，密钥均为占位符 `YOUR_*`） |
| `resources/` `scripts/` `tests/` `docs/` | 资源、脚本、测试、设计文档 |

## 构建

- 目标平台：Linux aarch64（RK3588S），Qt 5.12+，C++17。
- 交叉编译后将产物部署到板端运行。
- `voice/tts/` 依赖的第三方 TTS 引擎为外部依赖，需另行获取并在板端部署其运行时；本仓库仅包含对其的 ABI / 接口集成代码。

## 第三方与许可

- 自有代码以 MIT 许可发布（见 `LICENSE`）。
- 第三方组件各自保留其许可：sherpa-onnx（ASR / TTS 运行时）、Rockchip MPP / RGA / RKNN（厂商 SDK）、Qt（GPL / LGPL）、Baidu 地图 / 语音 API（需自备密钥）。
- `voice/tts/` 内含的 Eigen、gflags 等第三方库未随本仓库分发，请遵循其各自许可。

## 说明

- 所有 API 密钥（地图、语音等）在提交配置中均为占位符（`YOUR_*`），运行前请替换为自有密钥，**切勿提交真实密钥**。
- 内核、U-Boot、Buildroot 与设备树不属于本应用层仓库。
