# Architecture

## Architecture Principle

The RK3588S cockpit terminal must be layered. GUI must not control hardware directly, AI must not call widgets directly, and device paths must come from configuration.

Target layering:

```text
GUI layer
  -> service/domain layer
    -> common interfaces and command models
      -> device/media/voice/AI adapters
        -> Linux device nodes, libraries, models, network endpoints
```

## Current Physical Layout

```text
RK3588S_CarTerminal/                  # 当前主项目（所有代码修改应在此目录进行）
├── common/                           # logmanager, vehiclecommand
├── device/                           # vehicledeviceconfig, dht11device, dht11worker, dht11 (Qt适配器)
│                                     # ap3216device, ap3216worker, ap3216 (Qt适配器)
│                                     # keyinputdevice, i2cbusses, smbus (遗留)
├── media/                            # cameraconfig, capture_thread, rga_converter, mpp_decoder, mpp_encoder, rknn_detector
├── network/                          # staticmapconfig, staticmapresponse
├── gui/                              # mainwindow, settingwindow, settinggifscanner, clock, speechrecognition
│   ├── Map/                          # baidumap, gps
│   ├── Music/                        # musicplayer, searchmusic, style.qss
│   ├── Weather/                      # weather, weatherconfig, weatherformat
│   └── Monitor/                      # monitor
├── config/                           # vehicle_devices.conf, camera_settings.conf, map_settings.conf, weather_settings.conf
├── resources/                        # Qt资源文件（图标/GIF/样式）
├── scripts/                          # run_hdmi.sh（板端启动脚本）
├── myMusic/                          # 测试音乐文件目录
├── docs/                             # 11个主工程文档 + 2个设计规格文档
├── CarTerminal.pro                   # Qt项目文件
└── main.cpp                          # 程序入口
```

## Layer Responsibilities

### GUI Layer

Current path:

```text
RK3588S_CarTerminal/gui/
```

Responsibilities:

- Display pages and user controls.
- Emit user actions.
- Display data passed from lower layers.
- Call narrow service/device interfaces only.

Forbidden:

- Opening `/dev/*` directly.
- Hardcoding RK3588S or IMX6ULL device paths.
- Running AI/model logic directly.
- Owning hardware polling loops.

### Common Layer

Current path:

```text
RK3588S_CarTerminal/common/
```

Responsibilities:

- Shared domain data types.
- Stable command parsing interface.
- Code usable by both Qt GUI and service processes.

Current interface:

```cpp
vehiclecommand::CabinCommand parseCabinCommand(const std::string &text);
const char *toString(vehiclecommand::CabinCommand command);
```

### Device Layer

Current path:

```text
RK3588S_CarTerminal/device/
```

Responsibilities:

- Parse hardware/device configuration.
- Own configurable hardware adapters.
- Keep device paths outside GUI.
- Return errors through explicit results/log messages.

Current supported configuration:

- Key input.
- GPS serial input.
- DHT11 temperature/humidity input.
- AP3216 light/proximity/infrared sysfs input.

### Map Configuration Layer

Current path:

```text
RK3588S_CarTerminal/network/
```

Responsibilities:

- Parse static map network configuration.
- Build static map request URLs from validated config.
- Validate static map HTTP responses before GUI rendering.
- Keep map URL templates, API keys, fallback coordinates, image size, and zoom defaults out of GUI source.

Current supported configuration:

- Baidu static image URL template.
- API key.
- Default coordinate fallback.
- Width, height, scale, copyright, and zoom range.
- Output image file path.
- PNG/JPEG response validation.

### Weather Helper Layer

Current path:

```text
RK3588S_CarTerminal/gui/Weather/
```

Responsibilities:

- Provide GUI-independent formatting and validation helpers for weather data.
- Prevent provider/network errors from crashing Qt widgets.
- Parse weather API configuration outside the Qt widget.
- Build weather request URLs from validated configuration.
- Keep weather provider URL, appid, appsecret, city, and timeout outside GUI source.

Current supported behavior:

- Validate `yyyy-mm-dd` date text before converting it to display-safe `MM-DD`.
- Return explicit failure and a fallback display string for malformed dates.
- Parse `config/weather_settings.conf` through `vehicleweather::WeatherConfig`.
- Default weather requests to disabled until valid provider credentials are configured.
- Apply request timeout on Qt versions that support `QNetworkRequest::setTransferTimeout`.

### Voice Gateway Layer

Current path:

```text
RK3588S_CarTerminal/voice/
```

Responsibilities:

- Load voice pipeline config from `config/voice_settings.conf`.
- Manage ASR (in-process via sherpa-onnx dlopen), LLM (separate process), TTS (separate process).
- ZMQ inter-process communication: ASR→LLM (port 6666), LLM→TTS (port 7777), TTS status (port 6677).
- VoiceManager orchestrates QProcess lifecycle for LLM and TTS executables.
- Convert voice text to stable cockpit commands via `vehiclecommand`.

Thread safety model:

- ASR runs in a dedicated worker thread (`m_asrThread`) with `poll()` + 200ms timeout for clean shutdown.
- LLM ZMQ requests run in managed worker threads (`m_workerThreads`), joined via swap-then-join pattern in `deinit()`.
- `m_destroying` atomic flag suppresses signal emission during destruction to prevent use-after-free on queued receivers.
- Worker lambdas capture `m_llmClient` pointer by value to avoid dereferencing after `deinit()` deletes it.
- All GUI interaction goes through Qt signal/slot (`emit asrTextRecognized` → queued to main thread).

Safety defaults:

- `llm_enabled=false`, `tts_enabled=false` — never auto-start.
- `direct_command_enabled=true` — offline command path works out of the box.
- `unknown_command_fallback_to_llm=true` — only effective when `llm_enabled=true`.
- **Dual-path routing** (section 六 of the refactor spec):
  - Path 1 (default, no NPU dependency): ASR text → vehiclecommand keyword match → Qt GUI direct action. Fixed-command matches NEVER go to LLM.
  - Path 2 (optional, requires `llm_enabled=true`): unmatched ASR text → LLM fallback when `unknown_command_fallback_to_llm=true` AND LLM process is running AND ZMQ client is ready.
  - Path 3: unmatched + LLM unavailable → log and update status; never crash or block GUI.
- Routing decision is a pure Qt-free function `voice::decideVoiceRoute()` in `voice/voice_config.h` so it is unit-testable. `MainWindow::onOfflineAsrTextRecognized` mirrors this helper exactly.

Safety defaults (section 二):

- `voice_enabled=false` (entire pipeline off; app still runs).
- `direct_command_enabled=true` (offline ASR → fixed command → GUI works out of the box).
- `llm_enabled=false` (NPU driver v0.9.6 triggers SIGBUS in `rkllm_run`; must stay false until driver upgraded to v0.9.7+).
- `tts_enabled=false` (board test phase).
- `unknown_command_fallback_to_llm=true` (only takes effect when LLM is enabled and running).
- `parseBool` accepts `true/false`, `1/0`, `yes/no`, `on/off` (case-insensitive). Illegal values use safe defaults and emit a WARN log. Never throws.
- Missing config file returns a default-constructed `VoiceConfig` and logs a WARN. Never throws.

### Media Layer

Current status:

- V4L2采集已迁移到 `RK3588S_CarTerminal/media/capture_thread.*` 和 `media/cameraconfig.*`。
- MPLANE API重写完成，NV12格式，状态机+错误码。
- RGA硬件加速已集成到 `media/rga_converter.*`：通过dlopen加载librga.so，imcvtcolor_t API，NV12→RGB32硬件转换，CPU降级可用。
- MPP硬件解码已集成到 `media/mpp_decoder.*`：通过dlopen加载librockchip_mpp.so，MJPEG→NV12硬件解码，已集成到capture_thread MJPEG分支。✅ **ABI修复（第一次修改）**：decoder旧代码使用错误签名 `mpp_create(MppCtx*, MppCodingType)` / `mpp_init(MppCtx, MppCodingType)`，已改为与encoder一致的正确ABI `mpp_create(MppCtx*, MppApi**)` / `mpp_init(MppCtx, MppCtxType, MppCodingType)`，解码通过MppApi函数表调用。✅ **buffer语义修复**：`mpp_frame_get_buffer()` 返回 `MppBuffer` 句柄，必须通过 `mpp_buffer_get_ptr_with_caller()` 取得真实帧指针；decoder 已按该流程读取帧，并将 NV12/I420 输出归一到调用者的 NV12 缓冲。librockchip_mpp.so不可用时软失败，NV12路径不受影响。本次不处理零拷贝链路。
- MPP硬件编码已实现 `media/mpp_encoder.*`：通过dlopen加载librockchip_mpp.so，NV12→H.264/H.265硬件编码，待集成到推流路径。

Target responsibilities:

- Own V4L2 capture.
- Own frame format conversion (RGA硬件加速).
- Own MPP hardware decode/encode.
- Own RTSP preview/push integration (MPP编解码).
- Expose clean service APIs to GUI.

### AI Layer

Current status:

- RKNN NPU推理已实现 `media/rknn_detector.*`：通过dlopen加载librknnrt.so，YOLOv5目标检测（80类COCO），letterbox resize + sigmoid + NMS后处理 + 检测框绘制。
- 已集成到Monitor页面：摄像头帧到达时自动运行YOLOv5推理并绘制检测结果。
- 配置通过`config/rknn_settings.conf`外置控制（默认禁用）。

Target responsibilities:

- Own model path configuration.
- Own RKNN/NPU model loading and inference.
- Expose results through service/domain interfaces.

## Configuration Strategy

Current active config:

```text
RK3588S_CarTerminal/config/vehicle_devices.conf
RK3588S_CarTerminal/config/camera_settings.conf
RK3588S_CarTerminal/config/map_settings.conf
RK3588S_CarTerminal/config/weather_settings.conf
RK3588S_CarTerminal/config/rknn_settings.conf
RK3588S_CarTerminal/config/asr_settings.conf
```

Example templates (copy-and-fill, no real keys):

```text
RK3588S_CarTerminal/config/weather_settings.example.conf
RK3588S_CarTerminal/config/map_settings.example.conf
RK3588S_CarTerminal/config/asr_settings.example.conf
```

Rules:

- Hardware paths must be configured.
- Missing config must fail soft where possible.
- Enabled hardware with missing path must fail with a clear error.
- Config changes must be documented in `docs/INTERFACE_SPEC.md`.
- Config files must not ship real API keys/secrets/tokens. Use `*.example.conf` templates with `YOUR_*` placeholders; developers copy to the real `.conf` and fill in credentials locally.
- Placeholder values (`YOUR_WEATHER_APP_ID`, `YOUR_BAIDU_MAP_KEY`, `YOUR_ASR_CUID`, etc.) must never produce network requests. Enabled online-service configs reject missing/placeholder credentials; disabled template configs may keep placeholders for copy-and-fill workflows.
- Environment variable overrides: `VEHICLE_WEATHER_CONFIG`, `VEHICLE_MAP_CONFIG`, `VEHICLE_ASR_CONFIG` may point to alternative config file paths for local development.

## Event and Threading Strategy

Rules:

- Do not use naked `while(1)`.
- Do not use `sleep` or `usleep` as a fake event mechanism.
- Use Qt signals/timers for GUI.
- Use `poll`, event fd, callback, message queue, or controlled thread loop for device/service code.
- Every thread must have a clear stop condition and resource release path.

Current implemented example:

- GPS serial reading uses a running flag plus `poll()`.
- GPS exposes `gps_shutdown()` for cleanup.
- Key input is configurable and does not block GUI startup when disabled.
- DHT11 uses a device adapter with nonblocking open plus `poll()` read timeout, and Qt polling runs in `Dht11Worker` instead of the GUI thread.
- AP3216 uses a sysfs device adapter plus `Ap3216Worker` on a Qt worker thread. The Monitor page receives readings through signals and does not open or poll AP3216 files directly.

## Boundary Rule

When adding a new function:

- GUI may call service/device public interfaces.
- Service/domain may call common interfaces.
- Device/media/AI adapters may call OS/library APIs.
- Lower layers must not call GUI widgets.
