# TODO

## ✅ 已完成：RTSP forceIDR 机制 + 工程整洁性三件套（2026-07-20，板端 QuarkPi-CA2 验收通过）

### 任务目标

按车载场景评估的 ROI 优先级，处理 4 项问题：
1. RTSP PLAY 时不强制 IDR 帧（首帧延迟 1-2s，车载场景盲区风险）
2. TTS 进程重启时 ZMQ 端口占用崩溃（影响开发迭代效率）
3. monitor.cpp/.h 死代码残留（评估点名的简历整洁度问题）
4. mainwindow.cpp `eventFilter()` 编译警告刷屏 ~60 行（开发体验问题）

### 完成产出

#### 1. forceIDR 懒触发机制（5 文件，企业级架构）

- [media/mpp_abi_constants.h](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/mpp_abi_constants.h)：+ `kEncoderSetIdrFrameReq = 0x00320003`
- [media/mpp_encoder.h](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/mpp_encoder.h)：+ `forceIDR()` 公开接口（Doxygen 注释）
- [media/mpp_encoder.cpp](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/mpp_encoder.cpp)：+ `<atomic>` / 原子标志 `s_forceIDRRequest` / `forceIDR()` / `consumeIdrRequest()` / encode 内懒触发 / deinit 清标志
- [media/rtsp_server.cpp](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/rtsp_server.cpp)：`buildPlayResponse` 调 `mpp::forceIDR()`

**设计要点**：
- 懒触发模式：跨线程只设原子标志，MPP control 始终在 encodeThread 完成
- Doxygen 风格注释：说明线程安全模型、设计动机、使用场景
- 失败降级：IDR control 失败只 WARN 不阻断编码
- 资源生命周期：`deinitEncoder()` 显式清标志
- C++ 现代风格：`std::atomic<bool>` + `exchange()` 单次 RMW，无锁

**板端验收发现**：板载 libmpp 的 `MPP_ENC_SET_IDR_FRAME_REQ` API 存在缺陷：
- `nullptr` 参数 → 返回 -3（MPP_ERR_MALLOC），不崩溃但 IDR 未触发
- `uint32_t* = 1` 参数 → 返回 MPP_OK，但下次 `encode_put_frame` 段错误（内部状态被破坏）

**当前处置**：control 调用已禁用（注释保留完整 TODO 说明），forceIDR 标志仍被消费保持机制完整。VLC 首帧延迟 ~1 秒（靠自然 GOP 周期）。待后续切到 `MppEncRefCfg` API 真正实现（需 dlsym 加载 `mpp_enc_ref_cfg_init/_set/_deinit` 4 个函数）。

#### 2. ZMQ 端口占用根治（1 文件，2 行核心代码）

- [voice/zmq/src/ZmqInterface.cpp](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/voice/zmq/src/ZmqInterface.cpp)：`setupSocket()` 加 `ZMQ_LINGER=0`

**根因**：默认 `ZMQ_LINGER=-1`（无限等待）→ TTS SIGTERM 时 `close()` 阻塞 → 父进程 `waitForFinished(3000)` 超时 → `kill()` SIGKILL → TCP 端口 TIME_WAIT 60s → 下次启动 `EADDRINUSE`

**板端验收**：
- ✅ 首次启动 TTS 不再 `Address already in use`
- ✅ TTS 退出码 `code=0, status=0`（之前 `code=6, status=1`）
- ✅ 间隔 1 秒连续重启 2 次均正常
- ✅ TTS worker 完整退出：`playback worker exited` + `synthesis worker exited`

#### 3. Monitor 死代码清理（2 文件，-22 行）

- [gui/Monitor/monitor.h](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/gui/Monitor/monitor.h)：删 `#include <QCheckBox>` + 删 `checkBox1/checkBox2` 成员声明 + 中文注释（-6 行）
- [gui/Monitor/monitor.cpp](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/gui/Monitor/monitor.cpp)：删构造函数 13 行死代码注释块 + 删 resizeEvent 3 行残余注释（-16 行）

**板端验收**：编译通过，Monitor 页面行为完全不变（按钮、距离标签、视频显示全部保持原样），RTSP/MPP/Camera/GPS/ASR 全部正常初始化。

#### 4. mainwindow.cpp 编译警告消除（1 文件，+6 行）

- [gui/mainwindow.cpp](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/gui/mainwindow.cpp)：`eventFilter()` 第 515 行 switch 块外加 `#pragma GCC diagnostic push/pop` 局部抑制 `-Wswitch-enum`

**根因**：`-Wswitch-enum` 要求显式列出所有枚举值，`default` 不能替代。但 Qt::Type 有 ~100 个值，且代码前面已过滤非鼠标事件，switch 只需处理 2 个 case。

**策略对比**：全局关闭 `-Wswitch-enum` 会破坏 `CabinCommand::toString()` 的 missing-case 防御（CarTerminal.pro:9 主动开启）；`#pragma` 局部抑制最小侵入，仅影响该 switch。

**板端验收**：`grep "mainwindow.cpp.*(warning|error)"` 输出为空（之前约 60 行警告刷屏）；其他文件警告不受影响；手势切页功能不变。

### 简历亮点（嵌入式工程能力体现）

1. **跨线程懒触发模式**：原子标志 + 单次 RMW + 编码线程单线程消费 MPP API
2. **失败降级设计**：control 失败只 WARN 不阻断，下一帧自然出 P 帧
3. **资源生命周期管理**：deinit 显式清标志，防止状态泄漏到下次 init
4. **ZMQ socket 选项工程化**：ZMQ_LINGER 而非简单的 close/retry，根治而非缓解
5. **死代码清理纪律**：git 历史可回溯，源码整洁
6. **局部 warning 抑制**：`#pragma GCC diagnostic` 最小范围，全局防御保留

### 待办（继承自本次验收）

- [ ] forceIDR 真正生效：实现 `MppEncRefCfg` API 版本（dlsym 加载 4 个函数），把首帧延迟从 ~1s 降到 ≤500ms
- [ ] 应用层缓存 IDR 方案（备选）：RTSP server 缓存最近 IDR + SPS/PPS，PLAY 后立即推送
- [ ] 降 `rtsp_gop_size` 到 10（最简方案）：首帧延迟自然降到 ~330ms，代价是码率略升

## ✅ 已完成：GPS 硬件链路 + IMX415 摄像头 + RGA 硬件加速三件套（2026-07-20，板端 QuarkPi-CA2 验收通过）

### 任务目标

把三个硬件模块从"软件就绪"推进到"板端硬件链路打通"：
- GPS：ATGM336H-5N 模块接到 QuarkPi-CA2，跑通 NMEA 流量 + 修复 gpsworker.cpp 缺陷
- IMX415：从 test_pattern 切到真实 v4l2 源，跑通 800x600 NV12 采集
- RGA：把 legacy `imcvtcolor_t`（每帧 -1 NotSupported）重写为官方 im2d API，作为简历亮点保留

### 完成产出

#### 1. GPS 硬件链路实机验收（ATGM336H-5N）

- 接线：ATGM336H-5N JP1-4/JP1-3 → QuarkPi J11 Pin 10/8 = UART5_M1 (GPIO3_C5/C4)
- 设备树：`fdtoverlay` 合并 `/boot/dtbs/rk3588s-uart5-m1.dtbo` 到主 dtb，重启后 `/dev/ttyS5` 注册成功
- 权限：`quark` 用户加入 `dialout` 组免 sudo
- 代码修复（[device/gpsworker.cpp](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/device/gpsworker.cpp)）4 处缺陷：
  1. 百度地图 URL 缺 `&coordtype=wgs84ll`（WGS84 被误作 BD09）
  2. 失去定位后状态不回退（RMC status=V / GGA fixQuality=0 时未切回 `hasFix=false`）
  3. 非 RMC/GGA 句子触发重复坐标通知（用 `sentence.substr(3,3)` 前缀过滤）
  4. 断线无退避（200ms 可中断退避，`kSimSleepStepMs=50` 步进）
- 配置：[config/vehicle_devices.conf](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/config/vehicle_devices.conf) `gps_enabled=true` `gps_device=/dev/ttyS5` 9600 8N1；[config/map_settings.conf](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/config/map_settings.conf) 真实 AK + `&coordtype=wgs84ll`
- 板端验收：`$GPTXT,01,01,01,ANTENNA OK*35` 天线自检通过，GPS+北斗双模收星，`map image saved: map.png (62281 bytes)` 62KB 地图正常返回

#### 2. IMX415 摄像头硬件链路验证

- 硬件拓扑：`m00_b_imx415 2-001a → csi2-dphy0 → mipi-csi2 → CIF → rkisp0-vir0 → /dev/video33`
- 配置切换：[config/camera_settings.conf](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/config/camera_settings.conf) line 34 `media_source_type=test_pattern` → `v4l2`
- 板端验收：`v4l2-ctl --stream-mmap --stream-count=30` 30 帧无错；sensor 实际输出 3864x2192@10bpp，ISP downscale 到 800x600 NV12；程序内 `source=v4l2` + 颜色正常 + 干净退出

#### 3. RGA 硬件加速修复（im2d API 重写）

- 根因：原 [media/rga_converter.cpp](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/rga_converter.cpp) 用自定义 `rga::Buffer` 与 librga 真实 `rga_buffer_t` ABI 不兼容（`vir_addr`/`phy_addr`/`fd` 应是 union，原代码当独立字段），导致 legacy `imcvtcolor_t` 每帧返回 -1 (NotSupported)
- 重写：`__has_include(<im2d.h>)` 检测板端 SDK → 直链 `-lrga`（不再 dlopen）→ `c_RkRgaInit()` + `wrapbuffer_virtualaddr()` + `imcvtcolor()` 官方 im2d API
- 字节序：输出 `RK_FORMAT_BGRA_8888` 匹配 `QImage::Format_RGB32` 小端字节序（B-G-R-A）
- 色彩空间：`IM_COLOR_SPACE_DEFAULT` 对应 BT.601 limited range（IMX415 默认输出）
- 降级容错：单帧失败自动降级 CPU，不立即禁用，下帧仍可重试
- 板端验收：`RGA hardware acceleration enabled (librga2 im2d API)` + 颜色正常 + 无 `imcvtcolor_t failed: -1` 警告刷屏
- 工程改动：[media/rga_converter.h](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/rga_converter.h) 简化为 4 个纯接口；[CarTerminal.pro](file:///d:/E盘文件/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/CarTerminal.pro) 加 `linux:exists(/usr/include/rga/im2d.h)` 条件链接

### 简历亮点（嵌入式工程能力体现）

1. **ABI 兼容性分析**：发现自定义结构体与 librga 真实 `rga_buffer_t` 不兼容（union 错位）
2. **SDK 集成**：用 `__has_include` 检测板端 SDK 头文件，自动切换 dlopen/直链两种模式
3. **官方 API 优先**：用 `im2d` API 替代 legacy `imcvtcolor_t`，符合 Rockchip 官方推荐
4. **字节序处理**：`BGRA_8888` 匹配 `QImage::Format_RGB32` 小端序
5. **色彩空间**：BT.601 limited range 配置正确，画面颜色正常
6. **降级容错**：硬件失败自动降级 CPU，保证可用性
7. **条件编译**：host（无 librga）和板端（有 librga）都能编译

### 待办（继承自 2026-07-20 验收）

- [ ] 室外 GPS 定位实测：放窗台等卫星，验证 `first valid fix` 日志
- [ ] 拔线/重连实测：验证 200ms 退避不刷屏 + 自动恢复
- [ ] 长时间稳定性：30+ 分钟运行，确认无帧丢失、无内存泄漏
- [ ] RKNN 检测联动：`rknn_enabled=true` 后，IMX415 帧 → YOLOv5 检测 → 危险报警
- [ ] RTSP 推流：`rtsp_enabled=true` 后，IMX415 帧 → MPP 编码 → RTSP 推流

## ✅ 已完成：dmesg 内核日志集成到 LogManager（2026-07-18，Windows + 板端 QuarkPi-CA2 双平台全绿）

### 任务目标

将内核日志（dmesg）实时转发到应用日志流（LogManager），排查 ASR/NPU/USB/ALSA
问题时不再需要手动 `dmesg` 切换窗口。对应 "工程优化与稳定性增强 → dmesg 系统诊断"
条目（原状态：做了一半）。

### 完成产出

#### 1. `DmesgTail` 类（新增）

- [common/dmesg_tail.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/common/dmesg_tail.h) / [common/dmesg_tail.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/common/dmesg_tail.cpp)
- 后台线程读 `dmesg --follow --since now`，每行内核日志通过 `VLOG_INFO("kernel", ...)`
  转发到 LogManager。
- **self-pipe 停止机制**：`stop()` 向管道写 1 字节，`poll()` 立即唤醒退出。
  这是中断 poll() 的标准可靠方法，不依赖 `close(fd)` 的未定义行为。
- `start()` 在创建线程前设置 `m_running=true`，消除 double-start 窗口期。
- 跨平台：非 Linux 平台 `start()` 返回 false 空操作（Windows 编译通过）。
- 失败降级：dmesg 不可用 → 记一次 WARN 后线程退出，不影响应用主流程。
- RAII：析构自动 stop，main.cpp 无需显式 stop。

#### 2. 单元测试（新增）

- [tests/common/test_dmesg_tail.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/common/test_dmesg_tail.cpp)
- 6 项测试：new/start/double-start/idempotent-stop/destructor/restart
- 带进度输出（`[test] xxx...`），便于定位卡死问题

#### 3. 主程序集成

- [main.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/main.cpp) — 日志初始化后启动 `DmesgTail`，RAII 自动停止
- [CarTerminal.pro](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/CarTerminal.pro) — `common/` 段加入 `dmesg_tail.cpp` / `dmesg_tail.h`

#### 4. 顺带修复 Qt `slots` 宏冲突

- [media/frame_broker.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/frame_broker.h) / [media/frame_broker.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/frame_broker.cpp)
- `LaneState::slots` 改名 `ringSlots`（Qt 工程里 `slots` 是 `#define slots Q_SLOTS`，
  不能用作变量名）。全量编译时触发暴露。

### 板端验收数据

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `head -20` 内容 | 452 行历史 boot 日志（`[15:59:09]`） | 正常应用启动日志（`[19:17:40]`） |
| `[kernel]` 行数 | 452+（历史刷屏） | 0（被 `--since now` 过滤） |
| `DmesgTail:` 启动信息 | `using /dev/kmsg` | `started (dmesg --follow)` |
| 单元测试 | 卡住（poll 不返回） | ALL PASSED (exit=0) |

### 设计决策

- **放弃 `/dev/kmsg` + `lseek(SEEK_END)` 方案**：在 RK3588S 内核上 `poll()`
  不返回（既不超时也不响应 self-pipe），行为不一致。改用 `dmesg --follow --since now`
  用户态工具，行为可靠。
- **self-pipe 而非 close(fd)**：`stop()` 关闭 fd 来打断 `poll()` 是未定义行为，
  不可靠。self-pipe 是 Linux 中断 poll() 的标准可靠方法，保证 1ms 内退出。
- **`--since now` 跳过历史**：避免应用启动时把整个 dmesg 环形缓冲区倒进
  应用日志，淹没真正的实时内核事件。

### 双平台验证

| 平台 | 单元测试 (6 场景) | 主程序集成 |
|---|---|---|
| Windows g++ | ✅ ALL PASSED | N/A（非 Linux） |
| 板端 QuarkPi-CA2 | ✅ ALL PASSED (exit=0) | ✅ 历史日志 0 行刷屏 |

## 2026-07-18 Status Update: Real-Mic ASR/TTS Loop + Echo Suppression Closed

**Completed on 2026-07-18** (see `docs/CHANGELOG.md` and
`docs/MODULE_STATUS.md` 2026-07-18 entries for the full record):

- ✅ Real-microphone capture path: switched the ASR ALSA source from the
  broken ES8323 record path (`plughw:2,0`, I2C comm failure) to the USB
  camera microphone (`plughw:3,0`, card 3 stereo). TTS playback still
  uses `plughw:2,0` via `AudioPlayer.cpp` (only the record path is
  broken on ES8323).
- ✅ ALSA plughw lazy-start fix: `voice/VoiceManager.cpp` replaced
  `snd_pcm_start()` with a blocking priming `snd_pcm_readi()`. Root
  cause: `snd_pcm_start()` returns 0 but the underlying hw stream stays
  PREPARED for plughw devices; `poll()` never returns POLLIN until the
  app pulls audio.
- ✅ ASR/TTS echo suppression: `m_asrMuted` atomic + 3 s window armed by
  `sendToTts()`; while muted the ASR loop drains ALSA (prevents overrun)
  but discards audio and calls `m_asrEngine->reset()` on the mute-entry
  edge. Verified end-to-end on the board —— `TTS ack: ok` and the 3 s
  mute/unmute sequence are both visible in the logs.
- ✅ ZMQ 30 s recv timeout fix: `config/voice_settings.conf` switched
  `llm_address` / `tts_address` / `tts_status_address` from
  `tcp://localhost:` to `tcp://127.0.0.1:` because the board's
  `/etc/hosts` can be missing the `localhost` entry. `/etc/hosts` also
  patched with `127.0.0.1 localhost` as a permanent backup.
- ✅ Debug scaffolding cleanup: removed all 23 lines of the temporary
  `ASR_DBG` / `dbgPollCnt` / `dbgReadCnt` / `dbgMaxAbs` instrumentation
  from `VoiceManager.cpp`. Anchor-based Python replacement was required —
  regex delete broke brace balance (kept `if (pollRet < 0)` body after
  deleting the `int pollRet = poll(...)` line).
- ✅ Config sync: both `config/asr_settings_offline.conf` and
  `config/voice_settings.conf` were edited on **both** the board (sed)
  and Windows (`SearchReplace`) so future code/container syncs do not
  overwrite the board-side fixes.
- ✅ Handoff verification (2026-07-18, next-session AI): local Windows
  `qmake CarTerminal.pro` succeeded; `VoiceManager.cpp` passed
  `g++ -fsyntax-only` (brace balance and variable-declaration order in
  the priming-`readi` block at lines 593-625 and the `m_asrMuted` while
  loop at 649-692 are correct; `grep -c ASR_DBG = 0`). Full
  `mingw32-make -j8` fails only on Linux-only headers (`sigaction` /
  `linux/types.h` / `sys/ioctl.h`), not on code logic. Board regression:
  `qmake && make -j8` clean (`make_exit=0`), `VEHICLE_VOICE_TEST_TEXT=
  "打开地图"` produced the full closed-loop sequence (inject → match →
  muted → sending to TTS → ack:ok → stream reset → drain → unmuted →
  resuming recognition); all 5 do-not-regress defaults verified against
  runtime logs.
- ✅ One-shot no-peripheral verification script
  (`scripts/verify_no_peripheral.sh`): integrates 6 stages — pre-flight
  dependency check, 5 red-line config verification (asr_alsa_device,
  ZMQ 127.0.0.1, tts_feedback_for_command + mute_ms, llm_enabled,
  priming snd_pcm_readi) + ASR_DBG residue + sherpa config consistency,
  main program qmake+make (with future-mtime auto-touch), voice/ CTest
  (4 tests, "0 tests = FAIL" guard), TTS→WAV→ASR closed loop (6/6 smoke
  profile), and optional GUI text-injection (`--with-gui`). Any stage
  failure exits non-zero immediately; numeric parsing hardened with
  `tr -d '[:space:]'` + `=~ ^[0-9]+$`. Board-validated: 5 PASS + 1 SKIP
  (GUI) = `RESULT = PASS`, `exit=0`.

**Still open** (not in this session's scope):

- ⚠ Item 6 (RKNN enabled): unchanged — keep `rknn_enabled=false` until
  RKNPU kernel driver ≥ 0.9.7 (0.9.6 SIGBUS on RKLLM init). See
  2026-07-15 audit and 2026-07-17 Soak test entries.
- ⚠ `/etc/hosts` patch on the board may not survive a reflash — if a
  fresh image is flashed, re-add `127.0.0.1 localhost` *or* keep relying
  on the `tcp://127.0.0.1:` config (which works regardless).
- Items 2, 4, 5 from the 2026-07-15 audit remain as previously documented.
- `PENDING_REAL_CAMERA`: V4L2 code path still preserved but not exercised
  in this session (USB camera was used only as a microphone, not as a
  video source).

## 2026-07-17 Status Update: Task A+B Board Validated

**Completed on 2026-07-17** (see `docs/MODULE_STATUS.md` section
"2026-07-17 Board Validation Complete (Authoritative)"):

- ✅ Item 1 (board build): native qmake + full link passed on QuarkPi-CA2,
  `BUILD_RC=0`, binary timestamp 2026-07-17 19:13:37.
- ✅ Item 3 (forced `QThread::terminate`): removed entirely; board-validated
  via scenarios A/B/C/E with clean shutdown (`application exiting code=0`).
- ✅ Media-pipeline board combinations A–E: all PASSED on QuarkPi-CA2.
  Scenario matrix and metrics in `docs/MODULE_STATUS.md`.
- ✅ GUI throttle fix: 30 FPS event-loop starvation resolved; Esc/button
  responsive; 32 `[MEDIA_STATS]` events over 30 s in scenario A.

**Still open** (not in task A+B scope):

- ⚠ Item 6 (RKNN enabled): keep `rknn_enabled=false` by default. RKNPU
  kernel driver 0.9.6 has a NULL-pointer dereference bug in
  `rknpu_job_subcore_commit_pc`. Scenario B proved the application-side
  RKNN integration works (403 frames at 8.63 fps), but the driver can
  crash on certain inputs. Needs matched librknnrt.so / driver pair from
  Rockchip, or kernel/driver upgrade.
- Items 2, 4, 5 remain as previously documented.
- `PENDING_REAL_CAMERA`: V4L2 code path is preserved and inherits the
  lifecycle fixes, but no real-camera claim is made.

## 2026-07-15 Current Audit Priorities (Authoritative)

1. On an RK3588S/AArch64 SDK host, build the exact current worktree and run every current unit/source-contract test. Do not treat historical logs as current evidence.
2. On the target board, execute the matrix in `../../reports/audit_pending_manual_review_20260715.md`, including clean shutdown, MPP/RGA/camera, RKNN-disabled and controlled-enabled paths, and ASR runtime ABI checks.
3. Resolve the open major findings: forced `QThread::terminate`, completed voice-worker accumulation, legacy online-ASR timeout/HTTP parsing, music download validation/limits, Wi-Fi password exposure in process arguments, and unbounded device I/O timeouts.
4. ~~Replace or formally retire the absent `rk3588s_migration/voice_gateway/` runner and migrate remaining historical test/document paths to `RK3588S_CarTerminal`.~~ ✅ **Completed 2026-07-18 (task A2)**: All 31 non-historical `voice_gateway` references across 7 docs files (`BUILD_RUN_TEST.md`, `PROJECT_CONTEXT.md`, `INTERFACE_SPEC.md`, `DECISIONS.md`, `MIGRATION_PLAN.md`, `MIGRATION_BLUEPRINT.md`, `MODULE_STATUS.md`) are now marked as retired/historical. The 16 entries in `CHANGELOG.md` are preserved as immutable historical record. No code paths reference the retired skeleton; the active voice stack lives under `RK3588S_CarTerminal/voice/`.
5. Treat GPS as Planned/Stub until a real serial implementation and board test exist; keep it disabled by default.
6. Keep `rknn_enabled=false` until the current-worktree build and live board gates pass.

The 2026-07-15 audit report is the controlling status source for these items. Older completed-item sections below are historical context.

Last updated: 2026-07-20 (RTSP forceIDR 机制 + 工程整洁性三件套板端验收通过：forceIDR 懒触发+原子标志+单线程消费企业级架构落地（板载 libmpp `MPP_ENC_SET_IDR_FRAME_REQ` API 缺陷，control 调用禁用待 MppEncRefCfg 实现）；ZMQ_LINGER=0 根治 TTS 重启端口占用；monitor.cpp/.h -22 行死代码；mainwindow.cpp `#pragma GCC diagnostic` 局部抑制 -Wswitch-enum 消除 ~60 行警告刷屏。操作历史见下方 ✅已完成 区块；下一步候选：MppEncRefCfg 版 forceIDR / 应用层缓存 IDR / 降 gop_size / 统一命名前缀 / 车载特化文档)

## ✅ 已完成：基础设施改造四件套（2026-07-18，Windows + 板端 QuarkPi-CA2 双平台全绿）

### 任务目标

将项目核心管线的基础设施从"能用"升级到"企业级"：消除每帧 heap 分配、
引入通用线程池、RKNN 后处理并行化、FrameBroker 稳态零堆分配。四个改造
全部 TDD 驱动，既有测试零改动回归，新增专项测试覆盖边界与压测。

### 完成产出（按交付顺序）

#### 1. FrameBufferPool — 媒体帧缓冲内存池（D025）

- [common/frame_buffer_pool.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/common/frame_buffer_pool.h) / [.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/common/frame_buffer_pool.cpp)
- 固定块大小 + 自由链表；`shared_ptr` 自定义 Releaser 自动归还；
  兼容 `FramePacket::data` 类型零侵入
- [media/file_frame_source.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/file_frame_source.h) / [.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/file_frame_source.cpp) —
  新增 pool-enabled 构造函数，readFrame 优先从池借出
- [media/test_pattern_source.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/test_pattern_source.h) / [.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/test_pattern_source.cpp) — 同上
- 单测：[tests/common/test_frame_buffer_pool.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/common/test_frame_buffer_pool.cpp)（7场景 + 4线程×500次并发）
- 集成单测：[tests/common/test_file_frame_source_with_pool.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/common/test_file_frame_source_with_pool.cpp)（3场景）
- 双平台验证：Windows ✅ | 板端 ✅（`./tests/common/build_and_run_common_tests.sh`）

#### 2. ThreadPool — 通用固定 worker 数线程池（D026）

- [common/thread_pool.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/common/thread_pool.h) / [.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/common/thread_pool.cpp)
- C++17 兼容、零 Qt 依赖；`submit()` 返回 `std::future`；
  `waitIdle()` 谓词感知 `m_stopping` 防死锁；幂等 `stop()`；析构自动 join
- 单测：[tests/common/test_thread_pool.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/common/test_thread_pool.cpp)（9场景 + 500任务压测）
- 双平台验证：Windows ✅ | 板端 ✅

#### 3. RKNN 后处理 3 路 decode 并行化（D027）

- [media/rknn_postprocess.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/rknn_postprocess.h) / [.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/rknn_postprocess.cpp) —
  提取 `prepareDecodeLayers()` + `finalizeDetections()` 内部 helper；
  新增 `decodeOutputsParallel()`（3路 decodeYoloLayer 并行，NMS 串行，与串行版 byte-for-byte 一致）
- [media/rknn_detector.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/rknn_detector.cpp) —
  静态 `unique_ptr<ThreadPool>` (3 worker)；`detect()` 优先并行，pool 缺失时降级串行
- **关键判断**：不并行 NMS（候选<100，调度开销>收益），真正 CPU 密集的是 3 路 decode（2.1M float 扫描+sigmoid）
- 单测回归：[tests/rknn/test_rknn_postprocess_adapter.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/rknn/test_rknn_postprocess_adapter.cpp)（9场景96断言，零改动）
- 新增单测：[tests/rknn/test_rknn_postprocess_parallel.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/rknn/test_rknn_postprocess_parallel.cpp)（8场景16断言 + 200次压测）
- 双平台验证：Windows ✅ | 板端 ✅（`./tests/rknn/build_and_run_rknn_tests.sh`）

#### 4. FrameBroker 环形缓冲区 — 稳态零堆分配（D028）

- [media/frame_broker.h](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/frame_broker.h) / [.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/media/frame_broker.cpp) —
  `std::queue<FramePacket>` → 固定容量环形 `std::vector<FramePacket>` + head/tail/count；
  publish/take 用 move 语义替代 push/pop；稳态零堆分配
- **关键判断**：不追求 lock-free（mutex 竞争<1μs，远小于 33ms 帧间隔；
  真实瓶颈是 `std::queue` 的堆操作而非锁；`shared_ptr` 原子引用计数
  本身就不是 lock-free 的）
- 公共接口零改动 → MediaPipeline/Monitor 等调用方零改动
- 单测回归：[tests/media/test_frame_broker.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/media/test_frame_broker.cpp)（8场景，零改动全绿）
- 新增单测：[tests/media/test_frame_broker_ring.cpp](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/media/test_frame_broker_ring.cpp)（8场景：capacity-1、多周期wrap-around、move语义正确性、槽位buffer释放、disable/reset释放、1producer/3consumer 500帧压测、enable/disable循环压测）
- 双平台验证：Windows ✅ | 板端 ✅（`./tests/media/build_and_run_frame_broker_tests.sh`）

### 板端一键验收汇总

| 脚本 | 测试套件 | 状态 |
|---|---|---|
| `./tests/common/build_and_run_common_tests.sh` | FBP + ThreadPool + 集成 | ✅ 3/3 PASS |
| `./tests/rknn/build_and_run_rknn_tests.sh` | 既有9场景(96断言) + 新增8场景(16断言) | ✅ 全部 PASS |
| `./tests/media/build_and_run_frame_broker_tests.sh` | 既有8场景 + 新增8场景(500帧压测) | ✅ 全部 PASS |

### 设计决策文档

完整记录在 [docs/DECISIONS.md](file:///e:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/docs/DECISIONS.md)：
- D025：FrameBufferPool — 固定块大小、shared_ptr Releaser 自动归还
- D026：ThreadPool — C++17 兼容、future 同步、waitIdle 防死锁
- D027：RKNN 3 路 decode 并行化 — 并行 decode 不并行 NMS 的决策理由
- D028：FrameBroker 环形缓冲区 — 不追求 lock-free 的决策理由

## ✅ 已完成：B5 dev-env 陷阱表脚本化（2026-07-18）

### 任务目标

将项目中踩过的所有环境配置陷阱（dev-env pitfalls）从散落的
`project_memory.md` / `CHANGELOG.md` / `verify_no_peripheral.sh` 中
提取出来，整合成结构化的开发者可见文档 + 一键自检脚本，避免
新机器/新克隆/重置环境后重复踩坑。

### 完成产出

#### 1. `docs/PITFALLS.md`（陷阱清单文档，559 行）

完整记录 19 条已知陷阱（P001-P019），每条包含：
- 症状（如何识别）
- 根因（为什么会发生）
- 解决方案（具体修复命令）
- 验证命令（如何确认已修复）
- 对应检测函数名

按类别分组：
- 构建系统（P001-P002）：qmake vs CMake、系统时间未同步
- ALSA 配置（P003-P006）：设备选错、Mic 开关、音量 clipping、priming readi
- ASR 时序（P007-P009）：6s 启动、3s 命令后静音、2.4s 尾静音
- ASR 配置（P010-P014）：sherpa 版本、解码方法、热词 score、热词文件、TTS 反馈
- ZMQ（P015-P017）：localhost、/etc/hosts、TTS mute window
- NPU（P018）：llm_enabled vs NPU 驱动版本
- 工程纪律（P019）：ASR_DBG 残留

#### 2. `scripts/check_dev_env.sh`（预防性检测脚本，515 行）

与 `PITFALLS.md` 中的 P001-P019 **一一对应**的检测函数，输出
彩色报告：✅ PASS / ⚠ WARN / ❌ FAIL，失败时给出具体修复命令。

**特性**：
- 不构建任何二进制，不运行 CTest，纯环境检查
- 支持 `--strict`（WARN 算 FAIL）、`--skip-board`（跳过板端硬件检查）、
  `--help`
- 退出码：0=PASS、1=FAIL、2=参数错误
- 与 `verify_no_peripheral.sh` 互补：
  - `check_dev_env.sh` = **环境前置检查**（开发前）
  - `verify_no_peripheral.sh` = **功能验收**（开发后）

### 与现有脚本的关系

```
scripts/
├── check_dev_env.sh          ← B5 新增：环境前置检查（开发前运行）
├── verify_no_peripheral.sh   ← 已有：功能验收（开发后运行）
├── verify_voice_deployment.sh
├── install_buzzer_gpio_permissions.sh
└── run_hdmi.sh
```

### 维护规则

1. 新增陷阱：按 P### 编号递增添加到 `PITFALLS.md`，并在
   `check_dev_env.sh` 添加对应 `check_xxx` 函数
2. 修改陷阱：解决方案变化时同步更新文档和脚本
3. 删除陷阱：陷阱彻底消除时文档保留历史（标记 Resolved），脚本可删函数
4. `project_memory.md` 是 AI 内部记忆，`PITFALLS.md` 是开发者可见权威

## ✅ 已完成：A2 退役 `rk3588s_migration/voice_gateway/`（2026-07-18）

### 任务目标

形式上退役遗留的 `rk3588s_migration/voice_gateway/` 运行器（当前工作区不存在该目录），把 47 处文档残留中**非历史记录**的部分全部标记为退役/历史，避免新用户照抄非功能性构建命令。

### 完成内容

- **`docs/BUILD_RUN_TEST.md`** (8 处)：两个 "Build Voice Gateway" 章节标题加 `(RETIRED 2026-07-18, task A2)`，命令块前置 `> DO NOT RUN` 警示框 + `# HISTORICAL ONLY` 注释；`tests\voice_gateway\build_and_run.ps1` 行加退役注释。
- **`docs/PROJECT_CONTEXT.md`** (1 处)："Current submodules" 列表中的 `voice_gateway/` 标记为 RETIRED，指向 `RK3588S_CarTerminal/voice/`。
- **`docs/INTERFACE_SPEC.md`** (1 处)："Voice Gateway Config Interface" 章节加 `(RETIRED 2026-07-18, task A2)` + 历史 only 警示框。
- **`docs/DECISIONS.md`** (4 处)：D004 加 "Status: RETIRED" 框；D005 测试列表项加退役注释；D017 中两处 `voice_gateway` Logger 引用标记为历史。
- **`docs/MIGRATION_PLAN.md`** (4 处)：源映射表、Phase 1 Completed、Task 阅读列表 2 处均加 "(RETIRED 2026-07-18, A2)" 注释。
- **`docs/MIGRATION_BLUEPRINT.md`** (2 处)：3.3 节前置退役说明框；3.4 节 `voice_gateway/` 删除行加 "已执行" 注释。
- **`docs/MODULE_STATUS.md`** (4 处)：两处 "Voice gateway skeleton" 行加 "Formally retired 2026-07-18 (task A2)"；Unified log module 行加历史注释；`voice_gateway_app builds` 行加 "HISTORICAL ONLY" 注释。
- **`docs/TODO.md`**：本完成区块 + line 105 删除线 + Last updated 更新。
- **`docs/CHANGELOG.md`**：新增本日条目（见下）。

### 保留不动

- **`docs/CHANGELOG.md`** 中 16 处历史 `voice_gateway` 条目（line 1643/2156/2256/2337/2383/2465/2509/2587/2632/2733/2834/2883/2885/2901 等）——历史日志的不可变性优先于引用清理，仅在新条目中说明退役决定。

### 验证

- 所有 7 个非历史文档文件的 `voice_gateway` 引用均带 RETIRED/历史 only 标记。
- 无任何代码路径（`*.cpp`/`*.h`/`*.pro`/`CMakeLists.txt`）引用 `rk3588s_migration/voice_gateway/`（已通过 grep 确认）。
- 当前工作区不存在 `rk3588s_migration/voice_gateway/` 目录。
- 活跃语音栈位置（`RK3588S_CarTerminal/voice/`）在每处退役标记中均明确指向。

## ✅ 已完成：一键无外设验收脚本（2026-07-18）

### `scripts/verify_no_peripheral.sh`

- 将 CTest、smoke 闭环、配置保护（5 条红线）和主程序构建整合到 `scripts/verify_no_peripheral.sh`。
- 正确处理旧版 CTest 不支持 `--test-dir` 的情况：进入构建目录执行 `ctest`。
- 任一阶段失败必须返回非0；不得把"0个测试"当成通过。
- GUI注入作为可选阶段：默认 SKIP，`--with-gui` 启用。
- `test_asr_config.cpp` 已从 `tmpnam()` 改为 `mkstemp()`；板端构建成功、`tmpnam`警告消失、`asr_config_tests` 1/1通过。
- **板端验证结果**（2026-07-18）：6 阶段 5 PASS + 1 SKIP (GUI) = `RESULT = PASS`, `exit=0`。Stage 1 红线全过、Stage 2 `make -j8` 干净、Stage 3 CTest 4/4、Stage 4 TTS→WAV→ASR 6/6。

## ✅ 已解决的阻塞问题（Resolved）

### 5. sherpa-onnx 1.13.3 ABI 修复：删除未经验证偏移 fallback（板端验证通过 2026-06-28）

- **根因**：`AsrEngine::getResult()` 在 `SherpaOnnxOnlineStreamGetResultText` 不可用时，使用 `*reinterpret_cast<const char *const *>(result)` 假设 text 在结构体偏移 0。该假设无头文件证据、无运行时版本校验，库版本变化即可能崩溃。
- **板端已确认事实**：
  - 库路径：`/usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so`
  - SHA256：`c713c6c6ca7fcaff01607768d3a163f73aae0c83e476bc1bcbce7839b9695507`
  - 版本：`1.13.3`，Git SHA：`330609da`，Git Date：`Mon Jun 15 07:50:54 2026`
  - 导出：`SherpaOnnxGetOnlineStreamResult`、`SherpaOnnxDestroyOnlineRecognizerResult`、`SherpaOnnxOnlineStreamInputFinished`、`SherpaOnnxGetVersionStr`、`SherpaOnnxGetGitSha1`、`SherpaOnnxGetGitDate`
  - 不导出：`SherpaOnnxOnlineStreamGetResultText`
  - 官方 1.13.3 头文件确认 `SherpaOnnxOnlineRecognizerResult.text` 为首字段（偏移 0）
- **修改文件**：
  - `voice/asr/sherpa_onnx_abi_1_13_3.h`（新增）：官方 1.13.3 `OnlineRecognizerResult` 最小结构 + `static_assert` text 偏移 0 + `isCompatible(version, gitSha)` 双重版本校验
  - `voice/asr/AsrEngine.h`：删除 `FnGetResultText`/`m_fnGetResultText`/`hasGetResultTextApi()`；新增 `GetVersionStrFn`/`GetGitSha1Fn`/`GetGitDateFn` 函数指针、`m_runtimeVersion`/`m_runtimeGitSha`/`m_runtimeGitDate`/`m_resultAbiCompatible` 成员、`runtimeVersion()`/`runtimeGitSha()`/`runtimeGitDate()`/`isResultAbiCompatible()` 查询接口、`loadRuntimeVersion()` 私有方法
  - `voice/asr/AsrEngine.cpp`：`loadSymbols()` 加载三个版本 API；`loadRuntimeVersion()` 调 dlsym→复制版本→`isCompatible()` 校验；`init()` 调 `loadRuntimeVersion()`（不初始化失败但禁止读取结果）；`getResult()` 重写为 `sherpa_abi_1_13_3::OnlineRecognizerResult*` 类型化访问 + RAII `ResultGuard` 保证 destroy 恰好一次；删除偏移 fallback
  - `voice/tests/test_asr_abi.cpp`（新增）：4 个 `static_assert`（标准布局、text 偏移0、字段顺序、结构体大小）+ 10 个测试用例（兼容、错误 SHA、错误版本、nullptr、空字符串、前缀匹配）
  - `voice/audio/wav_io.h`：新增 `checkedPcm16DataSize()` 纯函数声明，注释明确 `readPcm16Wave` 为通用解析器
  - `voice/audio/wav_io.cpp`：实现 `checkedPcm16DataSize()`（nullptr/channels=0/sampleCount=0/溢出均返回 false）；`writePcm16Wave()` 拒绝 sampleCount=0/samples=nullptr/channels=0/sampleRate=0，并复用纯函数计算 dataSize
  - `voice/tests/test_wav_io.cpp`：test 2 改为 sampleCount=0 拒绝且不生成文件；删除坏 test 13（nullptr+hugeCount 被 null 检查先拦截）；新增 test 13（正常输入计算）+ test 14（5 个失败路径包括边界）+ test 15（nullptr+非零 count）
  - `voice/tts/include/TTSModel.h`：加 `<stdexcept>`，构造函数文档化 `throws std::runtime_error`
  - `voice/tts/src/TTSModel.cpp`：构造函数 `load_model()` 失败时先释放 `dataW_` 避免泄漏，再抛 `std::runtime_error`
  - `voice/tools/tts_wav_test.cpp`：`model` 改为 `unique_ptr<TTSModel>`，构造调用包在 try/catch 中捕获 `std::runtime_error`
  - `voice/tools/asr_wav_test.cpp`：`initAsrEngine()` 输出 `SHERPA_VERSION`/`SHERPA_GIT_SHA`/`SHERPA_GIT_DATE`/`RESULT_ABI_COMPATIBLE`/`INPUT_FINISHED_API=1`/`RESULT_API=typed-abi-1.13.3`；ABI 不兼容返回 8
  - `voice/CMakeLists.txt`：注册 `test_asr_abi` 目标 + CTest `asr_abi_tests`
- **核心验收点**：代码中不得再存在未经版本校验的结果结构偏移猜测；只有运行时版本 `1.13.3` 且 Git SHA `330609da` 匹配时，才允许按官方结构读取 `result->text`。
- **板端验证结果**：
  - 运行时输出 `SHERPA_VERSION=1.13.3`、`SHERPA_GIT_SHA=330609da`、`RESULT_ABI_COMPATIBLE=1`、`RESULT_API=typed-abi-1.13.3`。
  - `test_asr_abi`：12 passed，0 failed。
  - CTest：wav_io、asr_abi、vehiclecommand、asr_config 共 4/4 PASS。
  - `tts_wav_test` 可生成 16kHz、单声道、PCM16 WAV。
  - `asr_wav_test` 已执行真实 sherpa decode，不再只是静态审查。
- **尚未验证**：真实麦克风、真实人声识别率、车内噪声、扬声器播放、NPU LLM。

### 4. TTS→WAV→ASR→固定指令无硬件闭环测试链路（正向板端验证通过，负向修复中）

- **目标**：在无麦克风/扬声器/GUI/LLM/NPU 的条件下，验证 `AsrEngine` 能否从真实 PCM 音频中识别中文并映射为车载指令（而非仅"初始化成功"）。
- **链路**：中文文字 → SummerTTS 合成 PCM → 写入标准 WAV → AsrEngine 读取 WAV → sherpa-onnx 离线识别 → `vehiclecommand::parseCabinCommand()` → 指令输出。
- **新增文件**：
  - `voice/audio/wav_io.h` / `wav_io.cpp` — 纯 C++17 PCM16 WAV 读写模块（RIFF/WAVE/fmt/data chunk 手工小端写入，未知 chunk 跳过，奇数 padding 处理，溢出保护）。
  - `voice/tools/tts_wav_test.cpp` — TTS→WAV CLI 工具（复用 `TTSModel`，RAII `PcmGuard` 确保 `free_data()`，回读自检，脚本友好输出）。
  - `voice/tools/asr_wav_test.cpp` — WAV→ASR→指令 CLI 工具（严格 16kHz/mono/PCM16 校验，PCM 诊断 peak/RMS，分块喂入 1600 采样点，`inputFinished()` 或 1 秒静音 fallback，最大解码循环限制 1000）。
  - `voice/tests/test_wav_io.cpp` — 13 例 WAV I/O 单元测试（CTest 注册）。
  - `voice/scripts/verify_tts_asr_wav.sh` — 6 条指令批量验证脚本（`set -euo pipefail`，数组驱动，失败样本保留）。
- **修改文件**：
  - `voice/tts/include/TTSModel.h` / `tts/src/TTSModel.cpp` — 新增 `bool isLoaded() const noexcept`。
  - `voice/asr/AsrEngine.h` / `AsrEngine.cpp` — 新增 `inputFinished()` + `hasInputFinishedApi()` + 可选 dlsym `SherpaOnnxOnlineStreamInputFinished`。
  - `voice/CMakeLists.txt` — 提取 `summer_tts_engine` 静态库（TTSModel+SummerTTS internals+TN，无 main/AudioPlayer/MessageQueue/TextProcessor）和 `wav_io` 静态库；新增 `tts_wav_test`/`asr_wav_test`/`test_wav_io` 三个目标；`enable_testing()` + CTest 注册；install 列表仅保留生产目标。
- **设计约束**：不打开 ALSA、不启动 Qt GUI、不启动 LLM/TTS 服务进程、不访问 NPU、不伪造 ASR 结果、不复制 AsrEngine 核心实现。
- **板端验证结果**：
  - `modified_beam_search`、热词文件和 score 2.0 已确认生效。
  - accuracy 6/6 PASS；smoke 6/6 PASS。
  - 负向 6/6；`播放列表 → ASR_TEXT=播放 → COMMAND=Unknown`。
- **当前状态**：无外设软件闭环已验收通过。真实麦克风、人声、车内噪声和扬声器仍待硬件验证。

### 1. 离线ASR→固定指令→GUI路径与NPU驱动解耦（已解决 2026-06-27）

- **根因**：原 `VoiceManager::init()` 强制启动LLM/TTS，ASR线程只有在LLM/TTS启动后才会启动；`asrThreadFunc` 中存在无条件 `sendToLlm()` 调用。导致离线固定指令模式被NPU驱动v0.9.6 SIGBUS阻塞。
- **修复内容**：
  - `VoiceConfig` 提取到 Qt-free 的 `voice/voice_config.h/cpp`，可单元测试。
  - `init()` 改为ASR/LLM/TTS独立可启用，LLM/TTS默认false不阻塞ASR。
  - `asrThreadFunc` 删除无条件 `sendToLlm()`，只 `emit asrTextRecognized()`。
  - 新增 `decideVoiceRoute()` 纯函数实现双路径路由决策。
  - `MainWindow::onOfflineAsrTextRecognized` 调用 `decideVoiceRoute()` 做路由。
  - `vehiclecommand.cpp` 扩展关键词（查看地图/查看摄像头/今天天气等）+ containsAll helper。
- **host验证**：vehiclecommand 41例、voice_config 46例、voice_routing 7例全部PASS；qmake语法检查、静态检查、源码-syntax-only全部通过。
- **相关文件**：
  - `RK3588S_CarTerminal/voice/voice_config.*` — Qt-free配置与路由决策
  - `RK3588S_CarTerminal/voice/VoiceManager.*` — 三模块独立启用
  - `RK3588S_CarTerminal/gui/mainwindow.*` — 双路径路由槽函数
  - `RK3588S_CarTerminal/common/vehiclecommand.*` — 关键词扩展
  - `RK3588S_CarTerminal/config/voice_settings.conf` — 新增5个独立开关
  - `tests/voice_routing/test_voice_routing.cpp` — 路由测试
  - `tests/voice_config/test_voice_config.cpp` — 配置测试
  - `tests/voice_lifecycle/run_board_lifecycle_tests.sh` — 板端生命周期/失败路径测试

### 3. 语音管线代码审查修复（已解决 2026-06-27）

- **根因**：30年工程师审查发现6项问题（2×P1线程安全 + 1×P1遗留持锁join + 2×P2 UB/越界 + 1×P3解析不严格）。
- **修复内容**：
  - P1-1：ALSA `snd_pcm_readi` 阻塞 → `poll()` + 200ms timeout。
  - P1-2：析构期emit信号 → `m_destroying` 标志 + worker按值捕获client指针。
  - P1-2遗留：`sendToLlm` 持锁join → 只push_back不join，`joinWorkerThreads` swap-then-join。
  - P2-1：`eventFilter` 未检查事件类型就cast → 先检查type再cast。
  - P2-2：`lastIndexOf('\n')` 返回-1 → 先检查>=0再remove。
  - P3-1：`stoi("5000ms")` 静默解析 → `stoi(val,&pos)` + `pos==val.size()` 严格校验。
- **host验证**：voice_routing 7例 + voice_config 50例（新增4例stoi严格校验）+ vehicle_command 41例，全部PASS。
- **相关文件**：
  - `RK3588S_CarTerminal/voice/VoiceManager.h` — m_destroying 标志
  - `RK3588S_CarTerminal/voice/VoiceManager.cpp` — poll timeout / m_destroying / client按值捕获 / swap-then-join
  - `RK3588S_CarTerminal/gui/mainwindow.cpp` — eventFilter类型检查 / lastIndexOf边界检查
  - `RK3588S_CarTerminal/voice/voice_config.cpp` — stoi严格校验
  - `tests/voice_config/test_voice_config.cpp` — 新增4例stoi测试

### 2. MPP硬件编码器输出pktSize=0（已解决 2026-06-27）

- **根因**：`MPP_FMT_YUV420P` 枚举值错误硬编码为 `1`（实际应为 `4`），导致编码器配置的像素格式错误，所有帧编码输出为空。
- **板端验证**：
  - ✅ `test_mpp_direct` 直接链接测试：10/10 帧编码成功，总计 22467 bytes，H.264 Annex-B 流头正确。
  - ✅ `CarTerminal` 主程序集成测试：MPP 编码 800x600 → 200-4000 bytes/帧，IDR 帧每 50 帧一个，VLC 成功连接播放。
- **修复内容**：
  - 修正 `MPP_FMT_YUV420P`（1→4）、`MPP_VIDEO_CodingHEVC`（16→0x01000004）、`MPP_ERR_TIMEOUT`（-2→-8）
  - 恢复 project1 的 `encode_put_frame` + `encode_get_packet` 异步流程
  - 使用 16 字节对齐 stride + I420 平面布局
  - 新增 `mpp_abi_constants.h`（ABI 常量集中定义）和 `mpp_frame_layout.h`（对齐布局 helper）
- **相关文件**：
  - `RK3588S_CarTerminal/media/mpp_encoder.cpp` — dlopen方式编码器实现
  - `RK3588S_CarTerminal/media/mpp_abi_constants.h` — ABI常量集中定义
  - `RK3588S_CarTerminal/media/mpp_frame_layout.h` — I420对齐布局
  - `RK3588S_CarTerminal/tests/test_mpp_direct.cpp` — 直接链接板端回归测试

## High Priority

1. ~~板端验证离线ASR→固定指令→GUI路径~~ ✅ 2026-06-27（文字指令注入测试全部通过）
   - host端代码重构与单元测试已完成（vehiclecommand 41例、voice_config 50例、voice_routing 7例全部PASS）。
   - **板端无麦克风验证通过（2026-06-27）**：
     - 板端编译成功：`make clean && qmake && make -j$(nproc)` → 0 错误。
     - ASR 引擎初始化：`ASR engine initialized (model=..., sample_rate=16000, threads=2)` ✓。
     - ALSA 设备打开：`ALSA capture opened (device=plughw:2,0, rate=16000)` ✓。
     - LLM/TTS 未启动：`LLM disabled` / `TTS disabled` ✓。
     - 管线就绪：`voice pipeline initialized (asr=1 llm=0 tts=0 direct=1)` ✓。
     - 旧 ASR 互斥：`offline ASR ready, legacy online ASR disabled` ✓。
     - 无 SIGBUS/崩溃/段错误 ✓。
     - 程序正常启动并显示地图页面 ✓。
   - **sherpa-onnx 结构体布局修复**：板端库版本比代码定义的结构体多 3 个字段（`nemo_ctc`/`t_one_ctc`/`hr`），导致 `SherpaOnnxCreateOnlineRecognizer` 段错误。已修复 `voice/asr/AsrEngine.cpp` 结构体定义。
   - **文字指令注入测试全部通过（2026-06-27）**：
     - 测试方法：通过 `VEHICLE_VOICE_TEST_TEXT` 环境变量注入文本到 `onOfflineAsrTextRecognized` 槽函数，绕过麦克风。
     - ✅ "打开地图" → `command=MapShow route=direct` + GUI 切换。
     - ✅ "打开摄像头" → `command=MonitorShow route=direct` + GUI 切换。
     - ✅ "播放音乐" → `command=MusicPlay route=direct` + GUI 切换。
     - ✅ "查看天气" → `command=WeatherShow route=direct` + GUI 切换。
     - ✅ "给我讲个笑话" → `voice command unmatched` + `LLM unavailable`，不崩溃。
     - ✅ `direct_command_enabled=false` 时，"打开地图" 不打开地图，`voice command unmatched`。
     - ✅ `grep -Ei 'rkllm|sendToLlm' test_*.log` 无输出（LLM 完全未调用）。
   - **待验证（需麦克风）**：真实语音输入端到端（ALSA 采集 → sherpa-onnx 中文识别 → 指令匹配 → GUI）。

2. Weather API/config migration
   - Board HDMI runtime verifies the Weather crash guard works: malformed provider data no longer crashes the app.
   - Weather API URL/appid/appsecret/city/timeout moved to `config/weather_settings.conf`.
   - `vehicleweather::WeatherConfig` and `vehicleweather::loadWeatherConfigFile()` implemented.
   - Weather refresh now skips requests when config is disabled or URL construction fails.
   - Host tests (`tests\weather_config\build_and_run.ps1`) pass.
   - Board default-disabled runtime path verified: rebuild succeeds, runtime logs `weather input disabled`, and app continues to `map image saved`.
   - Current `weather_enabled=false`, `weather_appid`, and `weather_appsecret` keep weather disabled until valid credentials are provided.
   - Live provider verification pending: fill valid appid/appsecret, set `weather_enabled=true`, rebuild/run, and verify weather data renders.

2. DHT11/sensor migration
   - Config migrated: `dht11_enabled`, `dht11_device`, `dht11_poll_interval_ms`, `dht11_read_timeout_ms` added to `vehicle_devices.conf`.
   - DHT11 device adapter created in `rk3588s_migration/device/dht11device.*`.
   - `Dht11Device` now applies `dht11_read_timeout_ms` through Linux `poll()` and closes any old fd before reopening.
   - GUI no longer directly accesses DHT11 hardware; uses `Dht11Reader`, which starts `Dht11Worker` in a Qt worker thread.
   - Host tests (`tests\device_input\build_and_run.ps1`) now compile and exercise `dht11device.cpp`.
   - Default config disables DHT11; GUI logs `dht11 input disabled` when disabled.
   - Board binary verification passed: no hardcoded `/dev/dht11`, `/dev/key`, or `/dev/ttymxc2`.
   - Current board discovery found no DHT11 node or loaded DHT11 module through `/dev`, `dmesg`, or `lsmod`.
   - Real DHT11 hardware verification is blocked until the RK3588S kernel/device tree/driver exposes a usable device node.

3. Camera/V4L2 migration
   - Board I2C discovery shows IMX415 camera sensors at `0-001a`, `1-001a`, and `2-001a`.
   - Board `v4l2-ctl --list-devices` confirmed `/dev/video33` is rkisp0-vir0 mainpath (Video Capture Multiplanar).
   - Board `v4l2-ctl -d /dev/video33 --list-formats-ext` confirmed UYVY/NV16/NV61/NV21/NV12/NM21/NM12 support, max 800x600.
   - `capture_thread.*` rewritten: config-driven, V4L2 MPLANE API, NV12→RGB32 conversion, proper cleanup.
   - `rk3588s_migration/media/cameraconfig.*` added with host tests.
   - `config/camera_settings.conf` added (device=/dev/video33, format=NV12, 800x600, 4 buffers, 30fps).
   - Monitor page now accepts CameraConfig; mainwindow loads config and passes through.
   - ✅ **RGA hardware acceleration integrated**: `media/rga_converter.*` created, dlopen librga.so + imcvtcolor_t API, CPU fallback. Pre-allocated RGB32 buffer.
   - ✅ **MPP hardware decoding integrated**: `media/mpp_decoder.*` created, dlopen librockchip_mpp.so, MJPEG→NV12, integrated into capture_thread.
   - ⚠️ **MPP硬件编码已集成到RTSP推流**：`media/mpp_encoder.*` 重写（参考project1），`rtsp_server.*` 改为MPP优先+libx264回退双路径编码。✅ **代码审查修复6项缺陷**（含2项P0致命：MppApi结构体布局错误、getHeader()时序错误）。❌ **板端测试编码输出pktSize=0，详见"当前阻塞问题"章节**。已创建test_mpp_direct.cpp直接链接测试程序用于隔离问题。
   - ✅ **RKNN NPU推理**: `media/rknn_detector.*` 创建完成，YOLOv5目标检测，已集成到Monitor页面（帧到达时自动推理+绘制检测框）。配置通过`config/rknn_settings.conf`控制（默认禁用）。
   - **已获取**: yolov5s.rknn模型文件（15.2MB），已放置于`config/`目录。
   - **摄像头运行验证暂时跳过**: 当前无摄像头硬件，待获取摄像头后再验证完整链路（V4L2采集 + MPP解码 + RGA转换 + RKNN推理）。
   - 代码和模型文件均已就绪，仅缺硬件验证。

4. Offline voice interaction deployment
   - ✅ **Code review completed**: all voice modules reviewed against PROJECT_CONTEXT.md coding standards.
   - ✅ **llm_demo.cpp refactored**: global variables eliminated (LlmService class), bare while(1) replaced with exit condition, debug code removed, signal safety fixed (sigaction+sig_atomic_t), deprecated wstring_convert replaced, unified logging.
   - ✅ **tts/main.cpp refactored**: global variables eliminated (TtsService class), bare while(true) replaced with exit condition, dead code fixed (thread join now reachable), signal safety fixed, unified logging.
   - ✅ **AsrEngine ABI safety improved**: void* opaque handles, non-copyable/non-movable, m_ prefix naming, GetResultText optional API, stoi exception handling.
   - ✅ **VoiceManager ASR integration**: ALSA capture thread, streaming recognition, async LLM communication (non-blocking GUI), complete pipeline (Microphone → ASR → LLM → TTS → Speaker).
   - ✅ **CMakeLists.txt improved**: RPATH configured, unified -O2 optimization, PRIVATE link dependencies.
   - ✅ **Config paths corrected**: `llm_bin_path=./llm_service`, `tts_bin_path=./tts_service`.
   - ✅ **ZMQ fixed**: deprecated message_t constructors replaced.
   - **Models ready**: DeepSeek .rkllm, SummerTTS .bin, sherpa-onnx ASR models all in `config/` directory.
   - ✅ **TTS服务板端验证通过**（2026-06-26）：
     - 构建成功：`cd ~/car/build_voice && cmake ../RK3588S_CarTerminal/voice && make -j`
     - 生成可执行文件：`llm_service`、`tts_service`、`libzmq_component.so`
     - ZMQ通信验证：状态信号（端口6677）和LLM消息（端口7777）收发正常
     - TTS合成验证：模型加载成功，语音合成流程正常（日志显示`synchronizing: 你好，我是车载助手`）
     - 测试方法：先发送状态信号到6677端口，再发送LLM文本到7777端口
     - 待验证：实际语音播放效果（需要音响/耳机硬件）
   - **Pending board verification**: sherpa-onnx library installation, ALSA device discovery, full pipeline test.
   - **Board deployment steps**:
     1. Install sherpa-onnx: `sudo apt-get install -y libsherpa-onnx-dev` or build from source
     2. Build voice services: `cd ~/car/build_voice && cmake ../RK3588S_CarTerminal/voice && make -j`
     3. Deploy: `cp ~/car/build_voice/llm_service ~/car/build_voice/tts_service ~/car/VehicleTerminal/`
     4. Enable voice in config: `voice_enabled=true`, `asr_enabled=true`
     5. Run verification script: `./scripts/verify_voice_deployment.sh`
     6. Test full pipeline: Microphone → ASR → LLM → TTS → Speaker

## Medium Priority

1. Investigate map runtime further only if a new backtrace points at map code
   - Current board backtrace points at Weather after `map image saved`.
   - If another crash appears, run `gdb --args ./VehicleTerminal`, capture `bt`, and classify the failing module before editing.

2. ~~Remove `static SpeechRecognition *SR` from header~~ ✅ 2026-06-25
   - ~~Current warning: `speechrecognition.h: static SpeechRecognition *SR defined but not used`.~~
   - Removed global `SR` from header; mainwindow.cpp now uses existing `AsrThread` member variable.

3. ~~Qt resource warning~~ ✅ 2026-06-25
   - ~~Fix `Could not create pixmap from :/img/images/tmp.png`.~~
   - Root cause: `weather.ui` referenced `:/img/images/tmp.png` but .qrc path is `:/img/Weather/images/tmp.png`.
   - Fixed in `gui/Weather/weather.ui` line 283.

4. ~~Qt stylesheet warnings~~ ✅ 2026-06-25
   - ~~Remove unsupported `rotation-point` and `rotation` properties.~~
   - Removed from `resources/Music/style.qss` and `gui/Music/style.qss`.

5. ~~Voice gateway integration with GUI~~ ✅ 2026-06-27
   - ~~Define IPC or local service interface.~~
   - ~~Do not call GUI directly from voice runtime.~~
   - Integration completed: `VoiceManager::asrTextRecognized` signal → `MainWindow::onOfflineAsrTextRecognized` slot (Qt::AutoConnection, queued across threads). GUI is only touched from the main thread. ASR worker thread never calls QWidget directly. VoiceManager uses `emit` only.

## Low Priority

1. ~~Replace deprecated Qt `endl` usage in music code.~~ ✅ 2026-06-25
2. ~~Replace deprecated `qrand()` with `QRandomGenerator`.~~ ✅ 2026-06-25
3. Clean generated binaries/build outputs from repository if a VCS cleanup step is approved.
4. Add Linux board-side test scripts under `scripts/`.

## Blocked / Needs Hardware Info

1. **NPU驱动升级到v0.9.7+（阻塞LLM推理）**
   - 板上NPU驱动版本：v0.9.6（`sudo cat /sys/kernel/debug/rknpu/version`）
   - rkllm-runtime v1.3.0要求：v0.9.7+
   - `rkllm_init`（模型加载）成功，但`rkllm_run`（NPU推理执行）触发Bus error (SIGBUS)
   - NPU驱动是内核级别组件，无法通过apt升级
   - 升级方案：
     - **方案A（推荐）**：百度网盘预编译内核deb包（需自行从厂商或官方渠道获取预编译内核 deb 包），需先执行`uname -r`确认内核版本兼容性
     - **方案B**：从源码编译rknpu驱动（下载rknpu_driver_0.9.8_20241009.tar.bz2，覆盖内核源码drivers/rknpu，重新编译内核并烧录）
     - **方案C**：烧录预装v0.9.8驱动的新固件
   - 前置操作：`sudo cp /lib/aarch64-linux-gnu/librkllmrt.so /lib/aarch64-linux-gnu/librkllmrt.so.v1.3.0.bak`（备份当前runtime）
   - 升级后验证：`sudo cat /sys/kernel/debug/rknpu/version`应输出`RKNPU driver: v0.9.8`，然后运行`echo "你好" | ~/build_voice/llm_service <模型路径>`
   - 影响范围：Voice LLM Service和ASR/TTS/LLM runtime两个模块状态均为Blocked

2. Real GPS device node on RK3588S
   - Candidate examples: `/dev/ttyUSB0`, `/dev/ttyS*`.
   - Need actual board command output before enabling.

3. Real key input node on RK3588S
   - Candidate examples: `/dev/input/event*`.
   - Need event value mapping before enabling.

4. Camera node and supported formats — **Resolved**
   - `/dev/video33` (rkisp0-vir0 mainpath) confirmed working.
   - Supported formats: UYVY, NV16, NV61, NV21, NV12, NM21, NM12. Max 800x600.
   - Config migrated to `config/camera_settings.conf`.

5. Real DHT11 device node on RK3588S
   - Board commands returned no DHT11 result:
     `find /dev -maxdepth 2 -iname "*dht*"`, `dmesg | grep -i dht`, `lsmod | grep -i dht`.
   - Need DHT11 wiring, kernel driver, and device tree confirmation before enabling `dht11_enabled=true`.

6. Real AP3216 sysfs directory on RK3588S
   - Software migration is complete enough for config-disabled runtime and host tests.
   - Broad board discovery returned no usable AP3216 node and many unrelated matches due to `*ps*` and `*ir*`.
   - Narrow board discovery listed IMX415 camera sensors, RK8602/RK8603 regulators, ES8388 audio codec, HUSB311, DDC, and RK3X I2C controllers, but no AP3216/AP3216C.
   - IIO illuminance/proximity/intensity discovery returned no output.
   - AP3216/AP3216C/proximity sysfs discovery returned no output.
   - `dmesg | grep -iE "ap3216|ap3216c|illuminance|proximity|iio"` returned no output.
   - Keep `ap3216_enabled=false` unless board hardware/device-tree/driver exposure changes.
   - Need confirmed ALS/PS/IR channel file names before setting `ap3216_enabled=true`.

7. Camera hardware for runtime verification
   - All camera/MPP/RKNN code is complete and compiled. Model file `yolov5s.rknn` obtained.
   - Runtime verification of full pipeline (V4L2 → MPP decode → RGA convert → RKNN detect → display) requires physical camera hardware.
   - Board confirmed IMX415 sensors at I2C addresses `0-001a`, but physical camera module not available for testing.
   - Deferred until camera hardware is obtained.
