# 迁移蓝图：从 5 个项目到统一 RK3588S 项目

## 一、核心问题

当前做法是直接在 IMX6ULL VehicleTerminal 上修改，导致：
- 路径混乱（rk3588s_migration/ 和 VehicleTerminal/ 分离）
- 文件上传困难（中文路径、路径不一致）
- 新旧代码混杂，无法统一风格

正确做法：**创建全新项目结构，从 5 个项目中提取代码重组**。

## 二、新项目结构

```
RK3588S_CarTerminal/
├── common/                    # 公共工具层（从 rk3588s_migration/common/ 迁移）
│   ├── logmanager.h/cpp       # 统一日志（新写）
│   ├── vehiclecommand.h/cpp   # 命令解析（从 IMX6ULL 迁移）
│   └── config_utils.h/cpp     # 通用配置解析工具（从 weatherconfig 提取）
│
├── device/                    # 设备抽象层（从 rk3588s_migration/device/ 迁移）
│   ├── vehicledeviceconfig.h/cpp  # 设备配置（从 IMX6ULL 迁移）
│   ├── keyinputdevice.h/cpp       # 按键输入（从 IMX6ULL 迁移）
│   ├── dht11device.h/cpp          # DHT11（从 IMX6ULL 迁移）
│   ├── dht11worker.h/cpp          # DHT11 Worker（从 IMX6ULL 迁移）
│   ├── ap3216device.h/cpp         # AP3216C（从 IMX6ULL 迁移）
│   └── ap3216worker.h/cpp         # AP3216C Worker（从 IMX6ULL 迁移）
│
├── media/                     # 媒体采集层（新写 + 从 4.17 项目迁移）
│   ├── cameraconfig.h/cpp         # 摄像头配置（新写）
│   ├── capture_thread.h/cpp       # V4L2 采集（从 IMX6ULL 重写）
│   ├── mpp_encoder.h/cpp          # MPP 硬件编码（从 4.17 迁移）
│   ├── mpp_decoder.h/cpp          # MPP 硬件解码（从 4.17 迁移）
│   ├── rga_convert.h/cpp          # RGA 色彩转换（从 4.17 迁移）
│   ├── rtsp_server.h/cpp          # RTSP 推流（从 4.17 StreamingManager 迁移）
│   └── frame_distributor.h/cpp    # 帧分发（新写）
│
├── ai/                        # AI 推理层（从 4.17 + DeepSeek 项目迁移）
│   ├── rknn_inference.h/cpp       # RKNN 推理封装（从 4.17 rknnPool 迁移）
│   ├── yolov5_detector.h/cpp      # YOLOv5 检测（从 4.17 迁移）
│   ├── voice_interaction.h/cpp    # 语音交互主控（从 DeepSeek 项目迁移）
│   ├── asr_engine.h/cpp           # ASR 引擎（从 DeepSeek 项目迁移）
│   ├── tts_engine.h/cpp           # TTS 引擎（从 DeepSeek 项目迁移）
│   └── llm_client.h/cpp           # LLM 客户端（从 DeepSeek 项目迁移）
│
├── gui/                       # GUI 界面层（从 IMX6ULL 迁移）
│   ├── mainwindow.h/cpp           # 主窗口（从 IMX6ULL 迁移）
│   ├── settingwindow.h/cpp        # 设置窗口（从 IMX6ULL 迁移）
│   ├── clock.h/cpp                # 时钟（从 IMX6ULL 迁移）
│   ├── Map/                       # 地图模块（从 IMX6ULL 迁移）
│   │   ├── baidumap.h/cpp
│   │   └── mapconfig.h/cpp        # 地图配置（从 rk3588s_migration/map/ 迁移）
│   ├── Music/                     # 音乐模块（从 IMX6ULL 迁移）
│   │   ├── musicplayer.h/cpp
│   │   └── searchmusic.h/cpp
│   ├── Weather/                   # 天气模块（从 IMX6ULL 迁移）
│   │   ├── weather.h/cpp
│   │   ├── weatherconfig.h/cpp    # 天气配置（从 rk3588s_migration/weather/ 迁移）
│   │   └── weatherformat.h/cpp   # 天气格式化（从 rk3588s_migration/weather/ 迁移）
│   └── Monitor/                   # 监控页面（从 IMX6ULL 迁移 + 重写）
│       └── monitor.h/cpp
│
├── network/                   # 网络层（从 4.17 + IMX6ULL 迁移）
│   ├── staticmapconfig.h/cpp      # 静态地图配置（从 rk3588s_migration/map/ 迁移）
│   ├── staticmapresponse.h/cpp    # 地图响应验证（从 rk3588s_migration/map/ 迁移）
│   └── http_client.h/cpp          # HTTP 客户端（新写或从 4.17 迁移）
│
├── config/                    # 配置文件（从 IMX6ULL config/ 迁移）
│   ├── camera_settings.conf       # 摄像头配置（新写）
│   ├── vehicle_devices.conf       # 设备配置（从 IMX6ULL 迁移）
│   ├── weather_settings.conf      # 天气配置（从 IMX6ULL 迁移）
│   ├── map_settings.conf          # 地图配置（从 IMX6ULL 迁移）
│   └── app.conf                   # 应用总配置（新写）
│
├── scripts/                   # 启动脚本
│   ├── run_hdmi.sh                # HDMI 启动（从 IMX6ULL 迁移）
│   └── deploy.sh                  # 部署脚本（新写）
│
├── resources/                  # 资源文件
│   ├── img/                       # 图片（从 IMX6ULL 迁移）
│   ├── gifs/                      # GIF（从 IMX6ULL 迁移）
│   ├── Music/                     # 音乐（从 IMX6ULL 迁移）
│   ├── Weather/images/            # 天气图标（从 IMX6ULL 迁移）
│   ├── Map/img/                   # 地图图标（从 IMX6ULL 迁移）
│   └── img.qrc                    # 资源文件（从 IMX6ULL 迁移）
│
├── tests/                     # 单元测试（从 rk3588s_migration/tests/ 迁移）
│   ├── test_cameraconfig.cpp
│   ├── test_vehiclecommand.cpp
│   ├── test_weatherconfig.cpp
│   ├── test_weatherformat.cpp
│   ├── test_mapconfig.cpp
│   ├── test_mapresponse.cpp
│   ├── test_device_input.cpp
│   └── CMakeLists.txt
│
├── docs/                      # 文档（从现有 docs/ 迁移）
│   ├── ARCHITECTURE.md
│   ├── CHANGELOG.md
│   ├── ENGINEERING_PRINCIPLES.md
│   └── ...
│
├── main.cpp                   # 主入口（从 IMX6ULL 迁移，精简）
├── CarTerminal.pro            # Qt 项目文件（新写）
└── CMakeLists.txt             # CMake 项目文件（新写，用于板端编译）
```

## 三、代码来源映射

### 3.1 从 IMX6ULL VehicleTerminal 提取

| 源文件 | 目标位置 | 修改程度 |
|--------|---------|---------|
| main.cpp | main.cpp | 精简，移除硬编码 |
| mainwindow.h/cpp | gui/mainwindow.h/cpp | 配置化，统一日志 |
| settingwindow.h/cpp | gui/settingwindow.h/cpp | 配置化 |
| clock.h/cpp | gui/clock.h/cpp | 直接复用 |
| Map/baidumap.h/cpp | gui/Map/baidumap.h/cpp | 配置化 |
| Map/baidumap.ui | gui/Map/baidumap.ui | 直接复用 |
| Map/gps.h/c | 删除（板上无GPS） | - |
| Map/uart.h/c | 删除（板上无UART GPS） | - |
| Music/musicplayer.h/cpp | gui/Music/musicplayer.h/cpp | 配置化 |
| Music/musicplayer.ui | gui/Music/musicplayer.ui | 直接复用 |
| Music/searchmusic.h/cpp | gui/Music/searchmusic.h/cpp | 配置化 |
| Music/searchmusic.ui | gui/Music/searchmusic.ui | 直接复用 |
| Music/style.qss | gui/Music/style.qss | 直接复用 |
| Weather/weather.h/cpp | gui/Weather/weather.h/cpp | 配置化 |
| Weather/weather.ui | gui/Weather/weather.ui | 直接复用 |
| Monitor/monitor.h/cpp | gui/Monitor/monitor.h/cpp | 重写（配置化） |
| Monitor/monitor.ui | gui/Monitor/monitor.ui | 直接复用 |
| Monitor/capture_thread.h/cpp | media/capture_thread.h/cpp | 重写（V4L2 MPLANE） |
| Monitor/ap3216.h/cpp | device/ap3216device.h/cpp | 配置化 |
| Monitor/ap3216worker.h/cpp | device/ap3216worker.h/cpp | 配置化 |
| Monitor/i2cbusses.h/c | device/i2cbusses.h/c | 直接复用 |
| Monitor/smbus.h/c | device/smbus.h/c | 直接复用 |
| dht11.h/cpp | device/dht11device.h/cpp | 配置化 |
| dht11worker.h/cpp | device/dht11worker.h/cpp | 配置化 |
| speechrecognition.h/cpp | 删除（用 DeepSeek 项目替代） | - |
| config/vehicle_devices.conf | config/vehicle_devices.conf | 直接复用 |
| config/weather_settings.conf | config/weather_settings.conf | 直接复用 |
| config/map_settings.conf | config/map_settings.conf | 直接复用 |
| img/ | resources/img/ | 直接复用 |
| img/gifs/ | resources/gifs/ | 直接复用 |

### 3.2 从 4.17 project1 提取

| 源文件 | 目标位置 | 修改程度 |
|--------|---------|---------|
| src/mpp/mpp_encoder.cpp/h | media/mpp_encoder.h/cpp | 适配接口 |
| src/mpp/mpp_decoder.cpp/h | media/mpp_decoder.h/cpp | 适配接口 |
| src/mpp/rga_color_convert.cpp/h | media/rga_convert.h/cpp | 适配接口 |
| src/yolo/rknnPool.hpp | ai/rknn_inference.h | 重写为非模板类 |
| src/yolo/postprocess.cpp/h | ai/yolov5_postprocess.h/cpp | 适配接口 |
| src/yolo/yolov5.cpp/h | ai/yolov5_detector.h/cpp | 适配接口 |
| src/yolo/clip_encoder.cpp/h | 删除（CLIP 不需要） | - |
| src/yolo/minicpm_encoder.cpp/h | 删除（MiniCPM 不需要） | - |
| src/yolo/videodecoder.cpp/h | media/video_decoder.h/cpp | 适配接口 |
| src/yolo/camera_capture.cpp/h | 合并到 media/capture_thread | - |
| src/streaming/StreamingManager.cpp/h | media/rtsp_server.h/cpp | 重写（改 RTSP） |
| src/controller/pump_controller.cpp/h | 删除（水泵控制不需要） | - |
| third_party/rknn/ | ai/third_party/rknn/ | 直接复用 |
| third_party/ffmpeg/ | media/third_party/ffmpeg/ | 直接复用 |
| third_party/opencv/ | 删除（用 FFmpeg 替代） | - |
| third_party/rga/ | media/third_party/rga/ | 直接复用 |
| third_party/sal/ | network/third_party/sal/ | 直接复用 |
| third_party/rtsp/ | media/third_party/rtsp/ | 直接复用 |
| third_party/libdrm/ | 删除（用 Qt 显示） | - |

### 3.3 从 DeepSeek + RK3576 项目提取

> **Note (2026-07-18, task A2):** The `source/voice_gateway/` skeleton
> referenced in this table has been **formally retired** — the historical
> `rk3588s_migration/voice_gateway/` path is absent from the current
> workspace. The active voice stack lives under `RK3588S_CarTerminal/voice/`
> (`voice/asr/`, `voice/llm/`, `voice/tts/`, `voice/zmq/`,
> `voice/voice_config.*`, `voice/VoiceManager.*`). The mapping below is
> retained only as historical traceability for the original extraction
> plan; new voice work must reference `RK3588S_CarTerminal/voice/`.

| 源文件 | 目标位置 | 修改程度 |
|--------|---------|---------|
| source/voice_gateway/app/main.cpp | 删除（用 Qt 主入口替代） | - |
| source/voice_gateway/core/config/VoiceGatewayConfig.cpp/h | ai/voice_config.h/cpp | 适配接口 |
| source/voice_gateway/core/service/VoiceCommandService.cpp/h | ai/voice_interaction.h/cpp | 重写（统一接口） |
| source/voice_gateway/core/platform/Logger.cpp/h | 删除（用 logmanager 替代） | - |
| source/voice_gateway/core/transport/ZmqTextClient.cpp/h | ai/zmq_transport.h/cpp | 适配接口 |
| source/voice_gateway/core/transport/ITextClient.h | ai/transport_interface.h | 直接复用 |
| source/voice_gateway/third_party/ | ai/third_party/zmq/ | 直接复用 |
| source/common/VehicleCommand/ | 删除（用 common/vehiclecommand 替代） | - |
| source/main_process/main.py | 删除（Python 不迁移） | - |
| source/asr/engine.py | ai/asr_engine.h/cpp | C++ 重写 |
| source/wakeword/engine.py | ai/wakeword_engine.h/cpp | C++ 重写 |
| source/intent/parser.py | 删除（用 vehiclecommand 替代） | - |
| source/tts/engine.py | ai/tts_engine.h/cpp | C++ 重写 |
| source/config/llm_config.json | ai/config/llm_config.json | 直接复用 |
| source/config/tts_config.json | ai/config/tts_config.json | 直接复用 |
| source/config/wakeword_config.json | ai/config/wakeword_config.json | 直接复用 |
| source/scripts/ | scripts/voice/ | 选择性复用 |

### 3.4 从 rk3588s_migration 提取

| 源文件 | 目标位置 | 修改程度 |
|--------|---------|---------|
| common/logmanager.h/cpp | common/logmanager.h/cpp | 直接复用 |
| common/vehiclecommand.h/cpp | common/vehiclecommand.h/cpp | 直接复用 |
| device/vehicledeviceconfig.h/cpp | device/vehicledeviceconfig.h/cpp | 直接复用 |
| device/keyinputdevice.h/cpp | device/keyinputdevice.h/cpp | 直接复用 |
| device/dht11device.h/cpp | device/dht11device.h/cpp | 直接复用 |
| device/dht11worker.h/cpp | device/dht11worker.h/cpp | 直接复用 |
| device/ap3216device.h/cpp | device/ap3216device.h/cpp | 直接复用 |
| device/ap3216worker.h/cpp | device/ap3216worker.h/cpp | 直接复用 |
| map/staticmapconfig.h/cpp | network/staticmapconfig.h/cpp | 直接复用 |
| map/staticmapresponse.h/cpp | network/staticmapresponse.h/cpp | 直接复用 |
| weather/weatherconfig.h/cpp | gui/Weather/weatherconfig.h/cpp | 直接复用 |
| weather/weatherformat.h/cpp | gui/Weather/weatherformat.h/cpp | 直接复用 |
| media/cameraconfig.h/cpp | media/cameraconfig.h/cpp | 直接复用 |
| voice_gateway/ | 删除（用 DeepSeek 项目替代） | - *(已执行：voice_gateway/ 已于 2026-07-18 task A2 正式退役，当前工作区无此目录)* |
| gui/ | 删除（用 IMX6ULL GUI 替代） | - |
| tests/ | tests/ | 直接复用 |

### 3.5 从"别人的项目"和"车载座舱终端系统"提取

| 源文件 | 目标位置 | 说明 |
|--------|---------|------|
| 别人的项目 | **不提取** | 代码质量差，平台耦合重，不值得迁移 |
| 车载座舱终端系统 | **不提取** | 代码风格与本项目不一致，且基于不同硬件平台 |

## 四、迁移顺序

### Phase 1：基础框架（先跑通 GUI）
1. 创建新项目目录结构
2. 从 IMX6ULL 提取 Qt GUI 代码（mainwindow, clock, settingwindow）
3. 从 rk3588s_migration 提取 common/ 和 device/
4. 配置文件外置化
5. 编译运行，验证 GUI 能显示

### Phase 2：媒体采集
1. 从 IMX6ULL 提取并重写 capture_thread（V4L2 MPLANE）
2. 从 rk3588s_migration 提取 cameraconfig
3. 从 IMX6ULL 提取 Monitor 页面
4. 验证摄像头采集能显示

### Phase 3：传感器
1. 从 IMX6ULL 提取 DHT11、AP3216C、按键
2. 从 rk3588s_migration 提取设备配置
3. 配置化，板上无硬件则禁用

### Phase 4：网络功能
1. 从 IMX6ULL 提取地图、天气模块
2. 从 rk3588s_migration 提取配置解析器
3. 配置化 API Key

### Phase 5：媒体推流
1. 从 4.17 提取 MPP encoder/decoder
2. 从 4.17 提取 RGA 色彩转换
3. 从 4.17 提取 RTSP server
4. 集成到 capture_thread 的帧分发

### Phase 6：AI 推理
1. 从 4.17 提取 RKNN 推理封装
2. 从 4.17 提取 YOLOv5 检测
3. 集成到 capture_thread 的帧分发

### Phase 7：语音交互
1. 从 DeepSeek 项目提取 VoiceCommandService
2. 从 DeepSeek 项目提取 ZMQ 传输层
3. C++ 重写 ASR/TTS 引擎（或使用 sherpa-onnx）
4. 集成到 Qt 主窗口

## 五、执行计划

### 第一步：创建新项目骨架
- 创建 RK3588S_CarTerminal/ 目录结构
- 创建 CarTerminal.pro
- 创建 CMakeLists.txt（用于板端编译）
- 创建 main.cpp（精简版）

### 第二步：迁移 Phase 1 代码
- 从 IMX6ULL 复制 GUI 代码
- 从 rk3588s_migration 复制 common/ 和 device/
- 修改 include 路径和命名空间
- 编译验证

### 第三步：上传到板上测试
- 打包上传整个 RK3588S_CarTerminal/
- 板上 qmake && make
- 运行验证

### 第四步：逐模块迁移
- 按 Phase 2-7 顺序逐个模块迁移
- 每个模块迁移后编译验证

## 六、关键决策

### D019: 不迁移"别人的项目"和"车载座舱终端系统"
**原因**: 代码质量差，平台耦合重，风格不一致，迁移成本高于重写。
**影响**: 减少迁移工作量，但需要自己写部分功能。

### D020: DeepSeek 项目的 Python 代码不迁移
**原因**: Python 代码无法直接集成到 C++ Qt 项目中。
**替代**: 使用 C++ 重写 ASR/TTS，或使用 sherpa-onnx 开源库。

### D021: 不使用 OpenCV
**原因**: OpenCV 体积大，编译慢，且 RK3588S 有 RGA 硬件加速。
**替代**: 使用 RGA 做色彩转换和缩放，FFmpeg 做编解码。

### D022: 不使用 LVGL
**原因**: 项目已有成熟的 Qt GUI 代码，迁移成本低。
**替代**: 继续使用 Qt5。

## 七、风险评估

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 文件路径混乱 | 编译失败 | 统一使用相对路径，不使用中文路径 |
| 接口不兼容 | 编译失败 | 统一命名空间和接口风格 |
| 依赖库缺失 | 链接失败 | 提前确认板上已安装的库 |
| 性能不足 | 运行卡顿 | 使用 RGA/MPP 硬件加速 |
| 板上无传感器 | 功能禁用 | 配置化，默认禁用 |
