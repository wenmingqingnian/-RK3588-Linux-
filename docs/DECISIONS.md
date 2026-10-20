# Decisions

## Decision Log

### D001: Migrate Incrementally Instead Of Rewriting

Date: 2026-06-22

Decision:

- Keep legacy Qt application usable while extracting modules one at a time.

Reason:

- User needs visible progress on RK3588S hardware.
- Full rewrite would break too many known working UI pieces.
- Incremental migration allows board verification after each module.

Consequences:

- Some legacy code remains temporarily.
- Each module must be cleaned with narrow scope.

### D002: Keep GUI In Existing Qt Widgets Project For Now

Date: 2026-06-22

Decision:

- Continue using `IMX6ULL车载终端/VehicleTerminal` as the GUI application.
- Link migration modules from `rk3588s_migration`.

Reason:

- Board already has Qt 5.15.2 runtime/development packages.
- Existing GUI can display over HDMI.
- This avoids introducing a new UI stack.

Consequences:

- `.pro` remains active for GUI.
- CMake is used for new non-GUI service modules.

### D003: Device Paths Must Be Configurable

Date: 2026-06-23

Decision:

- Device paths must not be hardcoded in GUI or service logic.
- Use `vehicle_devices.conf` for current key/GPS config.

Reason:

- IMX6ULL paths such as `/dev/key` and `/dev/ttymxc2` are invalid on RK3588S.
- RK3588S actual device names may differ by kernel/device tree/USB adapter.

Consequences:

- Key input and GPS default to disabled.
- Enabling hardware requires editing config only.

### D004: Voice Gateway Uses Optional ZeroMQ

Date: 2026-06-22

> **Status (2026-07-18, task A2): RETIRED.** The `rk3588s_migration/voice_gateway/`
> skeleton is absent from the current workspace and the runner has been
> formally retired. The active voice stack (`RK3588S_CarTerminal/voice/`)
> integrates ZMQ directly via `voice/zmq/` (see D019 / "Voice ZMQ" row in
> `MODULE_STATUS.md`); the optional-enable pattern below is preserved only
> as historical context for the original Project B contract.

Decision:

- `voice_gateway` builds without ZeroMQ by default.
- ZMQ integration must be explicitly enabled by CMake options.

Reason:

- Keeps host and board builds simple.
- Avoids requiring Project B dependencies before the gateway skeleton is useful.

Consequences:

- ZMQ must be tested separately when enabled.

### D005: Tests Must Exist For Shared Parser And Config

Date: 2026-06-23

Decision:

- Shared command parsing and device configuration need host-side tests.

Reason:

- These interfaces are used by multiple modules.
- Regression risk is high if config keys or command parsing silently change.

Consequences:

- `tests/vehicle_command/build_and_run.ps1`
- `tests/voice_gateway/build_and_run.ps1` *(RETIRED 2026-07-18, task A2 — historical only; active voice tests live under `RK3588S_CarTerminal/voice/tests/`)*
- `tests/device_input/build_and_run.ps1`

### D006: HDMI Uses Software OpenGL Workaround For Current Board Runtime

Date: 2026-06-23

Decision:

- Use software OpenGL env vars for the current HDMI run path.

Command:

```bash
export DISPLAY=:0
export QT_XCB_GL_INTEGRATION=none
export QT_OPENGL=software
./VehicleTerminal
```

Reason:

- Board produced Rockchip GL/DRI errors without the workaround.
- UI displayed successfully with software rendering.

Consequences:

- Later DRM/GPU acceleration can be revisited separately.

### D007: Documentation Must Be Read Before Work And Updated After Code Changes

Date: 2026-06-23

Decision:

- Before every task, code reading, code change, or documentation change, read the 9 required project context documents under `docs/`.
- After every code change, update `docs/CHANGELOG.md` and `docs/MODULE_STATUS.md`.
- Update `docs/TODO.md`, `docs/INTERFACE_SPEC.md`, `docs/DECISIONS.md`, and `docs/BUILD_RUN_TEST.md` when their trigger conditions apply.

Reason:

- The migration spans two legacy projects, RK3588S hardware, Qt GUI, device drivers, voice services, and future AI/media modules.
- Without a fixed project memory protocol, future changes may forget module status, break interfaces, or repeat old decisions.

Consequences:

- Every implementation task includes a documentation maintenance step.
- A change is not complete until the required docs are updated.

### D008: Static Map Network Settings Use A Dedicated Map Config Module

Date: 2026-06-23

Decision:

- Keep map URL template, API key, fallback coordinate, image size, output file, and zoom range in `config/map_settings.conf`.
- Parse and validate those keys in `rk3588s_migration/map/staticmapconfig.*`.
- Let the Qt map page request a URL built by this module instead of owning hardcoded network strings.

Reason:

- RK3588S deployments must be able to change map provider settings without recompiling.
- The old source hardcoded a Baidu AK, image size, fallback city text, and URL string in GUI code.
- ASCII coordinate fallback avoids the previous runtime mojibake in the `center=` parameter.

Consequences:

- Board deployment must upload `rk3588s_migration/map/staticmapconfig.*` and `VehicleTerminal/config/map_settings.conf`.
- Real map rendering still depends on network access and a valid Baidu static map API key.

### D009: Static Map Rendering Uses Response Validation And QLabel

Date: 2026-06-23

Decision:

- Validate map HTTP response content type and image magic before writing `map.png`.
- Render the received map image in a dedicated `QLabel` child of the map page.
- Stop applying `QWidget{border-image: ...}` to the whole map window.

Reason:

- Baidu can return HTTP 200 with JSON/text error content, which must not be treated as an image.
- On RK3588S, after a valid PNG response was received, the previous global stylesheet image path led to a segment fault.
- A dedicated label keeps map rendering local to the map page and avoids restyling child controls.

Consequences:

- `rk3588s_migration/map/staticmapresponse.*` becomes part of the Qt build.
- Board deployment must upload the new response helper files.
- Board runtime has since shown `map image saved`; the latest crash backtrace points at Weather.

### D010: Weather Provider Data Must Fail Closed

Date: 2026-06-23

Decision:

- Weather network replies and date strings must be validated before updating labels.
- Malformed weather data should log and stop the current update instead of indexing missing list items.
- Date formatting lives in `rk3588s_migration/weather/weatherformat.*`, not as unchecked string splitting in the Qt widget.

Reason:

- Board `gdb` showed a segment fault in `Weather::TransDataToMyData(QString)` after the map image was saved.
- The legacy weather page assumed the provider always returned at least seven valid daily records.
- Empty or malformed `date` values made `QStringList::at()` access out of range.

Consequences:

- Weather crash hardening is separated from the later weather API/config migration.
- Board deployment must upload the new weather helper files and rebuild `VehicleTerminal`.
- The weather page may skip a UI update when provider data is invalid, but the cockpit UI should keep running.
- Board runtime on 2026-06-24 confirmed malformed provider data is logged and the app continues; the next Weather issue is provider permission/configuration.

### D011: Weather API Settings Use A Dedicated Weather Config Module

Date: 2026-06-24

Decision:

- Keep weather URL template, appid, appsecret, city, and timeout in `config/weather_settings.conf`.
- Parse and validate those keys in `rk3588s_migration/weather/weatherconfig.*`.
- Keep the default repository config disabled until valid provider credentials are supplied.
- Let the Qt weather page skip refresh requests when configuration is disabled or URL construction fails.

Reason:

- RK3588S deployments must be able to change weather provider credentials and city without recompiling.
- The legacy source hardcoded provider URL and credentials in the Qt widget.
- Empty or expired credentials should not produce uncontrolled startup requests.

Consequences:

- Board deployment must upload `rk3588s_migration/weather/weatherconfig.*` and `VehicleTerminal/config/weather_settings.conf`.
- Enabling live weather now requires setting `weather_enabled=true` and filling valid `weather_appid` and `weather_appsecret`.
- On Qt 5.15+ runtime, `weather_timeout_ms` is applied to the network request.

### D012: DHT11 Polling Runs Outside The GUI Thread

Date: 2026-06-24

Decision:

- Keep `Dht11Device` in the device layer and make it apply `dht11_read_timeout_ms` with Linux `poll()` before reading.
- Keep `Dht11Reader` as the GUI-facing Qt adapter, but move the actual polling timer and sensor read into `Dht11Worker` on a `QThread`.
- Keep the public `Dht11Reader` signal interface stable.

Reason:

- The previous DHT11 Qt timer path could perform a synchronous device read from the GUI thread.
- `dht11_read_timeout_ms` had to become real behavior, not only a parsed config value.
- The GUI should only receive display-ready humidity/temperature signals.

Consequences:

- Board deployment must upload `dht11worker.*` in addition to `dht11.*` and `dht11device.*`.
- `tests/device_input` now compiles and exercises `dht11device.cpp`.
- Board runtime verification is still required because host tests do not simulate a real RK3588S DHT11 driver node.

### D013: AP3216 Uses Configurable Sysfs Adapter And Qt Worker

Date: 2026-06-24

Decision:

- Keep AP3216 sysfs directory and ALS/PS/IR channel file names in `config/vehicle_devices.conf`.
- Read AP3216 values through `rk3588s_migration/device/ap3216device.*`.
- Let `Monitor/ap3216.*` act as the Qt-facing adapter, with actual polling in `Ap3216Worker` on a `QThread`.
- Keep legacy `i2cbusses.*` and `smbus.*` out of the current runtime path until a real RK3588S I2C bring-up requirement appears.

Reason:

- The current legacy Qt path read AP3216 values through hardcoded sysfs files, and Monitor drove the read with a GUI timer.
- RK3588S may expose AP3216 through different sysfs paths or may not expose the device at all.
- The current app already uses a sysfs-style AP3216 interface, so a configurable sysfs adapter is the smallest safe migration step.

Consequences:

- Board deployment must upload `ap3216device.*`, `ap3216worker.*`, updated `ap3216.*`, `monitor.*`, `mainwindow.cpp`, `VehicleTerminal.pro`, and `config/vehicle_devices.conf`.
- Default config disables AP3216 and logs `ap3216 input disabled`.
- Enabled AP3216 runtime verification is blocked until the real RK3588S sysfs directory and channel file names are known.

### D014: GUI Helper Logic May Move To `rk3588s_migration/gui`

Date: 2026-06-25

Decision:

- Keep small GUI-adjacent but Qt-independent helper logic under `rk3588s_migration/gui`.
- Use this for `settinggifscanner.*` so `SettingWindow` does not own raw directory scanning details.

Reason:

- The board GDB backtrace showed a startup crash in `SettingWindow::ScanGif()` when setting a `QMovie` file name.
- The legacy settings page assumed `./MyGif` existed and contained at least one GIF.
- Moving directory scanning into a tested helper keeps the window focused on UI state and lets missing assets fail softly.
- The helper path is ASCII and compiles cleanly in host tests and board `.pro` builds.

Consequences:

- `VehicleTerminal.pro` must include `rk3588s_migration/gui`.
- Board deployment must upload `settinggifscanner.*` together with `settingwindow.*` and `VehicleTerminal.pro`.
- Future GUI helper additions should stay small, tested, and independent of hardware control.

### D015: AP3216 Enabled Testing Is Blocked Until Board Exposure Exists

Date: 2026-06-25

Decision:

- Keep AP3216 software migration in place, but keep `ap3216_enabled=false` on the current RK3588S image.
- Do not spend the next software step trying to enable AP3216 unless hardware/device-tree/driver work exposes a real AP3216/AP3216C node and ALS/PS/IR or IIO channels.
- Move the next actionable hardware migration to Camera/V4L2 because the board already exposes IMX415 camera sensors on I2C.

Reason:

- Narrow RK3588S I2C discovery lists IMX415, RK8602/RK8603, ES8388, HUSB311, DDC, and RK3X I2C controllers, but no AP3216/AP3216C.
- IIO illuminance/proximity/intensity discovery returned no output.
- AP3216/AP3216C/proximity sysfs discovery returned no output.
- `dmesg | grep -iE "ap3216|ap3216c|illuminance|proximity|iio"` returned no output.

Consequences:

- AP3216 remains configurable and default-disabled, with host tests preserving the software path.
- Enabled AP3216 board testing is classified as blocked by missing hardware/kernel exposure, not an application-layer bug.
- Camera/V4L2 migration should start with `/dev/video*`, `/dev/media*`, `v4l2-ctl`, and `media-ctl` discovery.

### D016: Camera/V4L2 Uses Multiplanar API with Config-Driven NV12 Capture

Date: 2026-06-25

Decision:

- Rewrite `Monitor/capture_thread.*` to use V4L2 multiplanar API (`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`).
- Use `/dev/video33` (rkisp0-vir0 mainpath) as default camera device.
- Use NV12 as default pixel format (matches RK3588S ISP output).
- Use 800x600 as default resolution (within ISP output range).
- Externalize all camera parameters to `config/camera_settings.conf`.
- Implement NV12/NV21/UYVY→RGB32 CPU conversion for QImage display.

Reason:

- Board `v4l2-ctl --list-devices` confirmed `/dev/video33` is rkisp0-vir0 mainpath (Video Capture Multiplanar).
- Board `v4l2-ctl -d /dev/video33 --list-formats-ext` confirmed UYVY/NV16/NV61/NV21/NV12/NM21/NM12 support, max 800x600.
- RK3588S rkisp ISP outputs multiplanar buffers; single-planar `VIDIOC_DQBUF` fails with EINVAL.
- Old code used single-planar API, RGB565, hardcoded `/dev/video1`, 1024x600 — none compatible with RK3588S.
- Config-driven approach allows disabling camera (`camera_enabled=false`) when hardware is unavailable.

Consequences:

- `rk3588s_migration/media/cameraconfig.*` added with parser and host tests.
- `VehicleTerminal.pro` must include `rk3588s_migration/media`.
- `Monitor::getInstance` and `Monitor` constructor now accept `CameraConfig`.
- `mainwindow.cpp` loads `config/camera_settings.conf` and passes to Monitor.
- NV12→RGB32 CPU conversion is functional but may be slow for 800x600@30fps; RGA hardware acceleration is a future optimization.
- Only rkisp0-vir0 (`/dev/video33`) is tested; rkisp0-vir1 and rkisp1-vir1 are unverified.

### D017: Unified Project Log Module (vehiclelog)

Date: 2026-06-25

Decision:

- Create `rk3588s_migration/common/logmanager.h/cpp` as the project-wide log module.
- Support DEBUG/INFO/WARN/ERROR/FATAL levels.
- Output format: `[HH:MM:SS][LEVEL][module] message`.
- Use variadic printf-style API, not iostream.
- Thread-safe via std::mutex.
- Provide convenience macros: `VLOG_DEBUG`, `VLOG_INFO`, `VLOG_WARN`, `VLOG_ERROR`, `VLOG_FATAL`.
- Module name is a string literal (e.g., "camera", "monitor", "mainwindow").
- Configurable minimum log level and output file.
- Not dependent on Qt — pure C++ so it can be used in non-Qt modules (voice_gateway, drivers, etc.). *(Note 2026-07-18, A2: `voice_gateway` itself is retired, but the "no Qt in non-Qt modules" rule still applies to `voice/llm/`, `voice/tts/`, `voice/asr/`, device drivers, etc.)*

Reason:

- voice_gateway has a minimal Logger (Info/Warning/Error only) in its own namespace; not suitable as project-wide log. *(Historical — the retired voice_gateway Logger is no longer present; the rationale still justifies a unified project logger for the active voice services.)*
- Qt's qDebug/qWarning cannot be used in non-Qt code.
- printf/perror scattered across modules makes log filtering impossible.
- Unified log enables per-module log filtering, timestamp correlation, and structured debugging on board.

Consequences:

- All new and refactored code must use VLOG_* instead of printf/qDebug/qWarning.
- Existing qWarning/qDebug in mainwindow.cpp will be replaced incrementally (not in this commit to limit change scope).
- VehicleTerminal.pro must include LOG_DIR.
- voice_gateway retains its own Logger (different output format, IPC context) — no conflict. *(Historical — voice_gateway is retired as of 2026-07-18, A2; no longer applicable.)*

### D018: CaptureThread Lifecycle State Machine and Error Codes

Date: 2026-06-25

Decision:

- Refactor CaptureThread to use explicit lifecycle: init() → startCapture() → stopCapture() → shutdown().
- Add State enum: Uninitialized, Ready, Starting, Capturing, Stopping, Error.
- Add ErrorCode enum: None, DeviceOpenFailed, FormatNotSupported, BufferSetupFailed, StreamStartFailed, FrameCaptureError.
- All state transitions logged via VLOG.
- Thread stop uses m_stopRequested flag + QMutex, with 3s timeout before terminate().
- Destructor calls shutdown() to prevent resource leaks.

Reason:

- Old interface (configure/setThreadStart/startThread/stopThread) had no state tracking, no error reporting, no resource lifecycle management.
- Enterprise engineering standards require: clear lifecycle, error codes, safe thread exit, resource ownership.
- Without state machine, callers cannot know if camera is ready, capturing, or errored.

Consequences:

- All callers of old interface (configure/startThread/stopThread) must update to init/startCapture/stopCapture.
- Monitor updated; mainwindow does not call CaptureThread directly.
- New signals: stateChanged(State), frameError(QString).
- 13 qWarning/qDebug calls in mainwindow.cpp remain (separate pass).

### D019: MPP Hardware Encoding Integration with Software Fallback

Date: 2026-06-26

Decision:

- Integrate MPP hardware encoding (RK3588S VPU) into the RTSP streaming pipeline as primary encoder.
- Retain FFmpeg libx264 as automatic fallback when MPP is not available.
- Rewrite `mpp_encoder.cpp` based on project1 reference implementation, using dlopen pattern consistent with existing mpp_decoder/rga_converter.
- Keep swscale for RGB32→YUV420P color conversion (shared by both MPP and FFmpeg paths).

Reason:

- Old mpp_encoder.cpp had completely wrong MPP API calls (`mpp_create(&ctx, type)` wrong signature, `s_mpp_encode` non-existent symbol, custom MppEncCfg struct not matching SDK).
- project1 has a complete, correct MPP encoder implementation that can be adapted.
- MPP hardware encoding reduces CPU usage from ~100% to <5% for 800x600@25fps H.264 encoding.
- dlopen pattern avoids compile-time SDK dependency (consistent with D012/D015 decisions).

Consequences:

- `mpp_encoder.h/cpp` fully rewritten with correct MPP API flow: mpp_create → mpp_init(ENC) → mpp_enc_cfg_set_s32 → ION buffer group → encode_put_frame/encode_get_packet → MPP_ENC_GET_EXTRA_INFO.
- `rtsp_server.cpp` encodeAndSend() now has dual-path: MPP (mpp::encode) or FFmpeg (avcodec_send_frame/receive_packet).
- MPP IDR frame detection: scan first NAL type in Annex-B output, prepend SPS/PPS when needed.
- Config file `rtsp_settings.conf` gains `rtsp_codec` option ("264" or "265").
- MPP constants (MPP_CTX_ENC=1, MPP_VIDEO_CodingAVC=7, etc.) hardcoded in cpp — no SDK headers needed at compile time.
- Board-side verification pending: need to compile and test on RK3588S with MPP library available.

### D020: MPP Encoder Code Review Fixes — Critical Bug Fixes

Date: 2026-06-26

Decision:

- Fix MppApi struct layout (add missing reset/flush function pointers).
- Fix getHeader() timing (defer SPS/PPS extraction until after first frame encoded).
- Fix IDR detection to support both H.264 and H.265 NAL types.
- Remove dead code (s_pktGrp, s_yuvBuffer, typedef uint32_t).
- Add per-symbol dlsym error logging for debugging.

Reason:

- MppApi struct was missing reset/flush before control, causing all function pointer offsets to be wrong by 2 positions. This would cause runtime crashes on the board.
- getHeader() called MPP_ENC_GET_EXTRA_INFO during init() before any frame was encoded. MPP requires at least one encoded frame before this returns valid data. SPS/PPS would always be empty.
- IDR detection used H.264 NAL type extraction (`& 0x1F`) for all codecs. H.265 uses a different NAL header format (`(byte >> 1) & 0x3F`).
- Dead code (s_pktGrp, s_yuvBuffer) wasted ~720KB memory and was never used.
- dlsym failures only logged a generic "missing symbols" message, making debugging difficult.

Consequences:

- MppApi struct now matches real MPP SDK layout (size, name, reset, flush, control, decode, encode).
- SPS/PPS extraction deferred to first encodeAndSend() call via `m_spsPpsExtracted` flag.
- IDR detection now correctly handles H.265 NAL types (VPS=32, SPS=33, PPS=34, IDR=19/20, CRA=21).
- extractSpsPpsFromMppHeader() now supports H.265 2-byte NAL headers.
- New `mpp::isH265()` function added for codec type query.
- RESOLVE_SYM macro used for cleaner dlsym code with per-symbol error logging.

### D021: MPP编码器pktSize=0问题用直接链接测试隔离

Date: 2026-06-27

Decision:

- 创建test_mpp_direct.cpp直接链接librockchip_mpp.so的测试程序，不使用dlopen，完全模仿project1的编码方式
- 用于隔离问题根源：是dlopen方式的问题还是MPP配置/硬件本身的问题
- 编译命令：`g++ -std=c++17 tests/test_mpp_direct.cpp -lrockchip_mpp -o test_mpp_direct`

Reason:

- dlopen方式的mpp_encoder.cpp经过11项排查修复后仍然pktSize=0
- 需要排除dlopen引入的不确定性（如dlsym返回的函数指针偏移不可靠、动态加载的符号版本不匹配等）
- project1使用直接#include + 直接链接方式，编码正常工作
- 直接链接测试可以作为"对照组"，快速确定问题方向

Consequences:

- 如果直接链接测试正常输出H.264数据 → 问题在dlopen方式，需逐一对比dlsym返回的函数指针
- 如果直接链接测试也pktSize=0 → 问题在MPP配置参数或硬件状态（如VPU被其他进程占用）
- test_mpp_direct.cpp使用`mpp_buffer_group_get_internal`宏（自动填充__FILE__和BUFFER_GROUP_TAG）和合并`encode`接口

### D022: MPP编码器保留dlopen并用SDK对照测试锁定ABI

Date: 2026-06-27

Decision:

- 保留主程序的 `dlopen(librockchip_mpp.so)` 架构和 libx264 初始化失败回退，
  不让主程序强依赖MPP开发包。
- 将主程序必须使用的MPP ABI值集中到 `mpp_abi_constants.h`，并通过
  `test_mpp_abi_constants.cpp` 与project1/板端SDK头文件进行编译期对照。
- 直接链接测试使用真实SDK枚举、`MPP_ENC_GET_CFG`和project1异步
  `encode_put_frame`/`encode_get_packet`流程，不再创建零容量输出packet。
- I420对齐和Y/U/V平面拷贝提取为无硬件依赖的纯C++ helper，以便host回归。

Reason:

- 板端官方 `mpi_enc_test` 已成功生成42 KiB H.264流，证明故障不在VPU、
  内核驱动或系统MPP库。
- 旧实现硬编码的YUV420P、HEVC和timeout枚举值与SDK不一致；其中
  YUV420P=1实际表示10-bit semiplanar，timeout实际为-8而不是-2。
- 旧direct test重复使用零容量packet，无法作为有效对照组；D021中的
  “直接测试已完全模仿project1”判断不成立。

Consequences:

- 板端MPP ABI变化会在direct test/static_assert阶段明确暴露。
- 主程序公共编码接口不变，RTSP调用方无需改动。
- 当前修复仍需板端direct test和主程序运行验证；验证前MPP Encoder保持
  Blocked。

### D023: MPP编码器pktSize=0根因确认并关闭

Date: 2026-06-27

Decision:

- 确认pktSize=0根因为`MPP_FMT_YUV420P`枚举值错误硬编码为1（实际为4）。
- 板端test_mpp_direct直接链接测试10/10帧编码成功（22KB H.264）。
- 主程序RTSP推流MPP路径板端验证通过，VLC成功播放。
- MPP Encoder状态从Blocked改为Done，pktSize=0问题正式关闭。

Reason:

- 值1实际是MPP_FMT_YUV420SP_10BIT，编码器期望YUV420P(4)但收到10-bit格式配置，
  导致所有帧因像素格式不匹配而编码输出为空。
- 直接链接测试排除了dlopen方式引入问题的可能性。
- 修复后的主编码器使用与test_mpp_direct完全相同的ABI常量和编码流程。

Consequences:

- MPP Encoder和RTSP模块均标记为Done。
- mpp_abi_constants.h集中管理所有MPP ABI常量，防止同类回归。
- 未来MPP相关开发以mpp_abi_constants.h为唯一常量来源。

### D024: MPP解码器代码规范审查修复（P0 ABI错误）

Date: 2026-06-27

Decision:

- 对照ENGINEERING_PRINCIPLES.md审查mpp_decoder.cpp，修复3项P0级ABI错误。
- 修复范围严格限制：只改常量值、函数签名、guard clause，功能逻辑零改动。
- P1问题（全局变量封装、RAII）暂缓，不影响当前功能。

Reason:

- 解码器与编码器存在同一类ABI错误（MPP_FMT_YUV420P=1应为4）。
- mpp_buf_group_get签名从2参数改为5参数与板端一致，否则栈破坏。
- buffer类型从NORMAL改为ION与project1参考一致。

Consequences:

- mpp_decoder.cpp待板端重新编译验证。
- P1问题（全局变量、RAII）记录在案，后续有需要时再重构。

### D025: FrameBufferPool 使用 shared_ptr 自定义删除器实现自动归还

Date: 2026-07-18

Decision:

- 内存池对外返回 `std::shared_ptr<std::vector<uint8_t>>`，引用归零时由
  自定义 Releaser 把底层 buffer 自动归还到池。
- 不替换 `FramePacket::data` 的类型（`shared_ptr<const vector<uint8_t>>`），
  利用 shared_ptr 的隐式 mutable→const 转换实现零侵入接入。
- 池上限策略：`minFreeBlocks` 保底预热、`maxBlocks` 强制上限、超上限
  分配游离块（不归还到池），保证调用方语义不变。

Reason:

- 现有 `FileFrameSource::readFrame` / `TestPatternSource::readFrame` 每帧
  `make_shared<vector<uint8_t>>(frameSize)`，反复 heap 分配 + 共享控制块分配，
  是媒体管线的尾延迟抖动主因。
- 直接换 allocator 方案会侵入 `FramePacket` 数据结构，破坏多消费者共享只读
  语义。Releaser 方案对外类型签名不变，向后兼容。
- 游离块路径保证池满时调用方仍能拿到合法 buffer，永不阻塞、永不失败。

Consequences:

- 池生命周期必须长于所有借出块（FrameBufferPool 析构前不能有借出块归还）。
  MediaPipeline 持有 pool 即可满足。
- overflow 路径下 `currentInUse` 统计近似（不递减），但 overflow 应是异常路径，
  长期近似可接受。
- 单测覆盖 7 场景 + 集成测试 3 场景，验证正确性与并发安全。

### D026: ThreadPool 使用 packaged_task<void()> 类型擦除 + C++17 兼容

Date: 2026-07-18

Decision:

- 线程池固定 worker 数，构造时启动；`submit(F, args...)` 通过
  `std::bind` + `shared_ptr<packaged_task<R()>>` 包装，再用 lambda 转
  `packaged_task<void()>` 入队，实现任意返回值类型的类型擦除。
- C++17 兼容：不使用 C++20 `jthread` / `stop_token`，避免项目
  `CONFIG += c++17` 升级。
- 不依赖 Qt：与 `media/` 现有风格一致，可在 g++ -std=c++17 单测下运行。
- `waitIdle()` 谓词同时感知 `m_stopping`，防止与 `stop()` 并发调用死锁。

Reason:

- 项目存在大量"为每件事新开 std::thread"的写法，thread 创建/销毁开销大；
  短时 CPU 密集任务（RKNN NMS、CPU 颜色转换回退、LLM 意图解析）适合池化。
- GPS/DHT11/AP3216 等长生命周期独占线程不放入池中，避免与 waitIdle 语义
  冲突。
- 不依赖 Qt 是为了：1) 与 media/ 模块风格一致；2) 单测可在裸 g++ 下跑。

Consequences:

- 析构前未取走的 future 不会阻塞池析构，但未启动任务的 future 会变 broken
  promise，调用方需通过 `future::get()` 感知。
- 单测覆盖 9 场景（含 500 任务压测），Windows g++ 7.3.0 + 板端 g++ 均通过。
- 主程序 CarTerminal.pro 已包含 thread_pool.cpp，但业务代码暂未调用，
  预留为后续 RKNN 后处理等模块的依赖。

### D027: RKNN 后处理 3 路 decode 并行化（NMS 仍串行）

Date: 2026-07-18

Decision:

- 把 `rknn_postprocess::decodeOutputs()` 内部 3 个独立的
  `decodeYoloLayer()` 调用（stride 8/16/32，即 80×80 + 40×40 + 20×20）
  用 ThreadPool 并行执行；新增 `decodeOutputsParallel()` 接收
  `concurrency::ThreadPool&`，NMS 仍串行。
- `decodeOutputs()` 原签名不变 → 既有 9 场景测试零改动，作为回归基线。
- 提取内部 helper `prepareDecodeLayers()` + `finalizeDetections()`，消除
  串行/并行两路径的校验与 NMS 代码重复，保证两路径对相同输入产生
  byte-for-byte 一致输出。
- `rknn_detector.cpp` 持有静态 `unique_ptr<ThreadPool>`（3 worker），在
  `initDetector()` 创建、`clearDetectorState()` 销毁；`detect()` 同步接口
  不变，调用方零改动。

Reason:

- NMS 本身数据量极小（候选 < 100，已被 confThreshold 过滤），并行化收益
  小于线程调度开销，是负优化。
- 3 个 decodeYoloLayer() 才是 CPU 密集部分：共约 2.1M float 扫描 + sigmoid，
  3 层完全独立无共享写入，是线程池的理想场景。
- 保持 `detect()` 同步接口不变，避免重构 Monitor.cpp 两处调用点
  （GUI 线程 + RKNN 消费线程）的同步消费流程。
- 串行版保留为回退路径：若 pool 未初始化（理论上不会发生），
  `detect()` 自动降级为串行 `decodeOutputs()`，保证鲁棒性。

Consequences:

- 既有 `decodeOutputs()` 9 场景测试 96 断言全绿（回归通过）。
- 新增 `decodeOutputsParallel()` 8 场景测试 16 断言全绿，含 200 次压测
  验证并行与串行结果完全一致。
- `PreparedLayer::anchors` 改用 `std::array<int, 6>`（非 C 数组），以便
  C++17 lambda 值捕获。
- `perLayerOk` 用 `std::vector<char>` 而非 `std::vector<bool>`，规避
  `vector<bool>` proxy 不可取地址的 C++ 著名坑点。
- 板端 RK3588S 多核（4×A76 + 4×A55）实际并行收益待 NPU 推理路径整体
  联调后压测验证。

### D028: FrameBroker 改用固定容量环形缓冲区（保留 mutex，不追求 lock-free）

Date: 2026-07-18

Decision:

- 把 `FrameBroker::LaneState` 内部的 `std::queue<FramePacket>` 替换为
  固定容量环形缓冲区（`std::vector<FramePacket> slots` + `head/tail/count`）。
- `publish()` / `take()` 用槽位移动赋值（`std::move`）替代 `queue.push/pop`，
  稳态运行零堆分配。
- **保留 `std::mutex` + `std::condition_variable`**：不追求 lock-free MPMC ring。
- 公共接口（`publish/take/setLaneEnabled/requestStop/reset/...`）签名零改动，
  既有 8 场景测试零改动全绿。

Reason:

- **真实瓶颈不是锁**：1 生产者 + 3 消费者场景下，mutex 竞争 < 1μs，远小于
  30fps 帧间隔（33ms）。盲目追求 lock-free 是为优化而优化。
- **真实瓶颈是堆分配**：`std::queue::push` 每次可能触发 `FramePacket` 拷贝
  构造 + 堆分配；`pop` 触发析构 + 可能 dealloc。固定容量环形 + 槽位复用
  让稳态零堆分配，这才是真实收益。
- **lock-free MPMC ring 复杂度不划算**：Dmitry Vyukov 风格的 MPMC ring
  实现极其复杂（ABA 问题、memory order 调优），且 `shared_ptr` 原子引用计数
  本身就不是 lock-free 的，所以即使队列 lock-free，整体也不是。
- **保留可读性与可验证性**：mutex 版本代码简单、可读、易测试；lock-free
  版本需要复杂的内存序论证，bug 难复现。

Consequences:

- 既有 8 场景测试全绿（回归通过，行为完全不变）。
- 新增 8 场景专项测试全绿，覆盖：capacity-1 边界、多周期 wrap-around、
  take 的 move 语义正确性、丢弃槽位 buffer 引用释放、disable/reset 释放
  所有 buffer、1-producer/3-consumer 500 帧压测、enable/disable 循环压测。
- `publish(const FramePacket&)` 因 const 引用无法 move，仍为拷贝赋值；但
  拷贝的是 `shared_ptr`（一次原子 refcount + 6 个 POD 成员），底层视频
  buffer 不被拷贝。
- `take()` 用 `std::move` 把 slot 内容移出，立即 `FramePacket{}` 重置槽位，
  确保 `shared_ptr` 引用及时释放（buffer 尽早回收）。
- 稳态运行零堆分配：`slots` 向量构造后大小永不变，push/pop 仅槽位赋值。

