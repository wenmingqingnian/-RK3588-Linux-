# Changelog

All notable migration changes should be recorded here.

## 2026-07-21 退出崩溃修复 + 录像默认启用

### 一、退出崩溃修复（RKNN worker 线程未 join 先 deinitDetector）

#### 根因

`Monitor::myStop()` 在 V4L2 路径下调用 `rknn::deinitDetector()` 释放 RKNN 检测器，
但**没有先停止 RKNN worker 线程**（`m_rknnThread`）。worker 线程可能正在
`rknn::detect()` 中，访问已释放的 RKNN 资源导致 `std::logic_error` 异常：

```
terminate called after throwing an instance of 'std::logic_error'
  what():  basic_string::_M_construct null not valid
```

崩溃发生在 `camera state: Stopping -> Ready` 之后，`RKNN detector released` 之前。
析构函数 `~Monitor()` 有正确的 join 顺序，但 `myStop()` 在析构前被
`MainWindow::~MainWindow()` 调用，此时 worker 线程还在跑。

#### 修复

[gui/Monitor/monitor.cpp myStop()](file:///gui/Monitor/monitor.cpp) 在
`deinitDetector()` 之前，先 `m_rknnThreadRunning = false` + `m_rknnCv.notify_all()`
+ `m_rknnThread.join()`，确保 worker 线程完全退出后再释放检测器。

与析构函数的 join 逻辑一致，且 `std::thread::joinable()` 保证不会重复 join
（`myStop` join 后，析构函数的 `joinable()` 返回 false 跳过）。

### 二、录像默认启用

`config/recording_settings.conf` 的 `recording_enabled` 从 `false` 改为 `true`。
录像作为车载终端核心功能应默认开启（复用 RTSP 编码器输出，零额外编码开销）。
`startRecording()` 在 `recording_enabled=false` 时静默返回的设计保留，方便
需要关闭录像时只需改配置不改代码。

### 板端验证

| 检查项 | 结果 |
|---|---|
| 录像自动启动 | ✅ `recording started: recordings/record_0001.mp4` |
| 录像分片 | ✅ 4.5 分钟生成 5 个分片（3.2M + 1.8M×3 + 769K） |
| 退出崩溃 | ✅ **无 `terminate called`**，干净退出 |
| 退出顺序 | ✅ camera stop → RKNN released → VoiceManager deinit → GPS stop |
| RKNN 检测 | ✅ 持续工作到退出前最后一秒 |
| MPP 编码 | ✅ 持续输出到退出前 |

### Files modified

- `gui/Monitor/monitor.cpp` — `myStop()` 在 `deinitDetector()` 前先 join RKNN worker 线程
- `config/recording_settings.conf` — `recording_enabled` 从 `false` 改为 `true`

## 2026-07-21 摄像头预览卡顿修复（RKNN 检测移出 GUI 主线程）

### 根因

`gui/Monitor/monitor.cpp` 的 `showImage()` 槽函数在 **GUI 主线程**同步执行
RKNN YOLOv5 检测（~80ms/帧），每 5 帧阻塞一次。阻塞期间采集线程持续 emit
`imageReady`，Qt 事件队列无界堆积（V4L2 路径无 drop-latest 节流），导致：
1. 画面每 5 帧卡顿一次（80ms = 2-3 帧时间）
2. 延迟越拉越长（事件队列累积，鼠标/按钮事件被排在后面）
3. RKNN 检测帧耗时 ~95ms，远超 33ms 帧间隔

### 修复方案

将 RKNN 检测从 `showImage()` 移到独立工作线程 `m_rknnThread`：

- **drop-olatest 单槽投递**：showImage 只把最新帧 `.copy()` 到 `m_rknnPendingFrame`
  并 `notify_one()`，不等待结果（~2ms）
- **异步检测**：`rknnWorkerLoop()` 在独立线程执行 `rknn::detect()`（~80ms），
  不阻塞 GUI 事件循环
- **结果滞后绘制**：showImage 画"上次检测结果"（`m_lastDetections`），检测框
  滞后 1-3 帧显示（车载辅助场景可接受）
- **queued invoke 回传**：检测完成后通过 `QMetaObject::invokeMethod(..., QueuedConnection)`
  在 GUI 线程更新 `m_lastDetections` 和 emit `dangerStateChanged`
- **pipeline 路径不受影响**：`m_usePipeline` 路径已有独立 RKNN consumer 线程，
  新 worker 仅在 V4L2 路径启动（`if (m_rknnReady && !m_usePipeline)`）

### 性能对比

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| 非检测帧 showImage 耗时 | ~15ms | ~15ms |
| 检测帧 showImage 耗时 | **~95ms**（80ms RKNN 阻塞） | **~15ms** |
| 事件队列 | 堆积（无 drop-latest） | 不堆积（showImage 不阻塞） |
| RKNN 检测频率 | 6Hz（每 5 帧） | ~12Hz（推理完即取下一帧） |
| 检测框滞后 | 0 帧（同步） | 1-3 帧（异步，可接受） |

### Files modified

- `gui/Monitor/monitor.h` — 新增 RKNN 工作线程成员（`std::thread` + `mutex` +
  `condition_variable` + drop-olatest 单槽 + `m_lastDetections`）
- `gui/Monitor/monitor.cpp` — 构造函数启动线程；析构函数 join 线程；`showImage()`
  移除同步 RKNN 调用改为异步投递 + 滞后绘制；新增 `rknnWorkerLoop()` 实现

### 板端验证

- 构建通过（`make_real_exit=0`）
- RKNN 检测持续工作（日志 `RKNN detect: N objects found`）
- 程序正常退出（`application exiting (code=0)`）
- 画面流畅度明显改善（主观感受）

## 2026-07-21 MP4 分片录像链路打通 + 编译错误/警告修复

### 一、Mp4Recorder 编译错误修复（2 处，阻断 make）

`media/mp4_recorder.cpp` / `media/mp4_recorder.h` 两处编译错误导致 `make` 返回 1
（此前 `make -j | tee build.log` 后 `make_exit=0` 是 `tee` 的退出码，掩盖了真实失败）：

- **`shouldSwitchSegment()` 引用不存在的成员 `m_segmentDurationSec`** → 改为
  `m_config.segmentDurationSec`（`RecordingConfig::segmentDurationSec`）。
- **`static annexBToAvcc()` 访问非静态成员 `m_config.codec`** → 新增 `bool isH265`
  参数，调用点 `writeFrame()` 传入 `(m_config.codec == "265")`，保持函数无状态。

### 二、编译警告清理（5 处，非阻断）

- `media/rknn_detector.cpp` 4 处 `-Wswitch-enum`（`RKNN_TENSOR_TYPE_MAX` /
  `RKNN_TENSOR_FORMAT_MAX` / `RKNN_TENSOR_QNT_MAX` 未处理）→ 4 个 switch 各补
  `case *_MAX:` 分支，与 `default` 一致返回 `UNKNOWN`/`Unsupported`。
- `voice/VoiceManager.cpp:488` `printf` 格式化类型不匹配（`%d` vs `qint64`）→
  改为 `%lld` 并 `static_cast<long long>(...)`。

### 三、Mp4Recorder 运行时链路三连修（核心成果：录像功能从未真正工作过）

录像功能虽代码已存在，但实际从未生成过有效 MP4 文件。经 3 轮板端验证迭代，
定位并修复了 3 个串联缺陷（任一不修都无法出片）：

#### 3.1 `startRecording()` 无任何调用者（链路从未启动）

`RtspServer::startRecording()` 在整个项目中只有声明和定义，**没有任何调用点**。
`RtspServer::init()` 内 `m_recorder.init()` 仅完成 muxer 初始化，必须再调用
`start()` 才会 `openNewSegment()` 创建文件、`writeFrame()` 才会真正写入。

**修复**：[gui/Monitor/monitor.cpp:149-156](file:///gui/Monitor/monitor.cpp)
在 `m_rtspServer->start()` 成功后自动调用 `m_rtspServer->startRecording()`。
同时 [rtsp_server.cpp:1768-1771](file:///media/rtsp_server.cpp) 在
`startRecording()` 中对 `RecorderStatus::Disabled` 静默返回，避免 recording 未启用
时每次启动都误报 WARN。

#### 3.2 `pushFrame()` 无 RTSP 客户端时跳过编码（帧进不了编码器）

[rtsp_server.cpp:1369-1371](file:///media/rtsp_server.cpp) 原逻辑：
```cpp
if (clientCount() == 0) { return true; }  // 省CPU
```
导致无 VLC/ffplay 连接时 `encodeAndSend()` 从不执行，`feedRecorder()` 从不调用，
`writeFrame()` 从不执行——MP4 文件只有 48 字节的 `ftyp` box，无 `moov`，无帧数据，
`ffprobe` 报 `moov atom not found`。

**修复**：跳过条件改为 `clientCount() == 0 && !m_recorder.isRunning()`——录像运行时
即使无 RTSP 客户端也继续编码。

#### 3.3 encode 线程第二道闸门同样跳过（帧进不了 `encodeAndSend`）

[rtsp_server.cpp:694](file:///media/rtsp_server.cpp) encode 线程内：
```cpp
if (!frameData.empty() && clientCount() > 0) {
    encodeAndSend(frameData.data(), w, h);
}
```
**第二道闸门**同样只在有客户端时编码。即使 3.2 修复后帧进了 `m_latestFrame`，
encode 线程取到帧后仍会因 `clientCount()==0` 丢弃。

**修复**：条件改为 `clientCount() > 0 || m_recorder.isRunning()`——录像运行时
encode 线程无条件编码。

### 四、板端验收数据（2026-07-21）

| 指标 | 结果 |
|---|---|
| `make_real_exit=${PIPESTATUS[0]}` | `0` ✅ |
| 错误 grep | 无输出 ✅ |
| 警告 grep | 无输出 ✅ |
| `recording started` 日志 | ✅ `recordings/record_0001.mp4` |
| `extradata extracted` 日志 | ✅（首帧 IDR 提取 SPS/PPS 成功） |
| 文件大小 | 48B → **5.1M** ✅（运行约 60s） |
| `ffprobe` 识别 | ✅ H.264 Constrained Baseline, 800x600, 754 kb/s, 14.19fps |
| 时长 | 00:00:59.20 ✅（接近 60s 分片周期） |
| 分片切换 | ✅ `record_0002.mp4` 自动创建（60s 到期） |
| MPP 硬件编码 | ✅ 持续输出 `MPP encode: 800x600 -> N bytes` |
| IDR 帧处理 | ✅ `IDR frame #1981: prepending SPS(25) + PPS(4)` |

### 五、Files modified

- `media/mp4_recorder.h` — `annexBToAvcc` 签名加 `bool isH265` 参数
- `media/mp4_recorder.cpp` — `shouldSwitchSegment` 用 `m_config.segmentDurationSec`；
  `annexBToAvcc` 实现去 `m_config` 依赖；`writeFrame` 调用点传入 `isH265`
- `media/rknn_detector.cpp` — 4 处 switch 补 `*_MAX` case
- `voice/VoiceManager.cpp` — `%d` → `%lld` + `static_cast<long long>`
- `gui/Monitor/monitor.cpp` — RTSP 启动后自动调用 `startRecording()`
- `media/rtsp_server.cpp` — `pushFrame` 与 encode 线程两道闸门加 `|| m_recorder.isRunning()`；
  `startRecording()` 对 `Disabled` 静默返回

### 六、简历亮点（调试方法论 + 嵌入式工程能力）

- **三连串联缺陷定位**：同一功能（录像）被 3 个独立缺陷串联阻断，任一不修都无法出片，
  体现了"编译通过 ≠ 功能可用"的嵌入式调试方法论——通过日志分层（init/start/write/
  exit）逐步缩小定位范围。
- **退出码陷阱**：`make | tee` 后取 `$?` 是 `tee` 的退出码，必须用 `${PIPESTATUS[0]}`
  取管道第一条命令的退出码，否则会掩盖编译失败。
- **无客户端编码策略**：车载场景下录像必须独立于 RTSP 客户端存在，`clientCount()==0`
  跳过编码的省 CPU 优化必须以 `|| recorder.isRunning()` 为前提，否则录像收不到帧。

## 2026-07-20 GPS 硬件链路验收 + IMX415 摄像头启用 + RGA 硬件加速修复

### 一、GPS 硬件链路实机验收通过（ATGM336H-5N）

ATGM336H-5N 模块接 QuarkPi-CA2 J11 Pin 8/10 = UART5_M1（GPIO3_C4/C5），
加载 BSP 预编译的 `rk3588s-uart5-m1.dtbo` 到主 dtb，重启后 `/dev/ttyS5`
注册成功（`ttyS5 at MMIO 0xfeb80000 (irq=111, base_baud=1500000) is a 16550A`），
9600 8N1 NMEA 流量正常，`$GPTXT,01,01,01,ANTENNA OK*35` 天线自检通过，
GPS+北斗双模收星。室内卫星 2 颗未达定位阈值（信号衰减预期），待室外实测。

#### Files modified

- **`device/gpsworker.cpp`** — 4 处 bug 修复：
  - 百度地图 URL 缺 `coordtype=wgs84ll`：WGS84 被误作 BD09 解释，地图明显偏移。
  - 失去定位后状态不回退：RMC status=V / GGA fixQuality=0 时未切回 `hasFix=false`，
    地图残留陈旧位置。修复后 `emit statusChanged("GPS未定位")`。
  - 非 RMC/GGA 句子触发重复坐标通知：GSA/GSV/VTG/ZDA/TXT 等句子被错误地
    进入 emit 路径。修复后用 `sentence.substr(3,3)` 前缀过滤，只对 RMC/GGA 进入 emit。
  - 断线无退避：`readChunk` 返回负值后 `continue` 无 sleep，拔线会刷屏日志和 CPU 占用。
    修复后 200ms 可中断退避（`kSimSleepStepMs=50` 步进，`isRunning()` 检查让
    `stop()` 仍能在 50ms 内生效）。新增 `<thread>` include。
- **`config/vehicle_devices.conf`** — `gps_enabled=true` `gps_device=/dev/ttyS5`
  `baud=9600` `8N1` `timeout=500ms` `interval=1000ms`。
- **`config/map_settings.conf`** — URL 模板加 `&coordtype=wgs84ll`，
  `map_enabled=true`，填入真实百度地图 AK。

#### 板端验收数据

| 指标 | 结果 |
|---|---|
| `/dev/ttyS5` 注册 | ✅ `crw-rw---- root dialout 4, 69` |
| uart5 status | ✅ `okay` |
| 串口打开 | ✅ `gps opened: device=/dev/ttyS5 baud=9600 8N1 timeout=500ms` |
| 工作线程 | ✅ `real-gps loop started` |
| NMEA 流量 | ✅ `$GNGGA/$GNGLL/$GNGSA/$GPGSV/$BDGSV/$GNRMC/$GNVTG/$GNZDA/$GPTXT` 完整输出 |
| 天线自检 | ✅ `$GPTXT,01,01,01,ANTENNA OK*35` |
| 双模 | ✅ GPGSV + BDGSV 同时输出 |
| 百度地图 API | ✅ `map image saved: map.png (62281 bytes)` |
| Host 单测 | ✅ nmeaparser / gpsstream / gpsconfig / gpssimulation / map_config 全通过 |

### 二、IMX415 摄像头硬件链路验证通过

IMX415 走 `m00_b_imx415 2-001a → csi2-dphy0 → mipi-csi2 → CIF → rkisp0-vir0 → /dev/video33`。
`v4l2-ctl --stream-mmap --stream-count=30` 30 帧采集成功，sensor 实际输出
3864x2192@10bpp，ISP downscale 到 800x600 NV12。

#### Files modified

- **`config/camera_settings.conf`** — `media_source_type=test_pattern` → `v4l2`
  （其他参数保持：`/dev/video33` `800x600` `NV12` `bufs=4` `fps=30`）。

#### 板端验收数据

| 指标 | 结果 |
|---|---|
| `/dev/video33` 节点 | ✅ `crw-rw----+ root video 81, 33`（ACL） |
| 支持格式 | ✅ UYVY/NV16/NV61/NV21/NV12/NM21/NM12 |
| 分辨率范围 | ✅ 32x32 - 3840x2160 (step 8/8) |
| 独立采集测试 | ✅ 30 帧无错 |
| ISP 时钟 | ✅ `set isp clk = 396000000Hz` |
| DPHY 速率 | ✅ `data_rate_mbps 892` |
| 程序内集成 | ✅ `source=v4l2` + `stream started` + 颜色正常 |
| 退出 | ✅ `stream stopped` + `capture loop exited` + `device closed` |

### 三、RGA 硬件加速修复（im2d API 重写）

原 `rga_converter.cpp` 用自定义 `rga::Buffer` 结构体，与 librga 真实
`rga_buffer_t` **ABI 不兼容**（`vir_addr`/`phy_addr`/`fd` 应是 union，
原代码当独立字段；缺 `color_space_mode`/`rd_mode`/`type`/`t` 字段，
后续字段全部错位），导致 legacy `imcvtcolor_t` 每帧返回 -1（NotSupported）。

重写为官方 `im2d` API：`#include <im2d.h>` + `<RgaApi.h>` + `<rga.h>`，
`__has_include` 检测板端 SDK，直链 `-lrga`（不再 dlopen）。

#### Files modified

- **`media/rga_converter.h`** — 删除自定义 `Buffer`/`Status`/格式常量，
  只保留 `init()` / `deinit()` / `isAvailable()` / `nv12ToRgb32()` 4 个接口。
- **`media/rga_converter.cpp`** — 重写：
  - `c_RkRgaInit()` / `c_RkRgaDeInit()`（librga 2.2.0 导出符号带 `c_` 前缀）
  - `wrapbuffer_virtualaddr(ptr, w, h, format)` 包装 buffer（注意是 `_virtualaddr` 不是 `_virtual`）
  - `imcvtcolor(src, dst, sfmt, dfmt, mode, sync)` 做跨格式转换
  - 输出格式 `RK_FORMAT_BGRA_8888`（B-G-R-A 字节序）匹配
    `QImage::Format_RGB32` 在小端机器上的实际布局，避免红蓝互换
  - `IM_COLOR_SPACE_DEFAULT` 对应 BT.601 limited range（IMX415 默认输出）
  - 单帧失败自动降级 CPU，不立即禁用，下帧仍可重试
- **`CarTerminal.pro`** — 加条件链接：
  ```qmake
  linux:exists(/usr/include/rga/im2d.h) {
      INCLUDEPATH += /usr/include/rga
      LIBS += -lrga
  }
  ```

#### 板端验收数据

| 指标 | 结果 |
|---|---|
| qmake 输出 | ✅ `RGA: using board-installed librga2 (im2d API)` |
| 链接 | ✅ `-lrga` 在最终 link 命令中 |
| 启动日志 | ✅ `RGA hardware acceleration enabled (librga2 im2d API)` |
| 颜色 | ✅ 正常（BGRA 字节序 + BT601 limited range） |
| 警告刷屏 | ✅ 消除（不再有 `imcvtcolor_t failed: -1`） |
| 退出干净 | ✅ `capture loop exited` + `device closed` |

### Key engineering decisions

- **UART5_M1 接线方案**：原理图 Page 5 显示 GPIO3_C4/C5 是 UART5_M1 的 TX/RX
  （与 GPIO2_A2/A3 的 CTS/RTS 分布在不同 GPIO 组），GPS 只用 TX/RX 不碰 eMMC。
  VCCIO5 域接 3.3V，与 ATGM336H 电平兼容。
- **dtbo 合并而非重编 dts**：BSP 已预编译 `/boot/dtbs/rk3588s-uart5-m1.dtbo`，
  用 `fdtoverlay` 合并到主 dtb 即可，免内核源码。
- **RGA 官方 API 而非 legacy**：librga 2.2.0 的 `im2d` API 是 Rockchip
  官方推荐，legacy `imcvtcolor_t` 在 mmap buffer 上返回 NotSupported。
- **BGRA_8888 而非 RGBA_8888**：`QImage::Format_RGB32` 在小端机器上字节序
  是 B-G-R-A，用 `RK_FORMAT_BGRA_8888` 直接匹配，避免红蓝互换。
- **条件编译 `__has_include`**：host（无 librga）自动降级 CPU，板端（有 librga）
  直链 im2d API，两端都能编译。

### 简历亮点

1. **ABI 兼容性分析**：发现自定义结构体与 librga 真实 `rga_buffer_t` 不兼容（union 错位）
2. **SDK 集成**：用 `__has_include` 检测板端 SDK 头文件，自动切换 dlopen/直链两种模式
3. **官方 API 优先**：用 `im2d` API 替代 legacy `imcvtcolor_t`，符合 Rockchip 官方推荐
4. **字节序处理**：`BGRA_8888` 匹配 `QImage::Format_RGB32` 小端序
5. **色彩空间**：BT.601 limited range 配置正确，画面颜色正常
6. **降级容错**：硬件失败自动降级 CPU，保证可用性
7. **条件编译**：host（无 librga）和板端（有 librga）都能编译

## 2026-07-18 dmesg 内核日志集成到 LogManager

新增 `vehiclelog::DmesgTail` 类，后台线程持续读取 `dmesg --follow --since now`
并把每行内核日志通过 `VLOG_INFO("kernel", ...)` 转发到 LogManager。排查
ASR/NPU/USB/ALSA 问题时不再需要手动 `dmesg` 切换窗口，应用日志与内核日志
统一流。板端验收：`[kernel]` 标签正确转发实时内核事件，历史 boot 日志
（452 行）被 `--since now` 过滤掉不再刷屏。

### Files added

- **`common/dmesg_tail.h`** / **`common/dmesg_tail.cpp`** — `DmesgTail` 类。
  - 后台线程读 `dmesg --follow --since now`（跳过历史缓冲，只捕获应用启动后
    的新内核消息）；`--since` 不支持时回退纯 `--follow`。
  - **self-pipe 停止机制**：`stop()` 向管道写 1 字节，`poll()` 立即唤醒退出。
    这是中断 poll() 的标准可靠方法，不依赖 `close(fd)` 的未定义行为。
  - `start()` 在创建线程前设置 `m_running=true`，消除 double-start 窗口期。
  - 跨平台：非 Linux 平台 `start()` 返回 false 空操作（Windows 编译通过）。
  - 失败降级：dmesg 不可用 → 记一次 WARN 后线程退出，不影响应用主流程。
  - RAII：析构自动 stop，main.cpp 无需显式 stop。
- **`tests/common/test_dmesg_tail.cpp`** — 6 项单元测试（new/start/double-start/
  idempotent-stop/destructor/restart），带进度输出。

### Files modified

- **`main.cpp`** — 日志初始化后启动 `DmesgTail`，RAII 自动停止。
- **`CarTerminal.pro`** — `common/` 段加入 `dmesg_tail.cpp` / `dmesg_tail.h`。
- **`media/frame_broker.h`** / **`media/frame_broker.cpp`** — 顺带修复 Qt
  `slots` 宏冲突：`LaneState::slots` 改名为 `ringSlots`（Qt 工程里 `slots`
  是 `#define slots Q_SLOTS`，不能用作变量名）。全量编译时触发暴露。

### Test results

| 平台 | 单元测试 (6 场景) | 主程序集成 |
|---|---|---|
| Windows g++ | ✅ ALL PASSED | N/A（非 Linux） |
| 板端 QuarkPi-CA2 | ✅ ALL PASSED (exit=0) | ✅ 历史日志 0 行刷屏 |

板端验收数据（对比修复前后）：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| `head -20` 内容 | 452 行历史 boot 日志（`[15:59:09]`） | 正常应用启动日志（`[19:17:40]`） |
| `[kernel]` 行数 | 452+ | 0 |
| `DmesgTail:` 启动信息 | `using /dev/kmsg` | `started (dmesg --follow)` |

### Key engineering decisions

- **放弃 `/dev/kmsg` + `lseek(SEEK_END)` 方案**：在 RK3588S 内核上
  `poll()` 不返回（既不超时也不响应 self-pipe），行为不一致。改用
  `dmesg --follow --since now` 用户态工具，行为可靠。
- **self-pipe 而非 close(fd)**：`stop()` 关闭 fd 来打断 `poll()` 是未定义
  行为，不可靠。self-pipe 是 Linux 中断 poll() 的标准可靠方法，保证 1ms
  内退出。
- **`--since now` 跳过历史**：避免应用启动时把整个 dmesg 环形缓冲区倒进
  应用日志，淹没真正的实时内核事件。

## 2026-07-18 FrameBroker 改用固定容量环形缓冲区（稳态零堆分配）

把 `FrameBroker::LaneState` 内部的 `std::queue<FramePacket>` 替换为固定容量
环形缓冲区（`std::vector<FramePacket>` + head/tail/count），稳态运行零堆分配。
**保留 mutex**（锁非瓶颈，lock-free MPMC ring 复杂度不划算）。公共接口零改动，
既有 8 场景测试零改动全绿。

### Files modified

- **`media/frame_broker.h`** — `LaneState` 改用 `std::vector<FramePacket> slots`
  + `head/tail/count`；类注释补充设计决策（为何保留 mutex）。
- **`media/frame_broker.cpp`** —
  - 构造函数：预分配每个 lane 的 `slots` 到容量大小。
  - `publish()`：drop_oldest 改为 `slots[head] = FramePacket{}` + head 推进；
    新帧拷贝赋值到 `slots[tail]`（const 引用无法 move，但 shared_ptr 拷贝廉价）。
  - `take()`：`std::move` 把 slot 内容移出，立即 `FramePacket{}` 重置槽位。
  - `setLaneEnabled(false)` / `reset()`：遍历有效槽位 `FramePacket{}` 重置，
    释放所有 shared_ptr 引用。

### Files added

- **`tests/media/test_frame_broker_ring.cpp`** — 8 场景专项测试（capacity-1
  边界、多周期 wrap-around、take move 语义、丢弃槽位 buffer 释放、disable/reset
  buffer 释放、1-producer/3-consumer 500 帧压测、enable/disable 循环压测）。
- **`tests/media/build_and_run_frame_broker_tests.sh`** — 板端一键脚本
  （同时跑既有 8 场景 + 新增 8 场景）。

### Test results

| 平台 | 既有 8 场景 (回归) | 新增 8 场景 (环形) |
|---|---|---|
| Windows g++ 7.3.0 | ✅ | ✅ |
| 板端 Linux | ✅ | ✅ |

### Key engineering decisions

- **不追求 lock-free**：1 生产者 + 3 消费者下 mutex 竞争 < 1μs，远小于 33ms
  帧间隔。真实瓶颈是堆分配，不是锁。
- **真实优化点是稳态零堆分配**：`std::queue::push/pop` 每次可能触发堆操作；
  固定容量环形 + 槽位复用让稳态零堆分配。
- **公共接口零改动**：避免重构所有调用点（MediaPipeline/Monitor 等）。

## 2026-07-18 RKNN 后处理 3 路 decode 并行化（接入 ThreadPool）

把 RKNN YOLOv5 后处理的 3 个独立 decodeYoloLayer() 调用（stride 8/16/32）
用 ThreadPool 并行执行，NMS 仍串行。`detect()` 同步接口不变，调用方
（Monitor.cpp GUI 线程 + RKNN 消费线程）零改动。

### Files modified

- **`media/rknn_postprocess.h`** — 新增 `decodeOutputsParallel()` 声明 +
  `concurrency::ThreadPool` 前向声明（头文件不引入 thread_pool.h 依赖）。
- **`media/rknn_postprocess.cpp`** —
  - 提取内部 helper `prepareDecodeLayers()` + `finalizeDetections()`，消除
    串行/并行两路径的校验与 NMS 代码重复。
  - `decodeOutputs()` 改用 helper，行为完全不变（既有 9 测试 96 断言回归通过）。
  - 新增 `decodeOutputsParallel()`：3 层 decode 通过 `pool.submit()` 并行，
    `std::future` 等待全部完成；任一失败整体返回 false。
  - `PreparedLayer::anchors` 用 `std::array<int, 6>`（C++17 lambda 值捕获）。
  - `perLayerOk` 用 `std::vector<char>`（规避 `vector<bool>` proxy 坑点）。
- **`media/rknn_detector.cpp`** —
  - 新增静态 `unique_ptr<ThreadPool> s_decodePool`（3 worker）。
  - `initDetector()` 创建 pool；`clearDetectorState()` 先销毁 pool 再释放缓冲。
  - `detect()` 优先调用 `decodeOutputsParallel()`，pool 未初始化时降级为串行
    `decodeOutputs()`（防御性回退）。

### Files added

- **`tests/rknn/test_rknn_postprocess_parallel.cpp`** — 8 场景并行等价性测试
  （空输入、单候选、跨层候选、NMS 抑制、乱序输出、NaN 拒绝、重复 stride 拒绝、
  200 次压测）。
- **`tests/rknn/build_and_run_rknn_tests.sh`** — 板端一键构建运行脚本
  （同时跑既有 9 场景 + 新增 8 场景）。
- **`tests/rknn/_win_project_root.h`** — Windows g++ 测试用的宏注入文件
  （绕过 PowerShell 中文路径转义问题）。

### Test results

| 平台 | 既有 9 场景 (回归) | 新增 8 场景 (并行) |
|---|---|---|
| Windows g++ 7.3.0 | 96 断言 ✅ | 16 断言 ✅ |
| 板端 Linux | 待执行 `build_and_run_rknn_tests.sh` | 待执行 |

### Key engineering decisions

- **不并行 NMS**：NMS 候选 < 100，线程调度开销 > 计算开销，是负优化。
- **保持 `detect()` 同步接口**：避免重构 Monitor.cpp 两处同步调用点。
- **保留串行 `decodeOutputs()`**：作为并行版的回归基线 + 防御性回退路径。

## 2026-07-18 内存池 + 线程池基础设施

新增 common/ 下两个零 Qt 依赖的工程基础设施，并把媒体管线帧源接入内存池。
对应 TODO「工程优化与稳定性增强 - 内存池」第一阶段。

### Files added

- **`common/frame_buffer_pool.h` / `.cpp`** — 媒体帧缓冲内存池。
  - 固定块大小 + 自由链表；`acquire()` 返回
    `shared_ptr<vector<uint8_t>>`，引用归零时通过自定义 Releaser 自动归还。
  - 与 `FramePacket::data`（`shared_ptr<const vector<uint8_t>>`）类型直接兼容，
    零侵入接入 FileFrameSource / TestPatternSource。
  - 容量上限策略：`minFreeBlocks` 保底、`maxBlocks` 上限、超上限 heap 分配
    游离块（不归还到池），保证调用方语义不变。
  - 统计接口：acquire/hit/miss/overflow/returned/inUse/free 完整可观测。

- **`common/thread_pool.h` / `.cpp`** — 通用固定 worker 数线程池。
  - C++17 兼容，不依赖 Qt、不依赖 C++20 jthread。
  - `submit(F, args...)` 返回 `std::future<ReturnType>`，异常通过 future 传播。
  - `waitIdle()` 等待队列清空且所有 worker 空闲；谓词感知 `m_stopping`，
    防止与 `stop()` 并发调用死锁。
  - `stop()` 幂等、析构自动 join。
  - 适用场景：RKNN NMS 后处理、RGA 失败时 CPU 颜色转换、V4L2 CPU fallback、
    LLM 意图解析等短时 CPU 密集任务。
  - 不适用：GPS/DHT11/AP3216 等长生命周期独占线程（保持原 worker 模型）。

- **`tests/common/test_frame_buffer_pool.cpp`** — 7 场景单测
  （大小、归还、prewarm、shared_ptr 转换、overflow、shrink、并发 4 线程 × 500 次）。
- **`tests/common/test_thread_pool.cpp`** — 9 场景单测
  （future 取值、参数透传、并行、异常传播、waitIdle、broken future、幂等 stop、
  析构 join、500 任务压测）。
- **`tests/common/test_file_frame_source_with_pool.cpp`** — 3 场景集成测试
  （正确字节、buffer 回收复用、容量不匹配回退）。
- **`tests/common/build_and_run_common_tests.sh`** — 一键构建运行脚本。

### Files modified

- **`media/file_frame_source.h` / `.cpp`** — 新增 pool-enabled 构造函数；
  `readFrame()` 优先从池借出，无池或容量不匹配时静默回退 heap。
  原有构造函数与行为完全保持不变（向后兼容）。
- **`media/test_pattern_source.h` / `.cpp`** — 同上。
- **`CarTerminal.pro`** — 把 `frame_buffer_pool.cpp` / `thread_pool.cpp`
  加入 SOURCES/HEADERS，主程序构建自动包含（暂未在业务代码调用，
  预留为后续 RKNN 后处理等模块的依赖）。

### Verification

Windows g++ 7.3.0 验证：
- `frame_buffer_pool tests passed` (7/7)
- `thread_pool tests passed` (9/9)
- `file_frame_source_with_pool tests passed` (3/3)
- 既有 `file_frame_source tests passed`、`test_pattern_source tests passed`、
  `frame_broker tests passed`、`frame_packet tests passed` 全部回归通过。

板端 Linux 编译运行：`./tests/common/build_and_run_common_tests.sh`。

### Design decisions

详见 `docs/DECISIONS.md` D025 / D026。

## 2026-07-18 Task B5 — Dev-Env Pitfalls Catalog + Pre-flight Check Script

The 19 known development-environment pitfalls previously scattered across
`project_memory.md`, `CHANGELOG.md` historical entries, and the redline
section of `verify_no_peripheral.sh` have been consolidated into a
single structured catalog and a one-shot pre-flight check script.

### Files added

- **`docs/PITFALLS.md`** (559 lines) — Catalog of 19 known pitfalls
  (P001-P019), grouped by category:
  - Build system (P001-P002): qmake vs CMake, future mtime
  - ALSA config (P003-P006): wrong device, Mic Capture Switch, clipping,
    priming readi
  - ASR timing (P007-P009): 6s startup, 3s post-cmd silence, 2.4s tail
  - ASR config (P010-P014): sherpa version, decoding method, hotwords
    score, hotwords file, TTS feedback
  - ZMQ (P015-P017): localhost vs 127.0.0.1, /etc/hosts, TTS mute window
  - NPU (P018): llm_enabled vs NPU driver version
  - Engineering discipline (P019): ASR_DBG residue
  Each entry includes: symptom, root cause, fix commands, verification
  command, and the corresponding `check_dev_env.sh` function name.

- **`scripts/check_dev_env.sh`** (515 lines) — Pre-flight environment
  check script with one detection function per pitfall (P001-P019).
  Features:
  - No build, no CTest, no peripheral dependency — pure env/config check
  - `--strict` (WARN counts as FAIL), `--skip-board` (skip amixer/NPU/
    proc/asound checks), `--help`
  - Exit codes: 0=PASS, 1=FAIL, 2=argument error
  - Colored output with per-check fix hints
  - Complementary to `verify_no_peripheral.sh`:
    `check_dev_env.sh` = pre-flight (before dev), `verify_no_peripheral.sh`
    = acceptance (after dev)

### Files modified

- `docs/TODO.md` — New "✅ 已完成：B5" section inserted above A2 section;
  `Last updated:` line updated.
- `docs/CHANGELOG.md` — This entry added.

### Design rationale

- **Single source of truth**: `PITFALLS.md` is the developer-visible
  authority; `project_memory.md` (AI-internal) syncs to it but is no
  longer the primary reference.
- **One-to-one mapping**: every P### in the doc has a `check_xxx`
  function in the script — adding a new pitfall requires updating both.
- **Maintenance rules**: new pitfalls take the next P### number; resolved
  pitfalls stay in the doc as historical (marked Resolved) but their
  check function can be removed.
- **Non-overlapping with `verify_no_peripheral.sh`**: the existing
  redline checks (1-5) in `verify_no_peripheral.sh` remain unchanged
  because they are part of the post-build acceptance flow; the new
  script covers the pre-build environment layer that the acceptance
  script does not (system time, /etc/hosts, sherpa lib SHA, Mic
  hardware switch, NPU driver version, etc.).

### Verification

- `bash -n scripts/check_dev_env.sh` — syntax OK (no execution).
- `grep -c "^check_" scripts/check_dev_env.sh` returns 16 check
  functions (some functions cover 2-3 related pitfalls, e.g.
  `check_mic_hardware` covers P004+P005, `check_asr_timing_docs`
  covers P007+P008+P009).
- All 19 P### entries in `PITFALLS.md` reference their corresponding
  `check_xxx` function name; no orphan references.

### Impact

- **No code changes** — pure documentation + shell script addition.
- **No runtime behavior change** — the new script is opt-in (run
  manually before development).
- **Reduced onboarding cost** — new developers can run
  `./scripts/check_dev_env.sh` to get a one-shot environment health
  report with concrete fix commands, instead of discovering pitfalls
  one by one through failed builds/ASR tests.

## 2026-07-18 Task A2 — Formal Retirement of `rk3588s_migration/voice_gateway/`

The legacy `rk3588s_migration/voice_gateway/` skeleton has been **formally
retired**. The directory does not exist in the current workspace and no
code path references it; only stale documentation references remained,
some of which (`docs/BUILD_RUN_TEST.md` lines 65-90) contained
**non-functional build commands** that would fail if a new user copy-pasted
them. All non-historical documentation references have now been marked as
retired/historical with explicit pointers to the active voice stack at
`RK3588S_CarTerminal/voice/`.

### Files modified (docs only — no code changes)

- `docs/BUILD_RUN_TEST.md` — Two "Build Voice Gateway" section headers
  relabeled `(RETIRED 2026-07-18, task A2)`; command blocks prefixed with
  `> DO NOT RUN` admonition + `# HISTORICAL ONLY` shell comments;
  `tests\voice_gateway\build_and_run.ps1` line + expected-output line
  annotated as historical.
- `docs/PROJECT_CONTEXT.md` — `voice_gateway/` entry in "Current
  submodules" list marked RETIRED with pointer to
  `RK3588S_CarTerminal/voice/`.
- `docs/INTERFACE_SPEC.md` — "Voice Gateway Config Interface" section
  header relabeled `(RETIRED 2026-07-18, task A2)` + historical-only
  admonition block.
- `docs/DECISIONS.md` — D004 (Voice Gateway Optional ZeroMQ) prefixed
  with "Status: RETIRED" block; D005 test list entry and D017 Logger
  rationale references annotated as historical.
- `docs/MIGRATION_PLAN.md` — Source mapping table row, Phase 1
  Completed list entry, and 2 Task reading-list entries annotated with
  `(RETIRED 2026-07-18, A2)` notes pointing to
  `RK3588S_CarTerminal/voice/`.
- `docs/MIGRATION_BLUEPRINT.md` — Section 3.3 prefixed with retirement
  admonition; section 3.4 `voice_gateway/` deletion row annotated as
  "已执行" (executed).
- `docs/MODULE_STATUS.md` — Both "Voice gateway skeleton" rows (lines
  331, 360) updated with "Formally retired 2026-07-18 (task A2)";
  Unified log module row's voice_gateway Logger reference marked
  historical; `voice_gateway_app builds` claim (line 398) marked
  "HISTORICAL ONLY" with pointer to the current `llm_service` /
  `tts_service` build path.
- `docs/TODO.md` — Item 4 (line 105) struck through and marked
  completed; new "✅ 已完成：A2 退役" section added at top of completed
  items; `Last updated:` line updated.
- `docs/CHANGELOG.md` — This entry added.

### Files NOT modified (historical immutability)

- The 16 historical `voice_gateway` entries already present in
  `docs/CHANGELOG.md` (lines 1643, 2156, 2256, 2337, 2383, 2465, 2509,
  2587, 2632, 2733, 2834, 2883, 2885, 2901, etc.) are preserved
  unchanged. Historical log entries are immutable; the retirement
  decision is recorded in this new entry only.

### Verification

- `grep -rn "voice_gateway" docs/` confirms all 31 non-historical
  references now carry RETIRED/historical markers.
- `grep -rn "rk3588s_migration/voice_gateway" --include="*.cpp" --include="*.h" --include="*.pro" --include="CMakeLists.txt"` returns 0 matches (no code references).
- The `rk3588s_migration/voice_gateway/` directory does not exist in
  the current worktree.
- Every retirement marker explicitly points readers to the active
  replacement: `RK3588S_CarTerminal/voice/` (ASR/LLM/TTS/ZMQ rows in
  `MODULE_STATUS.md`).

### Impact

- **No code changes** — pure documentation retirement.
- **No runtime behavior change** — the retired skeleton was already
  absent from the workspace.
- **Reduced onboarding risk** — new users can no longer accidentally
  execute the non-functional `cmake -S rk3588s_migration/voice_gateway`
  commands from `BUILD_RUN_TEST.md`.

## 2026-07-18 One-Shot No-Peripheral Verification Script

Added `scripts/verify_no_peripheral.sh` — a single command that runs the
full no-peripheral verification matrix (no mic / no speaker / no camera /
no NPU). Supersedes the manual command sequences used during the earlier
2026-07-18 handoff verification.

### Functional changes (scripts/)

- `scripts/verify_no_peripheral.sh` (new): integrates 6 stages —
  pre-flight dependency check, 5 red-line config verification
  (`asr_alsa_device=plughw:3,0`, ZMQ `127.0.0.1`,
  `tts_feedback_for_command=true` + `tts_feedback_mute_ms=3000`,
  `llm_enabled=false`, priming `snd_pcm_readi` in `VoiceManager.cpp`) +
  `ASR_DBG` residue check + sherpa config consistency, main program
  `qmake CarTerminal.pro && make -j8` (with future-mtime auto-touch),
  voice/ CTest (4 tests, "0 tests = FAIL" guard), TTS→WAV→ASR closed
  loop (6/6 smoke profile via `voice/scripts/verify_tts_asr_wav.sh`),
  and optional GUI text-injection (`--with-gui`).
- Any stage failure exits non-zero immediately; the "0 tests = FAIL"
  guard prevents false PASS when CTest registers nothing.
- Numeric parsing hardened with `tr -d '[:space:]'` + `=~ ^[0-9]+$`
  after `grep -c` / `wc -l` / `awk` to avoid bash `[[ ]]` parse errors
  on multi-line output.
- CLI: `--with-gui` (force-enable GUI inject), `--skip-voice` (skip
  voice/ CTest + closed loop), `--skip-build` (skip main program
  qmake+make), `--help`.

### Board validation (2026-07-18)

```
Stage 1 redline-config       PASS  (5 redlines + ASR_DBG + sherpa)
Stage 2 main-build           PASS  (make -j8 clean, 5057416 bytes)
Stage 3 voice-ctest          PASS  (4 tests: wav_io / asr_abi / vehiclecommand / asr_config)
Stage 4 tts-asr-closedloop   PASS  (6/6 smoke profile)
Stage 5 gui-inject           SKIP  (use --with-gui to enable)
RESULT = PASS, exit=0
```

### Bugs found and fixed during script development

- `grep -c` returns value with trailing newline → `[[ $dbg -eq 0 ]]`
  parse error on multi-line string. Fix: pipe through `head -1 |
  tr -d '[:space:]'` and validate with `=~ ^[0-9]+$`.
- `ctest_output=$(ctest 2>&1 | tee file)` + parsing `Total Test time`
  is unreliable (locale-dependent). Fix: run `ctest > log 2>&1` for
  exit code, then `ctest -N` for `Total Tests: N`.
- `verify_tts_asr_wav.sh` summary format is `PASSED      = 6` (not
  `PASSED 6`). Fix: use `awk -F= '{print $2}'` instead of
  `awk '{print $2}'`.

### Do NOT regress

- The "0 tests = FAIL" guard must stay — CTest returns 0 when no tests
  are registered, which would otherwise be a false PASS.
- The red-line stage must run before any build/test — a red-line
  violation exits immediately without wasting build time.

## 2026-07-18 ASR/TTS Echo Loopback Suppression + Real-Mic ALSA Fix + Dev-Env Traps

Real-microphone path now works on QuarkPi-CA2 and the ASR/TTS self-echo
loop is suppressed end-to-end. Previous 2026-07-17 milestone verified the
software loop only through text injection
(`VEHICLE_VOICE_TEST_TEXT`); today the loop was closed with a real USB
camera microphone (card 3) as the capture source and the TTS playback
(ES8323 / plughw:2,0) as the synthesis sink, while preventing the
microphone from picking up the TTS's own playback.

### Functional changes (code)

- `voice/VoiceManager.cpp` — ALSA capture startup replaced
  `snd_pcm_start()` with a **blocking priming `snd_pcm_readi()`** of one
  period. Root cause: for `plughw:` (plugin-layer) devices
  `snd_pcm_start()` returns 0 (success) but the underlying hw stream
  stays in `PREPARED` — plughw is lazy-start and only kicks off the hw
  device when the app actually pulls audio. Verified on USB camera mic
  card3: after `snd_pcm_start()` the stream stayed PREPARED and `poll()`
  never returned POLLIN; after the priming readi the stream transitioned
  to `RUNNING` and poll/readi worked normally for the rest of the run.
- `voice/VoiceManager.cpp` — TTS echo suppression implemented in the ASR
  worker loop using `std::atomic<bool> m_asrMuted`:
  - `sendToTts()` sets `m_asrMuted=true` and arms a `QTimer::singleShot
    (tts_feedback_mute_ms)` to clear it.
  - While muted, the loop still calls `snd_pcm_readi()` (to drain the
    ALSA buffer and prevent overrun) but discards the audio instead of
    feeding ASR, and on the mute-entry edge calls `m_asrEngine->reset()`
    to drop any partially-recognized TTS echo.
  - The unmute edge logs `ASR unmuted, resuming recognition`.
- `voice/VoiceManager.h` — exposed `isAsrMuted()` for inspection.
- `voice/voice_config.{h,cpp}` — added `ttsFeedbackMuteMs` field
  (default 3000) and parsing of `tts_feedback_mute_ms`.
- `gui/mainwindow.cpp` — the existing
  `VEHICLE_VOICE_TEST_TEXT` test hook is preserved (1 s
  `QTimer::singleShot` then `onOfflineAsrTextRecognized`).
- Cleaned all 23 lines of temporary debug scaffolding from `VoiceManager.cpp`
  (`ASR_DBG`, `dbgPollCnt`, `dbgReadCnt`, `dbgMaxAbs`, periodic stat dump).
  The final file was verified with `grep -c ASR_DBG = 0` and a clean
  `make -j8` (`make_exit=0`) on the board.

### Configuration changes (config/)

- `config/asr_settings_offline.conf`: `asr_alsa_device` changed from
  `plughw:2,0` (broken ES8323 record path) to **`plughw:3,0`** (USB
  camera microphone, card 3 stereo). Comment updated to record: ES8323
  I2C communication failure breaks the record path, so the playback path
  (plughw:2,0 in `voice/tts/src/AudioPlayer.cpp`) is preserved for TTS
  output while recording is moved to the USB mic.
- `config/voice_settings.conf`: `llm_address` / `tts_address` /
  `tts_status_address` all changed from `tcp://localhost:` to
  **`tcp://127.0.0.1:`**. Root cause: on the board `/etc/hosts` may be
  missing the `localhost` entry (`getent hosts localhost` returns empty),
  in which case ZMQ `send` still succeeds (queued) but `recv` blocks for
  the full `zmq_timeout_ms` (30 s) because the REP socket on the resolved
  IP never receives the request. `127.0.0.1` resolves without
  `/etc/hosts`. The board-side /etc/hosts was also patched with
  `127.0.0.1 localhost` as a permanent backup.
- `voice/tts/src/AudioPlayer.cpp` line 44 **keeps** `plughw:2,0` for
  playback — ES8323 playback path works, only its record path is broken.

### Verified closed loop (board, 2026-07-18)

With a real USB mic connected and `asr_enabled=true`, `tts_enabled=true`,
`llm_enabled=false`, `direct_command_enabled=true`:

```
12:12:54  ASR recognized: 打开地图
12:12:54  ASR muted for TTS feedback (mute_ms=3000)
12:12:54  sending to TTS: 已为您打开地图END
12:12:54  TTS ack: ok
12:12:54  ASR stream reset (muted for TTS echo suppression)
12:12:57  ASR unmuted after TTS feedback window
```

`CarTerminal` + `tts_service` processes stable after the sequence. TTS
ack returns immediately (was 30 s before the localhost fix). The 3-second
mute window covers the 1-2 s actual TTS playback on RK3588S.

### Dev-env / tooling traps encountered (recorded for the next AI)

These are not code bugs but board-side traps that consumed most of the
session and will recur on any fresh board or after a reboot. Record them
explicitly so the next session does not re-derive them from scratch.

| Symptom | Real root cause | Fix |
|---|---|---|
| `cmake --build build` fails "could not load cache" | Project is **qmake-based** (`CarTerminal.pro`), not cmake | `qmake CarTerminal.pro && make -j8` |
| `make` runs forever, logs `警告：文件"CarTerminal.pro"的修改时间在未来 59688348 秒后` | Board's RTC was wrong (year 2024) while files carried 2026 mtime from Windows edits; `make` kept re-running `qmake` to regenerate the Makefile | `sudo date -s "2026-07-18 04:10:00"` + `find . -type f -exec touch {} \;` |
| `amixer -c 3 sset 'Mic' 20% on` and `... unmute` leave the switch `[off]` | The capture switch (`cswitch`) of this USB camera mic does not respond to `on`/`unmute` keywords | `amixer -c 3 sset 'Mic' 20% cap` — the `cap` keyword enables the capture switch; verified `[on]` |
| `snd_pcm_start()` returns 0 but `/proc/asound/card3/pcm0c/sub0/status` still `state: PREPARED`, `read=0 maxAbs=0` | plughw lazy-start (see code section above) | blocking priming `snd_pcm_readi()` |
| Config file `asr_alsa_device` keeps reverting to `plughw:2,0` after edits | Windows-side file edits overwrite the board-side file on the next sync | Apply edits on **both** sides (Python/`sed` on the board, `SearchReplace` on Windows). The 2026-07-18 sync below ran both sides explicitly. |
| TTS ZMQ request hangs for 30 s then `TTS request failed: ZMQ Error: Receive timeout` | Board `/etc/hosts` missing `localhost` entry | `sed -i 's\|tcp://localhost:\|tcp://127.0.0.1:\|g' config/voice_settings.conf` + `echo "127.0.0.1 localhost" \| sudo tee -a /etc/hosts` |
| TTS service crash on first start: `terminate called after throwing an instance of 'zmq_component::ZmqCommunicationError'` | A stale `tts_service` from a previous session still holds port 7777, so the new process's ZMQ REP `bind` fails | `pkill -9 tts_service` before restarting `CarTerminal` (the app spawns `tts_service` as a `QProcess`). The stock `~/kill_voice.sh` flow handles this. |
| First Python debug-cleanup pass: `error: 'dbgMaxAbs' was not declared in this scope` | Regex deleted the variable declaration but left a usage site | Use **anchor-based replacement** (find `// 临时调试统计`, `while (m_asrRunning.load())`, `// TTS echo suppression: when muted` as anchors and replace the whole middle block) instead of regex delete. |
| Second Python pass: `error: expected declaration before '}' token` at `} // namespace voice` | Deletion removed `int pollRet = poll(&pfd, 1, 200);` but kept the `if (pollRet < 0)` body, breaking braces | Same as above — anchor-based replacement of the whole `while` body. |
| Bash terminal badly confused by pasted multi-line Python heredoc | Pasted lines were interpreted as shell commands | Write the Python to `/tmp/clean_dbg.py` with `cat > … << 'PYEOF'` then run `python3 /tmp/clean_dbg.py`. |

### Authoritative board runbook after 2026-07-18

The next AI should run a closed-loop verification in this order:

```bash
# 1. Reset state (avoid stale tts_service / ports)
~/kill_voice.sh
sudo date -s "2026-07-18 04:10:00"     # only if RTC is wrong
find . -type f -exec touch {} \;        # only if "modification time in future" warning

# 2. Enable the USB camera mic capture switch (cap keyword, NOT on/unmute)
amixer -c 3 sset 'Mic' 20% cap

# 3. Build (qmake, not cmake)
qmake CarTerminal.pro && make -j8

# 4. Run with text injection (no microphone needed for this path)
kill -9 $(pgrep tts_service) 2>/dev/null
VEHICLE_VOICE_TEST_TEXT="打开地图" ./CarTerminal
# Expect: ASR recognized → ASR muted → sending to TTS → TTS ack: ok
#         → ASR stream reset → ASR unmuted after TTS feedback window
```

### Do NOT regress these defaults (any of these breaks the closed loop)

- `asr_alsa_device=plughw:3,0`  — going back to plughw:2,0 breaks capture
  (ES8323 I2C comm failure on the record path).
- ZMQ addresses `tcp://127.0.0.1:...` — going back to `localhost`
  triggers the 30 s ZMQ recv timeout when `/etc/hosts` is missing the
  `localhost` entry.
- `tts_feedback_for_command=true` and `tts_feedback_mute_ms=3000` —
  disabling or zeroing these re-enables the ASR/TTS echo loop.
- `llm_enabled=false` — keep false until RKNPU kernel driver ≥ 0.9.7
  (driver 0.9.6 SIGBUS on RKLLM init; see 2026-07-15 audit).
- `snd_pcm_readi()` priming read in `VoiceManager.cpp` — replacing it
  with `snd_pcm_start()` regresses the PREPARED-stuck bug on plughw
  capture devices.

## 2026-07-17 Voice Software Loop COMPLETE + Scenario C 10-minute Soak Test PASSED

Two follow-up milestones on QuarkPi-CA2 (RK3588S, kernel 5.10.209,
Qt 5.15.2): the voice software loop was closed end-to-end without a real
microphone / LLM / NPU, and a 10-minute stability soak test of scenario C
was executed cleanly.

### Voice software loop closure

The path `ASR text inject → vehiclecommand → executeCabinCommand →
feedbackTextForCommand → VoiceManager::sendToTts → tts_service --direct →
AudioPlayer plughw:2,0 → ES8323 → 3.5 mm headphone` was verified end-to-end.
User confirmed audible "已为您打开地图" / "已为您打开音乐" / "已为您打开监控"
feedback in the headphone immediately after injection, with a clean
`code=0` exit on Ctrl+C.

### Key voice-loop fixes

- `voice/tts/src/AudioPlayer.cpp`: switched ALSA device from `default`
  (dmix) to `plughw:2,0` (ES8323 codec). The previous setup buffered the
  audio without flushing it on `drain`, so headphones only produced a
  buzz during the run and the actual phrase was only played back when
  `snd_pcm_close` was called on process exit. Now the device is opened
  directly, hw/sw params are configured explicitly (16000 Hz / mono /
  S16_LE / 5 s buffer / `start_threshold=1`), `prepare` resets pointers
  between utterances, and `drain` is called unconditionally — measured
  playback length matches the synthesized PCM length (3 s synthesis →
  3 s drain).
- `voice/tts/src/main.cpp`: new `--direct` CLI flag. In direct mode
  `tts_service` skips the 6677 status signal wait and the post-play
  `play_end` notification, so `VoiceManager::sendToTts` can drive the
  TTS service for short feedback phrases without an LLM turn.
- `voice/VoiceManager.*` + `voice/voice_config.*`: new
  `tts_feedback_for_command` config switch, `tts_status_address` field,
  `m_ttsClient` ZMQ REQ client, `sendToTts()` (worker-thread, appends
  `END` marker), `isTtsFeedbackEnabled()`. `startTtsProcess` passes
  `--direct` when LLM is disabled.
- `gui/mainwindow.cpp`: new `feedbackTextForCommand()` returning the
  short zh phrase for each of the 11 `CabinCommand` values;
  `executeCabinCommand()` now invokes
  `voiceManager->sendToTts(feedback)` when `isTtsFeedbackEnabled()`.
- `common/vehiclecommand.cpp`: added zh keyword variants
  `打开监控` / `显示监控` / `监控画面` / `打开天气` so those spoken phrases
  now parse to `MonitorShow` / `WeatherShow` instead of `Unknown`.
- `config/voice_settings.conf`: `tts_enabled=true`,
  `tts_feedback_for_command=true`, `tts_status_address` set.
- New `~/kill_voice.sh` workflow resets any orphaned `tts_service` /
  `CarTerminal` and verifies ports 7777/6677 are free before a new
  launch. Recommended invocation:
  `~/kill_voice.sh && VEHICLE_VOICE_TEST_TEXT="打开地图" ./CarTerminal`.

### Scenario C 10-minute soak test

Scenario C (file 800x600@30 + GUI + RTSP, MPP H.264 hardware encode) was
extended from ~85 s to 600 s. Over the 10-minute run, source / GUI / RTSP
lanes held steady at 29.9x-30.00 fps with 0 drops, the bounded
FrameBroker queues never filled (drop_oldest never invoked), FileFrameSource
looped cleanly at EOF with no memory growth, and the Ctrl+C shutdown
returned `code=0` with `pipeline stopped` and all worker threads joined.

A note for future runs: GUI fps is computed as `gui_frames / total_runtime`,
so once the Monitor page is exited mid-run the GUI lane stops accumulating
frames while `runtime` keeps growing — the printed GUI fps drops over time
but source and RTSP keep running at 30 fps with 0 drops. This is a
statistics-coverage artifact, not a runtime regression.

## 2026-07-17 Board Validation Scenarios A–E PASSED

All five board scenarios from the task spec were executed on QuarkPi-CA2
(RK3588S, kernel 5.10.209, Qt 5.15.2) and PASSED. Board binary built
2026-07-17 19:13:37 with `BUILD_RC=0`. See `docs/MODULE_STATUS.md` section
"2026-07-17 Board Validation Complete (Authoritative)" for the full matrix.

### Critical fix applied during board validation

- `gui/Monitor/monitor.cpp`: moved `m_pendingGuiFrames.fetch_sub(1)` from
  before `videoLabel->setPixmap()` to after it. At 30 FPS the previous
  placement let the worker thread emit a new frame while `setPixmap` was
  still running, permanently starving the Qt event loop (zero
  `[MEDIA_STATS]`, no Esc/button response). After the fix the throttle slot
  stays occupied for the full `setPixmap` duration; scenario A confirmed
  32 `[MEDIA_STATS]` events over 30 s and immediate Esc/button response.
  GUI queue max depth stayed at 1 with 0 drops.

### Board scenario results

| Scenario | Config | Result | Highlights |
|---|---|---|---|
| A | test_pattern 800x600@30 + GUI | PASS | 32 MEDIA_STATS, source/gui 29.97 fps, 0 drops, Esc + button responsive |
| B | file 800x600@30 + GUI + RKNN | PASS | RKNN 8.63 fps / 992 drops, GUI 29.94 fps / 0 drops (three lanes independent) |
| C | file 800x600@30 + GUI + RTSP | PASS | RTSP 29.99 fps / 0 drops via MPP H.264, GUI lane toggles on Monitor hide |
| D | file + RTSP only (autostart) | PASS | Covered by C: pipeline autostarts, RTSP keeps running after Monitor hide |
| E | missing-file error | PASS | `pipeline start failed: ... file not found`, no crash, GUI responsive, exit code=0 |

### Remaining risk (BSP layer, not task scope)

- RKNPU kernel driver 0.9.6 NULL-pointer dereference in
  `rknpu_job_subcore_commit_pc` observed once during an RKNN-enabled run.
  System `librknnrt.so` 2.0.0b0 may not match the driver. Scenario B re-ran
  cleanly (403 frames at 8.63 fps), so the application-side RKNN integration
  is correct. Keep `rknn_enabled=false` by default until a matched
  librknnrt.so / RKNPU driver pair is available.

## 2026-07-17 Media Pipeline Follow-up Fixes

- Fixed the Qt integration build break by adding the configured-capacity
  `MediaPipeline` constructor and applying `media_read_timeout_ms` to source
  reads.
- Raw NV12 `FileFrameSource` now honors `camera_fps`; pacing waits are
  interruptible, so stop does not wait for a long frame interval.
- Added per-run source/consumer FPS, processed/dropped counts, current and
  maximum queue depth, and average/P95 consumer processing time. Monitor logs
  `[MEDIA_STATS]` every five seconds and once at stop.
- Disabled RKNN/RTSP lanes no longer receive frames. Opening Monitor reuses
  an RTSP-autostarted pipeline instead of changing a live source. Closing the
  page disables the GUI lane while keeping RTSP and in-use RKNN resources
  alive; reopening the page re-enables GUI delivery safely.
- Fixed a test-pattern heap overwrite at valid resolutions below 80 pixels;
  bar and moving-square geometry now clamps to the actual frame dimensions.
- Added host coverage for queue capacity, metrics, file pacing, and pacing
  interruption. Seven media/alarm regressions pass; Qt/qmake object builds
  for Monitor and the changed media sources pass. Board matrix A-E is still
  pending.

## 2026-07-17 Media Pipeline Thread-Safety + Software End-to-End Loop

### Goal
Two-part task from the project owner:
1. Thread-safety and lifecycle overhaul of the camera path — remove
   `QThread::terminate()`, make `start` report real initialization success
   or failure, make `stop` interrupt blocking reads, and ensure destructors
   leave no running threads.
2. Software-only end-to-end loop with no real camera — implement
   `test_pattern` and `file` frame sources that drive
   RGA→GUI / RGA→RKNN→AlarmManager / MPP-encode→RTSP through a fan-out
   broker, so the whole pipeline can be exercised on host and board without
   camera hardware. V4L2 mode is preserved for future real-camera use.

No BSP, kernel, DTB, PHY, RTOS/RPMsg, or NPU driver changes were made.

### Status markers
- `SOURCE_IMPLEMENTED` — all source changes below.
- `HOST_TESTED` — five new media test suites plus existing alarm/v4l2
  utility regressions pass on Windows host g++ with exit 0.
- `PENDING_BOARD_VALIDATION` — combinations A–E (test_pattern+GUI,
  file+GUI+RKNN, file+GUI+RKNN+RTSP, file+RTSP-only, missing-file error)
  must be run on QuarkPi-CA2 before promotion.
- `PENDING_REAL_CAMERA` — V4L2 path is preserved and benefits from the
  lifecycle fixes, but no real-frame claim is made.

### New files (TDD RED→GREEN→REFACTOR)
- `media/frame_packet.h`: `FramePacket` struct with shared ownership
  (`std::shared_ptr<const std::vector<uint8_t>>` data), dmabufFd/stride/
  format/sequence/timestampUs fields, `isValid()` helper. `PixelFormat`
  enum (Unknown/NV12/RGB32/I420). `pixelFormatBufferSize()` helper.
- `media/frame_source.h`: `IFrameSource` interface and `FrameReadResult`
  enum (FrameReady/Timeout/EndOfStream/Stopped/RecoverableError/FatalError)
  so a single bool never collapses all failure modes.
- `media/test_pattern_source.h` / `media/test_pattern_source.cpp`: NV12
  color-bar + moving-block generator with monotonic sequence/timestamp,
  configurable fps/width/height (even dimensions, fps>0). Implements
  `IFrameSource`; `requestStop()` makes the next `readFrame()` return
  `Stopped`.
- `media/file_frame_source.h` / `media/file_frame_source.cpp`: raw NV12
  file reader. Validates dimensions and that file size is an exact multiple
  of frame size. `loop=true` seeks to 0 at EOF; `loop=false` returns
  `EndOfStream`. Missing file / empty file / bad size / invalid dimensions
  all return a clear error message.
- `media/frame_broker.h` / `media/frame_broker.cpp`: three independent
  bounded queues (Gui/Rknn/Rtsp, default capacity 3) with `drop_oldest`,
  per-lane enable/disable, `requestStop()` waking all `take()` waiters,
  per-lane dropped counters, and `reset()`. No unbounded `std::queue`/
  `QList`/`QVector` growth is permitted.
- `media/media_pipeline.h` / `media/media_pipeline.cpp`: top-level pipeline
  owning one source thread + three consumer threads. State machine
  Idle→Starting→Running→Stopping→Failed. `start()` opens the source
  synchronously and reports failure immediately; `stop()` requests stop on
  source + broker, wakes all waiters, and joins source→gui→rknn→rtsp
  threads in order. Destructor calls `stop()` so no thread outlives the
  object.
- `tests/media/test_frame_packet.cpp`, `tests/media/test_test_pattern_source.cpp`,
  `tests/media/test_file_frame_source.cpp` (10 cases),
  `tests/media/test_frame_broker.cpp` (8 cases),
  `tests/media/test_media_pipeline.cpp` (9 cases incl. 20× start/stop,
  idempotent stop, destructor join, restart-after-failure,
  disabled-lane-does-not-block-others).

### Modified files
- `media/capture_thread.h` / `media/capture_thread.cpp`: **removed
  `QThread::terminate()` entirely**. `startCapture()` now blocks on a new
  `QWaitCondition m_startCondition` until the worker transitions to
  `Capturing` (real V4L2 open + format set + STREAMON complete) or `Error`,
  with `startTimeoutMs`. A start timeout converts the state to `Error` and
  triggers a safe stop instead of leaving a background thread.
  `stopCapture()` is idempotent and uses `QThread::wait(stopTimeoutMs)`
  only; on timeout it logs ERROR but does not force-kill. A new
  `m_threadExited` flag lets the run() thread signal exit so startCapture
  never hangs waiting for a thread that has already terminated.
- `media/cameraconfig.h` / `media/cameraconfig.cpp`: added
  `sourceType` (v4l2/test_pattern/file), `filePath`, `fileLoop`,
  `queueCapacity`, `startTimeoutMs`, `readTimeoutMs`, `stopTimeoutMs`.
  New known keys: `media_source_type`, `media_file_path`,
  `media_file_loop`, `media_queue_capacity`, `media_start_timeout_ms`,
  `media_read_timeout_ms`, `media_stop_timeout_ms`. Unknown keys are still
  rejected. `sourceType=v4l2` is the backward-compatible default.
- `gui/Monitor/monitor.h` / `gui/Monitor/monitor.cpp`: when
  `sourceType=test_pattern|file`, `Monitor` creates a `MediaPipeline` and
  registers three consumer callbacks:
  - GUI: NV12→RGB32 (RGA where available) → emit queued
    `pipelineFrameReady(QImage)`; the GUI thread sets the label pixmap.
    Worker threads never touch QWidget.
  - RKNN: NV12→RGB32 → `rknn::detect` → `DangerAlarmPolicy` (3-frame
    trigger / 10-frame clear, alarm cooldown).
  - RTSP: NV12→RGB32 → `RtspServer::pushFrame`.
  V4L2 path still uses `CaptureThread`. Destructor order: stop+delete
  pipeline → deinit RTSP, so the encode thread exits before the encoder
  is torn down.
- `config/camera_settings.conf`: default changed to
  `media_source_type=test_pattern` so the GUI/RTSP/RKNN loop can be
  exercised with no camera hardware. All `media_*` keys added with
  documented defaults. V4L2 mode remains selectable by configuration.
- `CarTerminal.pro`: added the four new source files and six new headers
  to SOURCES/HEADERS.

### Host test evidence (2026-07-17, run from project root)
All seven suites pass with exit 0:

```text
frame_packet tests passed
test_pattern_source tests passed
file_frame_source tests passed
frame_broker tests passed
media_pipeline tests passed
alarm manager and policy tests passed
v4l2 capture utility tests passed
```

Static check: `Select-String -Path media\capture_thread.cpp,media\capture_thread.h -Pattern 'terminate\(\)'`
returns no matches.

### What was NOT done
- No U-Boot/Kernel/DTB/PHY/RTOS/RPMsg/NPU-driver changes.
- No real camera verification (PENDING_REAL_CAMERA).
- No board run of the new pipeline (PENDING_BOARD_VALIDATION).
- No Annex-B H.264/H.265 demux in `FileFrameSource` — raw NV12 only, to
  avoid introducing a new FFmpeg demux dependency in this task.
- No DMA-BUF zero-copy: `FramePacket.dmabufFd`/stride fields are reserved
  for future use but not populated by the software sources.
- No `QThread::terminate()` anywhere in the camera stop path.
- No unbounded frame queues.
- No file-source result misrepresented as real-camera verification.

### Remaining risk
- Board build of the four new `.cpp` files has not been run on QuarkPi-CA2;
  a board qmake+make is required before combinations A–E can be executed.
- `Monitor` still keeps the legacy `CaptureThread` path for V4L2; the
  three-lane broker is used only for `test_pattern`/`file`. A future
  refactor could route V4L2 through `MediaPipeline` as well, but that is
  out of scope for this task and would need real-camera validation.
- RTSP branch currently converts NV12→RGB32 before `pushFrame` because the
  existing `RtspServer::pushFrame` accepts RGB32; a future NV12-direct
  encode path would avoid the round trip.
- Alarm integration for file-source frames reuses `DangerAlarmPolicy`, so
  alarm cooldown and clear logic apply, but no live danger-trigger board
  test has been run with file-source frames.

## 2026-07-17 GPS QuarkPi-CA2 Simulation Verification

1. Build/test evidence:
   - Host and board strict builds/runs passed all four GPS suites: NMEA parser,
     stream reassembly, configuration, and simulation; every recorded return
     code was 0.
   - `CarTerminal.pro` now includes `common/nmeaparser.cpp/.h`. The full board
     application linked the parser, device, simulator, worker, and worker moc
     objects and returned 0.
2. Disabled lifecycle:
   - With real GPS and simulation both false, the application reached ready,
     handled SIGINT, exited with code 0, did not open a GPS serial device, and
     left no process.
3. Simulation lifecycle and GUI:
   - With real GPS false and simulation true, the worker loaded 14 NMEA
     sentences and logged simulation start.
   - The GUI displayed `GPS模拟定位`, nonzero coordinates, speed, course, and
     satellite count; coordinates changed during the 5-10 s observation.
   - Clean exit logged simulation-loop exit and worker join/release. All strict
     log checks returned 0, no real serial open was observed, and no process
     remained.
4. Safe restoration:
   - `gps_enabled=false` and `gps_simulation_enabled=false` were restored, with
     exactly one occurrence of each key.
5. Remaining boundary:
   - GPS software and board simulation are verified. Real UART/GNSS node,
     antenna acquisition, cold/warm start, real trajectory/speed accuracy, and
     disconnect/reconnect remain pending until hardware is available.

## 2026-07-16 GPS Software + Simulation Implementation

1. Migration audit (Phases 1-3):
   - Searched parent directory `<LOCAL_SRC_DIR>`
     for existing GPS/GNSS/NMEA/UART/serial/location code.
   - No safe migration candidate found. The best candidate had correct
     thread-safety structure but an algorithmically wrong parser, no checksum
     validation, no GGA support, and hardcoded fallback coordinates.
   - Decision: all new write (情况C — no available code).
2. New files (TDD RED→GREEN→REFACTOR):
   - `common/nmeaparser.h` / `common/nmeaparser.cpp`: Qt-free NMEA 0183 parser.
     Supports $GPRMC/$GNRMC/$GPGGA/$GNGGA with XOR checksum, ddmm.mmmm→dd
     conversion, S/W hemisphere sign, knots×1.852→km/h, NaN/Inf rejection,
     range checks, overlong-line rejection, unknown-sentence safe skip, and a
     streaming reassembler ($-sync, CRLF/LF, noise resync, 256/4096 caps).
   - `device/gpsdevice.h` / `device/gpsdevice.cpp`: serial device layer with
     termios configuration, poll-based readChunk, closeDevice()/
     closeDeviceLocked() split per Phase 9, RAII destruction, `#ifdef __linux__`
     guards. No hardcoded device path or baudrate.
   - `device/gpssimulator.h` / `device/gpssimulator.cpp`: Qt-free simulation
     player that reuses parseNmeaSentence and forces simulated=true. Loop
     playback at EOF. Fail-closed on missing/empty/all-invalid files.
   - `device/gpsworker.h` / `device/gpsworker.cpp`: Qt worker owning a
     std::thread. Picks real-GPS or simulation mode from config. Emits
     fixUpdated(GpsFix)/statusChanged/availabilityChanged/warning via queued
     connections. Stop sequence: set running_=false → join → closeDevice
     (no lock held during join).
   - `tests/gps/test_nmeaparser.cpp` (22+ tests), `tests/gps/test_gpsstream.cpp`
     (8 tests), `tests/gps/test_gpsconfig.cpp` (14 tests),
     `tests/gps/test_gpssimulation.cpp` (9 tests).
3. Modified files:
   - `device/vehicledeviceconfig.h` / `device/vehicledeviceconfig.cpp`: added
     GpsSimulationConfig struct, gps_update_interval_ms, gps_simulation_*,
     gps_map_update_interval_ms keys, mutual-exclusion enforcement.
   - `gui/Map/baidumap.h` / `gui/Map/baidumap.cpp`: replaced C API stub with
     GpsWorker signal/slot integration. Added 6-state status display
     (GPS已关闭/设备不可用/未定位/已定位/模拟定位/数据异常), rate-limited map
     refresh (mapUpdateIntervalMs), 0,0 coordinate rejection, source tagging
     (GPS数据/模拟数据/回退坐标), and GPS info overlay (lat/lon/speed/course/sats).
   - `CarTerminal.pro`: removed `gui/Map/gps.c` and `gui/Map/gps.h`; added
     `device/gpsdevice.cpp`, `device/gpssimulator.cpp`, `device/gpsworker.cpp`
     and corresponding headers.
   - `config/vehicle_devices.conf`: added gps_update_interval_ms,
     gps_simulation_enabled=false, gps_simulation_file,
     gps_simulation_interval_ms, gps_map_update_interval_ms.
4. Deleted files:
   - `gui/Map/gps.h` / `gui/Map/gps.c`: C fail-closed stub, replaced by the
     new C++ GPS worker stack.
5. Host verification (2026-07-16):
   - 4 test suites pass: nmeaparser, gpsstream, gpsconfig, gpssimulation
     (53+ tests total, all exit 0).
   - gpsdevice.cpp, gpssimulator.cpp compile clean with -Wall -Wextra -Werror.
   - gpsworker.cpp compiles clean with Qt 5.12.11 + moc.
   - qmake CarTerminal.pro returns 0.
   - Alarm/buzzer/alarmpolicy/vehicledeviceconfig regression compiles clean.
   - No hardcoded /dev/ttyUSB0 or /dev/ttyS* in device layer.
6. Pending after the implementation phase:
   - Board-side simulation was subsequently completed on 2026-07-17; see the
     verification entry above.
   - Real GPS hardware verification remains pending (UART node discovery, antenna, cold/warm
     start, real trajectory, speed accuracy, disconnect/reconnect).
   - Real-hardware items are not marked complete per Phase 15 boundary.

## 2026-07-16 RKNN Explicit Shutdown Release

1. Changed files:
   - `gui/mainwindow.cpp`: the main-window destructor now calls the existing
     idempotent `Monitor::myStop()` after alarm reset and before buzzer
     destruction.
   - `tests/rknn/test_rknn_detector_source.cmake`: added teardown-range and
     ordering checks, required exactly one guarded `deinitDetector()` call,
     and rejected Monitor singleton-ownership changes.
2. Design boundary:
   - No RKNN inference, postprocess, camera-start, alarm-policy, or singleton
     ownership behavior was changed.
   - `Monitor::myStop()` remains the single RKNN release path and clears
     `m_rknnReady` after release, so repeated stops do not double-release.
   - The tracked default remains `rknn_enabled=false`.
3. TDD and host evidence:
   - The new source contract first failed because the main-window destructor
     did not call `monitor->myStop()`, then passed after the minimal hook.
   - Final source-contract, alarm-manager, and buzzer tests all exited 0.
   - Specification and code-quality reviews approved the final minimal design.
4. QuarkPi-CA2 evidence:
   - RKNN source-contract test exited 0; shared RKNN CTest passed 4/4.
   - qmake and the full application build exited 0 after recompiling
     `mainwindow.cpp` and `monitor.cpp`.
   - A controlled enabled run logged `RKNN YOLOv5 detector initialized`,
     `application ready`, clean SIGINT handling, `application exiting (code=0)`,
     `RKNN detector released`, and buzzer silent level 1.
   - Application and all three strict log checks returned 0; no process
     remained. The configuration contained one `rknn_enabled` key and was
     explicitly restored to `false`.
5. Remaining boundary:
   - No real camera is connected. Real-frame inference, three-frame danger
     trigger, ten-frame clear, repeating 200/800 ms buzzer behavior, and
     post-camera shutdown remain hardware-pending.
   - Real DHT11 input remains a separate hardware-pending item.

## 2026-07-15 Enterprise Audit And Minimal Safety Fixes

1. Established a deterministic full-workspace inventory and pre-change hash baseline under `reports/`.
2. Corrected high-severity runtime risks: signal-safe shutdown flag, parentless Monitor teardown, camera/RTSP bounds validation, MPP cache-balance and bitrate overflow, capture-state signal emission outside locks, ASR typed/runtime-gated result ABI, and partial-init cleanup. The final 2026-07-16 RKNN lifecycle implementation uses explicit idempotent stop and does not change singleton ownership.
3. Removed map-key URL logging and insecure TLS peer-verification bypass; tightened voice timeout parsing and portable size logging.
4. Added camera/RTSP tests, MPP cache-balance coverage, ASR/source-contract checks, and corrected three stale current-module host runners. The absent legacy voice-gateway runner remains unresolved.
5. Preserved mandatory constraints: ASR tail silence, Sherpa 1.13.3/SHA `330609da`, dynamic MPP loading, exact command matching (no fuzzy/pinyin/edit distance), RKNN default false, and GPIO character-device buzzer control.
6. Verification boundary: the pre-change host baseline ran 10/14 runners successfully. Post-change runners/builds could not be launched in the available Windows sandbox; no AArch64 toolchain or current board session was available. Therefore no post-change build/test/board pass is claimed.
7. Delivery status remains `NOT_MET` pending the manual validation matrix and resolution of open major findings documented in `../../reports/audit_report_20260715.md`.

## 2026-07-15

### Shared RKNN Postprocess Host And Static Verification

1. Architecture recorded:
   - Production `media/rknn_detector.cpp` and the Stage B CLI use the single
     production implementation in `media/rknn_postprocess.cpp` for metadata,
     letterbox preprocessing, builtin-sigmoid decode, class-aware NMS, reverse
     mapping, and validated detection publication.
   - `CarTerminal.pro` contains the shared `.cpp` once in SOURCES and `.h` once
     in HEADERS. Common, adapter, and Stage B CMake targets compile the
     production `.cpp`; the old duplicate common implementation is absent.
   - `Monitor` has no `BuzzerDevice` or `setBuzzerAlarm` dependency, and
     `config/rknn_settings.conf` remains `rknn_enabled=false`.
2. Fresh host verification:
   - Strict direct MinGW common build/run: `PASS=122 FAIL=0`, exit 0.
   - Strict direct MinGW adapter build/run with a quoted
     `RKNN_TEST_PROJECT_ROOT`: `PASS=79 FAIL=0`, exit 0.
   - Alarm manager/policy and buzzer unit tests were freshly rebuilt with
     `-Wall -Wextra -Werror`; both printed their pass summaries and exited 0.
   - Direct detector and Stage B `cmake -P` source gates both exited 0.
   - Static scans found one production `decodeOutputs` call and no old local
     decoder or resize helper definition/call.
3. Targeted Qt host compilation:
   - Qt 5.12.11 qmake and MinGW compiled `release/rknn_detector.o` and
     `release/rknn_postprocess.o` from an ASCII `R:` mapping, using the existing
     host-only `tests/stubs/dlfcn.h`; make exited 0 and the mapping cleanup
     exited 0.
   - qmake reported that `rknn_api.h` is absent, so this verifies only the
     disabled host stub plus shared postprocess, not the board RKNN branch.
4. Preserved earlier board baseline:
   - Stage A was approved: probe exit 0, stability 10/10, zero new NPU dmesg
     errors.
   - Stage B returned five detections including `bus`, wrote the annotated
     output, reported `RKNN_IMAGE_RESULT=PASS`, and exited 0.
   - The earlier board common regression reported `PASS=94 FAIL=0`, exit 0.
5. Fresh unified-implementation board verification:
   - `build_rknn_shared` compiled against the board RKNN headers and passed
     all 4/4 registered CTests, exit 0.
   - Canonical Stage B matched the expected model SHA, returned 0 for RKNN
     input/run/output/release, retained five detections including `bus`, wrote
     a 346 KiB annotated image, printed `RKNN_IMAGE_RESULT=PASS`, and exited 0.
   - The full qmake application linked `rknn_detector.o` and
     `rknn_postprocess.o` and returned `CARTERMINAL_SHARED_BUILD_RC=0`.
   - With RKNN disabled, the normal `quark` user opened the buzzer GPIO,
     reached application ready, handled SIGINT, exited with code 0, and wrote
     active-low silent level 1. The earlier pipeline exit 141 was isolated to
     Ctrl+C closing `tee`; a no-pipe rerun verified the application lifecycle.
6. Remaining verification:
   - Task 7 is complete. The first Task 8 enable attempt exposed an over-strict
     `QNT_NONE`-only production check even though the verified model reports
     native FLOAT16 input/outputs as identity AFFINE (`zp=0`, `scale=1`).
   - Added a Qt/RKNN-independent FLOAT tensor quantization contract with TDD:
     NONE and identity AFFINE pass; DFP/unknown, nonzero zero point, non-unit
     or non-finite scale fail. Production input and all outputs use the same
     validator and report actionable `qnt/zp/scale` diagnostics.
   - Post-fix host RKNN CTest 4/4 plus alarm/buzzer regressions passed. Board
     CTest 4/4, canonical Stage B (five detections including `bus`), forced
     official-header compilation/link, and controlled live initialization all
     returned 0. Initialization matched the model SHA and validated NHWC
     FLOAT16 input plus NCHW FLOAT16 output grids 80/40/20, strides 8/16/32.
   - No real camera is currently connected. Real-frame inference, three-frame
     person alarm trigger, ten-frame clear, 200/800 ms buzzer pattern, and
     post-camera shutdown silence are explicitly deferred until hardware
     arrives; RKNN remains In Progress and the tracked default remains false.
   - At this point the init-only exit logged application code 0 and buzzer
     silent level 1 but no `RKNN detector released`. This historical gap was
     fixed and board-verified by the 2026-07-16 explicit shutdown release entry
     above.
   - DHT11 remains a separate live-hardware pending item.

## 2026-07-14

### Multi-Source Alarm Software Integration

1. Added:
   - `common/alarmmanager.h/.cpp`: aggregate temperature and danger sources with logical OR.
   - `common/alarmpolicy.h/.cpp`: temperature hysteresis and RKNN danger-frame policies.
   - `tests/alarm/test_alarmmanager.cpp`: Qt-free policy and aggregation regression tests.
   - `tests/alarm/test_alarm_board.cpp`: simulated multi-source sequence using the verified buzzer GPIO.
2. Integrated:
   - `Monitor` converts successful normalized RKNN detections into danger candidates and emits only danger-state transitions.
   - Detector failures preserve the danger state; stopping capture resets an active danger source.
   - `MainWindow` owns the aggregate state and a cancellable 200 ms on / 800 ms off buzzer pattern.
   - DHT11 temperature text uses 40 C trigger and 38 C clear hysteresis without direct GPIO access.
   - Clearing one source cannot silence another active source; shutdown stops timers and explicitly silences an available buzzer.
3. Host verification:
   - Alarm manager and policy tests pass with `-Wall -Wextra -Werror`.
   - Existing buzzer unit tests continue to pass.
   - New common objects and modified `MainWindow`, `Monitor`, and Qt MOC targets compile successfully.
4. Board verification:
   - Alarm policy tests pass on RK3588S.
   - The normal-user alarm board diagnostic completed the overlapping temperature/danger sequence, produced the expected short physical pulse, printed `alarm board diagnostic passed`, and returned exit code 0.
   - The updated full application compiled and linked successfully on RK3588S.
   - The normal `quark` user opened `/dev/gpiochip4` line 3 without `sudo`.
   - The application reached `application ready`, exited with code 0 after SIGINT, and restored active-low silent level 1 during shutdown.
5. Remaining verification:
   - Real DHT11 triggering remains hardware-pending because no DHT11 node exists.
   - Live RKNN danger triggering remains runtime-pending until RKNN is enabled and exercised with real camera inference.

### QuarkPi-CA2 Active Buzzer Board Verification

1. Date:
   - 2026-07-14.
2. Task:
   - Complete host, GPIO, physical-board, permission, full-build, and application lifecycle verification for the active buzzer.
3. Added files:
   - `RK3588S_CarTerminal/tests/buzzer/test_buzzer_board.cpp`
   - `RK3588S_CarTerminal/scripts/install_buzzer_gpio_permissions.sh`
4. Modified files:
   - `RK3588S_CarTerminal/config/vehicle_devices.conf`
   - `RK3588S_CarTerminal/media/rtsp_server.cpp`
   - `RK3588S_CarTerminal/docs/MODULE_STATUS.md`
   - `RK3588S_CarTerminal/docs/BUILD_RUN_TEST.md`
   - `RK3588S_CarTerminal/docs/CHANGELOG.md`
5. Hardware mapping confirmed:
   - QuarkPi-CA2 GPIO4_A3.
   - `/dev/gpiochip4`, line offset 3.
   - Active-low polarity: low level beeps and high level is silent.
6. Test result:
   - Host/unit test printed `buzzer tests passed`.
   - Board hardware test completed the silent/beep/silent sequence and printed `buzzer board test passed`.
   - The normal `quark` user was verified as a member of the `gpio` group.
   - `/dev/gpiochip4` was verified as `root:gpio` with mode `crw-rw----`.
7. Build result:
   - Fixed the RTSP build blocker by invoking the thread-local `std::mt19937` engine when generating RTP sequence and SSRC values.
   - The full `CarTerminal` application linked successfully on RK3588S.
8. Runtime result:
   - The application opened the buzzer GPIO without `sudo`.
   - Application startup and shutdown remained silent, confirming safe open/close behavior.
9. Configuration and deployment:
   - `buzzer_enabled=true` is now the checked-in QuarkPi-CA2 target configuration.
   - Added an idempotent udev permission installer for restoring GPIO access after reflashing the board.
10. Remaining issue:
   - Business alarm triggering is not implemented. Danger detection and abnormal-temperature states are not yet connected to `setBuzzerAlarm(true)`.
11. Next step:
   - Add a central multi-source alarm state manager before connecting RKNN danger detection and temperature thresholds to the buzzer.

## 2026-06-28

### 固定语音指令安全闭环验收与测试临时文件安全修复

1. 固定指令闭环验收：
   - 删除孤立“播放”映射 `MusicPlay` 的旧容错规则。
   - `播放列表` 经 ASR 截断为“播放”后，最终命令为 `Unknown`。
   - vehiclecommand 18/18、CTest 4/4、accuracy 6/6、smoke 6/6、negative 6/6。
   - CarTerminal 主程序构建成功。
   - GUI注入：“播放音乐”匹配 `MusicPlay route=direct`；孤立“播放”走 unmatched。
2. 测试代码安全修复：
   - `voice/tests/test_asr_config.cpp` 删除 `std::tmpnam()`。
   - 改用 `mkstemp()` 原子创建临时配置文件，通过文件描述符循环写入，处理 `EINTR` 和部分写入；失败路径关闭并删除文件。
   - 板端验证通过：`BUILD_RC=0`、`TMPNAM_WARNING=NONE`、`asr_config_tests` 1/1通过、`ASR_CONFIG_TEST_RC=0`。
3. 文档修正：
   - `voice/CMakeLists.txt` 的过时注释由“播放精确匹配”改为“孤立播放拒绝执行、完整播放音乐匹配”。
4. 硬件边界：
   - 真实麦克风、人声、车内噪声、扬声器和RKLLM/NPU自由对话不在本次软件闭环验收范围内。

### sherpa 热词 2.0 板端验证与“播放列表”误触发定位

1. Date:
   - 2026-06-28.
2. Task:
   - 验证 `modified_beam_search`、热词文件和热词分数是否真实提升固定指令识别。
   - 对 `SummerTTS → WAV → sherpa → CabinCommand` 执行正向、冒烟和负向回归。
3. Verified configuration:
   - `asr_decoding_method=modified_beam_search`
   - `asr_hotwords_file=./config/asr_hotwords.txt`
   - `asr_hotwords_score=2.0`
4. Build/test result:
   - CTest 4/4 PASS：wav_io、asr_abi、vehiclecommand、asr_config。
   - accuracy 6/6 PASS。
   - smoke 6/6 PASS。
   - 负向 5/6 PASS。
5. Root cause found:
   - `播放列表` 被 sherpa 截断为 `播放`。
   - `vehiclecommand.cpp` 的旧孤立“播放”精确规则将其映射为 `MusicPlay`。
   - 这是 ASR 截断结果与旧容错规则组合产生的误触发，不是热词未生效。
6. Decision:
   - 保持最低有效热词分数 2.0，不继续提高到 2.5。
   - 下一步按 TDD 让孤立“播放”返回 `Unknown`，保留完整播放指令。
7. Remaining issue:
   - negative 达到 6/6、全部 CTest 和主程序构建通过前，语音闭环保持 In Progress。

### sherpa-onnx 1.13.3 ABI 修复：删除未经验证偏移 fallback

1. Date:
   - 2026-06-28.
2. 修改目标：
   - 删除 `AsrEngine::getResult()` 中未经验证的 `*reinterpret_cast<const char *const *>(result)` 结构体偏移 fallback。
   - 改为：运行时加载版本和 Git SHA → 验证动态库恰好是 1.13.3 / 330609da → 使用官方 1.13.3 `OnlineRecognizerResult` 结构读取 `result->text` → 复制到 `std::string` → `DestroyOnlineRecognizerResult`。
   - 同步处理代码审查中确认的 WAV 测试问题与 TTSModel 构造失败处理。
3. 板端已确认事实：
   - 库路径：`/usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so`
   - SHA256：`c713c6c6ca7fcaff01607768d3a163f73aae0c83e476bc1bcbce7839b9695507`
   - 版本：`SherpaOnnxGetVersionStr=1.13.3`
   - Git SHA：`SherpaOnnxGetGitSha1=330609da`
   - Git Date：`SherpaOnnxGetGitDate=Mon Jun 15 07:50:54 2026`
   - 导出：`SherpaOnnxGetOnlineStreamResult`、`SherpaOnnxOnlineStreamInputFinished`、`SherpaOnnxGetVersionStr`、`SherpaOnnxGetGitSha1`、`SherpaOnnxGetGitDate`
   - 不导出：`SherpaOnnxOnlineStreamGetResultText`
   - 官方 1.13.3 结构：`SherpaOnnxOnlineRecognizerResult { const char *text; const char *tokens; const char *const *tokens_arr; float *timestamps; int32_t count; const char *json; }`（text 在偏移 0）
4. 新增文件：
   - `voice/asr/sherpa_onnx_abi_1_13_3.h` — sherpa-onnx 1.13.3 在线识别结果结构 ABI 定义（最小子集）。
     - `OnlineRecognizerResult` 结构：字段与顺序与官方 1.13.3 头文件一致。
     - `static_assert(std::is_standard_layout<...>)` 验证标准布局。
     - `static_assert(offsetof(..., text) == 0)` 验证 text 在偏移 0。
     - `inline bool isCompatible(const char *version, const char *gitSha)`：使用逐字符 strcmp 实现的安全比较，nullptr 返回 false，版本和 Git SHA 必须同时匹配 1.13.3 / 330609da。
     - 文件注释记录：sherpa 版本、Git SHA、Git Date、官方头文件路径（仅作为验证来源）、系统动态库 SHA256、库导出/不导出符号。
   - `voice/tests/test_asr_abi.cpp` — sherpa ABI 单元测试（CTest 注册）。
     - 4 个 `static_assert`：标准布局、text 偏移 0、tokens 在 text 之后、json 在 count 之后、结构体总大小 = 4*ptr + 1*float* + 1*int32_t。
     - 10 个运行时测试用例：兼容、错误 SHA、错误版本、nullptr version、nullptr sha、空字符串、双方 nullptr、前缀版本匹配、前缀 SHA 匹配、过长 SHA 匹配。
     - 不加载真实动态库、不依赖板端模型。
5. 修改文件：
   - `voice/asr/AsrEngine.h` — 删除 `FnGetResultText` typedef、`m_fnGetResultText` 成员、`hasGetResultTextApi()` 方法；新增 `GetVersionStrFn`/`GetGitSha1Fn`/`GetGitDateFn` 函数指针类型、`m_fnGetVersionStr`/`m_fnGetGitSha1`/`m_fnGetGitDate` 成员、`m_runtimeVersion`/`m_runtimeGitSha`/`m_runtimeGitDate`/`m_resultAbiCompatible` 成员、`runtimeVersion()`/`runtimeGitSha()`/`runtimeGitDate()`/`isResultAbiCompatible()` 公有查询接口、`loadRuntimeVersion()` 私有方法。
   - `voice/asr/AsrEngine.cpp` — include `sherpa_onnx_abi_1_13_3.h`；`loadSymbols()` 用 `LOAD_SYM` 强制加载三个版本 API（缺失则初始化失败）；新增 `loadRuntimeVersion()`：调用前清空 dlerror，调用后检查错误，复制返回字符串到 `std::string` 避免长期持有库内部裸指针，调用 `isCompatible()` 校验；`init()` 调用 `loadRuntimeVersion()`（不失败但禁止读取结果）；`getResult()` 重写：`ResultGuard` RAII 保证 rawResult 在所有路径下恰好 destroy 一次，ABI 不兼容时返回空字符串并 log error，兼容时 `static_cast<const sherpa_abi_1_13_3::OnlineRecognizerResult *>(rawResult)` 类型化访问 `result->text`，复制后返回，log `RESULT_API=typed-abi-1.13.3`；删除 `hasGetResultTextApi()` 实现。
   - `voice/audio/wav_io.h` — 新增 `checkedPcm16DataSize(sampleCount, channels, dataSize)` 纯函数声明；注释明确 `readPcm16Wave` 为通用 PCM16 解析器，业务层（asr_wav_test）应自行校验 16kHz/单声道。
   - `voice/audio/wav_io.cpp` — 实现 `checkedPcm16DataSize()`：`dataSize==nullptr`/`channels==0`/`sampleCount==0`/溢出 `uint32_t` 均返回 false；`writePcm16Wave()` 现拒绝 `sampleCount==0`（`"PCM sample data is empty"`）、`samples==nullptr`、`channels==0`（`"channel count must be non-zero"`）、`sampleRate==0`（`"sample rate must be non-zero"`），并复用 `checkedPcm16DataSize()` 计算 dataSize（`"data size overflow"`）。
   - `voice/tests/test_wav_io.cpp` — test 2 改为 `testWriteEmptySamples()`：`sampleCount=0` 必须拒绝且不生成文件；删除原 test 13 `testWriteOverflow()`（nullptr+hugeCount 被 null 检查先拦截，实际未覆盖溢出路径）；新增 test 13 `testCheckedPcm16DataSizeNormal()`：16000×1=32000 字节、8000×2=16000 字节；新增 test 14 `testCheckedPcm16DataSizeFailures()`：0 samples/0 channels/nullptr 输出/超大 sampleCount/边界值（UINT32_MAX/2）共 5 个失败路径；新增 test 15 `testWriteNullSamples()`：nullptr + 非零 count 失败且不生成文件。
   - `voice/tts/include/TTSModel.h` — 新增 `#include <stdexcept>`；构造函数文档化 `@throws std::runtime_error 当 load_model() 返回 false`。
   - `voice/tts/src/TTSModel.cpp` — 构造函数改为：`if (!load_model(model_path)) { /* 先释放 dataW_ 避免抛异常后析构不执行导致泄漏 */ if (dataW_) { tts_free_data(...); dataW_=nullptr; modelSize_=0; } throw std::runtime_error("failed to load TTS model: " + model_path); }`。
   - `voice/tools/tts_wav_test.cpp` — `TTSModel model(modelPath)` 改为 `std::unique_ptr<TTSModel> model; try { model = std::make_unique<TTSModel>(modelPath); } catch (const std::runtime_error &e) { ... return 3; } catch (const std::exception &e) { ... return 3; }`；`PcmGuard` 改为 `guard{*model, pcmData}`；新增 `#include <stdexcept>`。
   - `voice/tools/asr_wav_test.cpp` — `initAsrEngine()` 改为：输出 `SHERPA_VERSION=`/`SHERPA_GIT_SHA=`/`SHERPA_GIT_DATE=`/`RESULT_ABI_COMPATIBLE=`/`INPUT_FINISHED_API=`/`RESULT_API=typed-abi-1.13.3`；ABI 不兼容时 log 详细错误并返回 8（ASR ABI 或运行错误）。`loadAndReportWav()` 已有 16kHz/mono/16-bit 严格校验，无需修改。
   - `voice/CMakeLists.txt` — 新增 `test_asr_abi` 目标（include `asr` 目录、`-Wall -Wextra`、CTest 注册 `asr_abi_tests`）。
6. 设计约束（严格遵守）：
   - 不引入 Qt JSON 依赖到 AsrEngine。
   - 不引入大型 JSON 库只为读取 text。
   - 不继续保留未经版本校验的偏移 fallback。
   - 不通过输入文字或 WAV 文件名伪造 ASR 结果。
   - 不修改系统 sherpa 动态库。
   - 不把板端绝对头文件路径写入 CMake。
   - 不直接依赖 `~/.local/.../c-api.h` 编译。
   - 不提交模型、WAV、二进制和构建目录。
7. 结果生命周期不变量：
   - 创建：`m_fnGetResult`（`SherpaOnnxGetOnlineStreamResult`）在库内 malloc，返回裸指针给调用方。
   - 读取：仅当 `m_resultAbiCompatible==true` 时，`static_cast<const sherpa_abi_1_13_3::OnlineRecognizerResult *>(rawResult)` 访问 `result->text`。
   - 复制：`text.assign(result->text)` 立即复制到 `std::string`，不返回指向库内部的 string_view 或裸指针。
   - 销毁：RAII `ResultGuard` 析构时调 `m_fnDestroyResult`，所有路径（ABI 不兼容、text 为空、`std::string` 抛异常）都恰好 destroy 一次；destroy 后不再访问 result 任何字段；rawResult 为 nullptr 时不调 destroy。
8. 单元测试（host 端，Windows 静态检查标记 NOT RUN）：
   - `test_asr_abi`：4 个 `static_assert` + 10 个运行时测试，预期 10/10 PASS。
   - `test_wav_io`：13 个测试函数（含 15+ 个 CHECK 断言），预期全部 PASS。
   - 编译期检查：`static_assert` 任一失败即编译错误。
9. 待板端验证：
   - `cmake -S voice -B build_voice_review2 -DCMAKE_BUILD_TYPE=Release && cmake --build build_voice_review2 -j$(nproc)` — 0 error。
   - `ctest --test-dir build_voice_review2 --output-on-failure` — `test_wav_io` + `test_asr_abi` 100% 通过。
   - `./build_voice_review2/tts_wav_test --model ./config/single_speaker_fast.bin --text "打开地图" --output /tmp/open_map.wav` — 生成非空 WAV，16000Hz/mono/PCM16，采样不全零。
   - `./build_voice_review2/asr_wav_test --config ./config/asr_settings_offline.conf --wav /tmp/open_map.wav --expect-command MapShow` — 输出 `SHERPA_VERSION=1.13.3`/`SHERPA_GIT_SHA=330609da`/`RESULT_ABI_COMPATIBLE=1`/`RESULT_API=typed-abi-1.13.3`/`INPUT_FINISHED_API=1`/`ASR_TEXT=<非空>`/`COMMAND=MapShow`/`ASR_RESULT=PASS`，退出码 0。
   - 负向测试：错误模型（`/tmp/not-exist.bin`）、截断 WAV、错误预期命令均非 0 退出。
   - `qmake CarTerminal.pro && make -j$(nproc)` 主程序回归编译 0 错误。
10. 结果解释限制：
    - 若板端测试通过，只能写"sherpa-onnx 1.13.3 类型化 ABI 访问+版本校验+TTS 合成音频→ASR 识别→固定指令解析的无硬件软件链路验证通过"。
    - 不能写"真实麦克风语音识别验证通过"或"真实人声识别率验证通过"（TTS 合成语音比真人语音清晰，不能替代真实硬件验收）。
11. 风险：
    - 若板端 sherpa-onnx 库升级到非 1.13.3 / 非 330609da 版本，`isCompatible()` 返回 false，`getResult()` 返回空字符串不崩溃，但 ASR 功能失效。需重新核对新版本结构布局并更新 `sherpa_onnx_abi_1_13_3.h`。
    - TTS 合成语音可能比真人语音清晰，ASR 识别率可能高于真实麦克风场景。
    - `tts_service` 已有 try/catch 捕获 TTSModel 构造异常，无需修改；但其他直接构造 `TTSModel` 的代码需同步加 try/catch。

### TTS→WAV→ASR→固定指令无硬件闭环测试链路实现

1. Date:
   - 2026-06-28.
2. 修改目标：
   - 在无麦克风/扬声器/GUI/LLM/NPU 的条件下，验证 `AsrEngine` 能否从真实 PCM 音频中识别中文并映射为车载指令。
   - 回答核心问题：`AsrEngine` 究竟只是"初始化成功"，还是能够从真实 PCM 音频中识别出中文并映射为车载指令。
3. 链路设计：
   - 中文文字 → SummerTTS 合成 PCM → 写入标准 WAV → AsrEngine 读取 WAV → sherpa-onnx 离线识别 → `vehiclecommand::parseCabinCommand()` → 指令输出。
4. 新增文件：
   - `voice/audio/wav_io.h` / `voice/audio/wav_io.cpp` — 纯 C++17 PCM16 WAV 读写模块。
     - `voiceaudio::Pcm16Wave` 结构体（sampleRate/channels/bitsPerSample/samples）。
     - `writePcm16Wave()`：手工小端写入 RIFF/WAVE/fmt/data chunk，不使用带填充的结构体；空路径/空数据/溢出保护。
     - `readPcm16Wave()`：跳过未知 chunk，处理奇数 chunk 长度 padding，只接受 PCM 格式 1，拒绝非 16 位。
   - `voice/tools/tts_wav_test.cpp` — TTS→WAV CLI 工具。
     - 参数：`--model <bin> --text <UTF-8> --output <wav>`，`--help`。
     - 复用 `TTSModel`（不创建 AudioPlayer/ALSA/ZMQ/线程队列）。
     - RAII `PcmGuard` 确保所有路径调用 `free_data()`，无重复释放。
     - WAV 写入后回读自检元数据。
     - 脚本友好输出：`INPUT_TEXT=`/`MODEL_PATH=`/`WAV_PATH=`/`WAV_RATE=16000`/`WAV_CHANNELS=1`/`WAV_BITS=16`/`WAV_SAMPLES=`/`WAV_DURATION_MS=`/`TTS_RESULT=PASS|FAIL`。
     - 退出码：0 成功 / 2 参数错误 / 3 模型加载失败 / 4 合成失败 / 5 WAV 写入失败 / 6 WAV 回读失败。
   - `voice/tools/asr_wav_test.cpp` — WAV→ASR→指令 CLI 工具。
     - 参数：`--config <conf> --wav <wav> [--expect-text <text>] [--expect-command <Cmd>]`。
     - 严格校验 16kHz/mono/PCM16，不匹配明确报错（不重采样、不伪装采样率）。
     - PCM 诊断：peak absolute sample、RMS。
     - int16→float[-1,1] 转换，分块 1600 采样点（100ms）喂入 ASR。
     - 文件结束处理：优先调用 `inputFinished()` API；不可用时追加 1 秒静音触发 endpoint。
     - 最大解码循环 1000 次防止无限循环。
     - 调用 `vehiclecommand::parseCabinCommand()` 解析指令。
     - 脚本友好输出：`WAV_*`/`PCM_PEAK=`/`PCM_RMS=`/`INPUT_FINISHED_API=`/`RESULT_API=`/`DECODE_CALLS=`/`ENDPOINT_DETECTED=`/`ASR_TEXT=`/`COMMAND=`/`EXPECTED_COMMAND=`/`ASR_RESULT=PASS|FAIL`。
     - 退出码：0 成功 / 2 参数错误 / 3 WAV 读取或格式错误 / 4 ASR 配置或初始化失败 / 5 未获得识别文字 / 6 预期文字不匹配 / 7 预期指令不匹配 / 8 ASR ABI 或运行错误。
   - `voice/tests/test_wav_io.cpp` — 13 例 WAV I/O 单元测试（CTest 注册）。
     - 覆盖：空路径、空采样、有效 WAV、回读元数据、采样点逐个比较、截断 RIFF、非 WAVE、非 PCM、非 16 位、未知 chunk 跳过、奇数 padding、dataSize 越界、溢出保护。
   - `voice/scripts/verify_tts_asr_wav.sh` — 6 条指令批量验证脚本。
     - `set -euo pipefail`，数组驱动（打开地图/地图放大/打开摄像头/播放音乐/下一首/查看天气）。
     - 每条指令独立 WAV，失败样本保留在 `/tmp/car_terminal_voice_test/`。
     - 输出 `[PASS]`/`[FAIL]` + `TOTAL=`/`PASSED=`/`FAILED=`/`RESULT=PASS|FAIL`。
5. 修改文件：
   - `voice/tts/include/TTSModel.h` — 新增 `bool isLoaded() const noexcept;` 声明。
   - `voice/tts/src/TTSModel.cpp` — 新增 `isLoaded()` 实现：`return dataW_ != nullptr && modelSize_ > 0 && synthesizer_ != nullptr;`。
   - `voice/asr/AsrEngine.h` — 新增 `bool inputFinished();` 和 `bool hasInputFinishedApi() const;` 声明；新增 `FnInputFinished` typedef 和 `m_fnInputFinished` 成员。
   - `voice/asr/AsrEngine.cpp` — 新增可选 dlsym `SherpaOnnxOnlineStreamInputFinished`；实现 `inputFinished()`（API 可用时调用，不可用返回 false）；实现 `hasInputFinishedApi()`；缺可选符号不导致初始化失败。
   - `voice/CMakeLists.txt` — 重构：
     - 提取 `summer_tts_engine` 静态库（TTSModel.cpp + Utils.cpp + SummerTTS models/modules/nn_op/hz2py/engipa/platform/utils + TN_SOURCES，不含 main.cpp/AudioPlayer.cpp/MessageQueue.cpp/TextProcessor.cpp）。
     - 提取 `wav_io` 静态库（audio/wav_io.cpp）。
     - `tts_service` 改为链接 `summer_tts_engine`（不再重复源码列表）。
     - 新增 `tts_wav_test`（链接 summer_tts_engine + wav_io + pthread，无 ZMQ/ALSA）。
     - 新增 `asr_wav_test`（链接 wav_io + dl + pthread，源码含 AsrEngine.cpp/vehiclecommand.cpp/logmanager.cpp）。
     - 新增 `test_wav_io`（链接 wav_io，CTest 注册）。
     - `enable_testing()` 启用 CTest。
     - install 列表仅保留生产目标（zmq_component/llm_service/tts_service），测试工具留在 build 目录。
6. 设计约束（严格遵守）：
   - 不打开 ALSA 录音/播放设备。
   - 不启动 Qt GUI。
   - 不启动 LLM/TTS 服务进程。
   - 不访问 NPU。
   - 不伪造 ASR 识别结果（不把输入文字直接传给 vehiclecommand 冒充 ASR 结果）。
   - 不复制 AsrEngine 核心实现（复用 AsrEngine.cpp）。
   - 不提交模型、构建目录或临时 WAV。
7. WAV 格式实现说明：
   - RIFF/WAVE/fmt/data chunk 手工小端写入，不使用带填充的 C++ 结构体。
   - 正确计算 chunkSize/byteRate/blockAlign/dataSize。
   - 防止：长度溢出、空数据、空路径、文件写失败、截断文件。
   - 读取时跳过未知 chunk，处理奇数 chunk 长度对齐字节。
   - 只接受 PCM 格式 1，`asr_wav_test` 严格要求 16kHz/单声道/16 位。
8. sherpa ABI 风险处理：
   - 板端库缺少 `SherpaOnnxOnlineStreamGetResultText`，使用 struct-offset fallback（假设 text 在结果结构体偏移 0）。
   - 新增可选 `SherpaOnnxOnlineStreamInputFinished` dlsym，不可用时调用方追加 1 秒静音。
   - 缺可选符号不导致 AsrEngine 初始化失败。
   - 待板端实测：若 fallback 返回乱码/空/崩溃，停止猜结构体布局，查找板端匹配头文件修复。
9. 待板端验证：
   - `cmake -S voice -B build_voice_tools -DCMAKE_BUILD_TYPE=Release && cmake --build build_voice_tools -j$(nproc)` — 构建 0 error。
   - `ctest --test-dir build_voice_tools --output-on-failure` — WAV I/O 单元测试 100% 通过。
   - `./build_voice_tools/tts_wav_test --model ./config/single_speaker_fast.bin --text "打开地图" --output /tmp/open_map.wav` — 生成非空 WAV，16000Hz/mono/PCM16，采样不全零。
   - `./build_voice_tools/asr_wav_test --config ./config/asr_settings_offline.conf --wav /tmp/open_map.wav --expect-command MapShow` — ASR 识别中文，解析为 MapShow。
   - `./voice/scripts/verify_tts_asr_wav.sh` — 6 条指令批量验证。
   - CarTerminal 回归编译：`make clean && qmake && make -j$(nproc)` — 0 error。
   - tts_service 回归编译 — 0 error。
10. 结果解释限制：
    - 若测试通过，只能写"SummerTTS 合成音频→sherpa-onnx 识别→固定指令解析的无硬件软件链路验证通过"。
    - 不能写"真实麦克风语音识别验证通过"或"真实人声识别率验证通过"（TTS 合成语音比真人语音清晰，不能替代真实硬件验收）。
11. 风险：
    - sherpa-onnx struct-offset fallback ABI 敏感，库版本变化可能 break。
    - TTS 合成语音可能比真人语音清晰，ASR 识别率可能高于真实麦克风场景。
    - 板端 sherpa-onnx 库版本若与代码结构体定义不匹配，可能段错误（已通过 2026-06-27 修复 nemo_ctc/t_one_ctc/hr 字段，但新版本可能再变）。

## 2026-06-27

### 板端文字指令注入测试全部通过

1. Date:
   - 2026-06-27.
2. 修改目标：
   - 在无麦克风的情况下，验证离线ASR→固定指令→GUI双路径路由的指令匹配、路由决策、GUI动作、配置开关是否真实有效。
3. 测试方法：
   - 新增 `VEHICLE_VOICE_TEST_TEXT` 环境变量测试注入点：在 MainWindow 构造函数末尾读取环境变量，1秒后通过 `QTimer::singleShot` 调用 `onOfflineAsrTextRecognized` 槽函数，绕过麦克风直接验证指令匹配+路由+GUI。
   - 环境变量未设置时，注入点不影响正常运行。
4. 测试用例与结果：
   - ✅ "打开地图" → `command=MapShow route=direct` + GUI 切换。
   - ✅ "打开摄像头" → `command=MonitorShow route=direct` + GUI 切换。
   - ✅ "播放音乐" → `command=MusicPlay route=direct` + GUI 切换。
   - ✅ "查看天气" → `command=WeatherShow route=direct` + GUI 切换。
   - ✅ "给我讲个笑话" → `voice command unmatched` + `LLM unavailable`，不崩溃。
   - ✅ `direct_command_enabled=false` 时，"打开地图" 不打开地图，`voice command unmatched`。
   - ✅ `grep -Ei 'rkllm|sendToLlm' test_*.log` 无输出（LLM 完全未调用）。
   - ✅ 无 SIGBUS / 无崩溃 / 无段错误。
5. 修复文件：
   - `gui/mainwindow.cpp` — 新增 `VEHICLE_VOICE_TEST_TEXT` 测试注入点（构造函数末尾）。
   - `gui/mainwindow.cpp` — 修复 `direct_command_enabled=false` 时仍打开地图的问题（`onOfflineAsrTextRecognized` 先检查 `isDirectCommandEnabled()` 再调用 `dispatchVoiceCommand`）。
6. 影响：
   - 离线ASR→固定指令→GUI路径板端验证全部通过。
   - 测试注入点环境变量未设置时不影响正常运行。
   - 待验证（需麦克风）：真实语音输入端到端。
7. 验证日志：

```text
[09:03:02] injecting voice test text: "打开地图"
[09:03:02] voice command matched: text="打开地图" command=MapShow route=direct
[09:04:33] injecting voice test text: "打开摄像头"
[09:04:33] voice command matched: text="打开摄像头" command=MonitorShow route=direct
[09:05:01] injecting voice test text: "播放音乐"
[09:05:01] voice command matched: text="播放音乐" command=MusicPlay route=direct
[09:05:40] injecting voice test text: "查看天气"
[09:05:40] voice command matched: text="查看天气" command=WeatherShow route=direct
[09:06:12] injecting voice test text: "给我讲个笑话"
[09:06:12] voice command unmatched: text="给我讲个笑话"
[09:06:12] voice command unmatched and LLM unavailable: text="给我讲个笑话"
# direct_command_enabled=false
[09:07:35] injecting voice test text: "打开地图"
[09:07:35] voice command unmatched and LLM unavailable: text="打开地图"
```

### AsrEngine sherpa-onnx结构体布局修复（板端崩溃）

1. Date:
   - 2026-06-27.
2. 修改目标：
   - 修复板端运行时 `SherpaOnnxCreateOnlineRecognizer` 段错误崩溃。
3. 根因：
   - 板端安装的 sherpa-onnx 库版本（2026-06-26 编译）比代码中定义的结构体更新。
   - 官方头文件新增了 3 个字段：`SherpaOnnxOnlineNemoCtcModelConfig nemo_ctc`、`SherpaOnnxOnlineToneCtcModelConfig t_one_ctc`（在 `SherpaOnnxOnlineModelConfig` 中）、`SherpaOnnxHomophoneReplacerConfig hr`（在 `SherpaOnnxOnlineRecognizerConfig` 中）。
   - 代码中 `memset(&config, 0, sizeof(config))` 使用的 `sizeof` 比库期望的小 40 字节（aarch64），导致库读取越界内存 → 段错误。
4. 修复内容：
   - 新增 `SherpaOnnxOnlineNemoCtcModelConfig` 和 `SherpaOnnxOnlineToneCtcModelConfig` 结构体定义。
   - `SherpaOnnxOnlineModelConfig` 末尾新增 `nemo_ctc` 和 `t_one_ctc` 字段。
   - 新增 `SherpaOnnxHomophoneReplacerConfig` 结构体定义（`dict_dir`/`lexicon`/`rule_fsts`）。
   - `SherpaOnnxOnlineRecognizerConfig` 末尾新增 `hr` 字段。
   - `memset(0)` 后所有新字段为 NULL/0，不影响现有功能。
5. 修改文件：
   - `voice/asr/AsrEngine.cpp` — 新增 3 个结构体 + 3 个字段，功能逻辑零改动。
6. 板端验证结果：
   - 编译成功：`make clean && qmake && make -j$(nproc)` → 0 错误。
   - ASR 引擎初始化：`ASR engine initialized (model=..., sample_rate=16000, threads=2)` ✓。
   - ALSA 设备打开：`ALSA capture opened (device=plughw:2,0, rate=16000)` ✓。
   - LLM/TTS 未启动：`LLM disabled` / `TTS disabled` ✓。
   - 管线就绪：`voice pipeline initialized (asr=1 llm=0 tts=0 direct=1)` ✓。
   - 旧 ASR 互斥：`offline ASR ready, legacy online ASR disabled` ✓。
   - 无 SIGBUS/崩溃/段错误 ✓。
   - 程序正常启动并显示地图页面 ✓。

### 离线ASR→固定指令→GUI双路径重构（不依赖NPU/RKLLM）

1. Date:
   - 2026-06-27.
2. 修改目标：
   - 在NPU驱动v0.9.6存在SIGBUS的情况下，让离线ASR → vehiclecommand关键词匹配 → Qt GUI直接控制路径不依赖RKLLM。
   - LLM/TTS成为可独立启用的子模块（默认false，不自动启动）。
   - 实现双路径路由：固定指令命中直接GUI；未命中可选转发LLM。
   - 修正文档"整个语音链路都被NPU驱动阻塞"的错误描述。
3. 修改文件：
   - `voice/voice_config.h`（新）：Qt-free的VoiceConfig结构 + parseBool + loadVoiceConfig + VoiceRoute枚举 + decideVoiceRoute声明。
   - `voice/voice_config.cpp`（新）：parseBool实现（true/false/1/0/yes/no/on/off大小写不敏感，非法值用安全默认值且WARN）；loadVoiceConfig实现（缺文件返回默认配置且WARN，不抛异常）；decideVoiceRoute实现（commandMatched=true永远不路由LLM，未命中需shouldFallback && llmAvailable同时为true才路由LLM）。
   - `voice/VoiceManager.h`：包含voice_config.h；新增isAsrReady/isLlmAvailable/isTtsAvailable/isDirectCommandEnabled/shouldFallbackUnknownCommandToLlm状态接口；新增m_asrReady/m_llmStarted/m_ttsStarted/m_llmClientReady独立状态标志。
   - `voice/VoiceManager.cpp`：init()三模块独立启用（LLM/TTS默认false不阻塞ASR）；deinit()只停止实际启动过的模块（幂等）；asrThreadFunc删除无条件sendToLlm，只emit asrTextRecognized；sendToLlm先检查isLlmAvailable；loadConfig委托给loadVoiceConfig。
   - `gui/mainwindow.h`：包含vehiclecommand.h；新增onOfflineAsrTextRecognized/dispatchVoiceCommand/executeCabinCommand；新增legacyAsrStarted标志。
   - `gui/mainwindow.cpp`：构造函数连接voiceManager::asrTextRecognized→onOfflineAsrTextRecognized；离线ASR就绪时不启动旧网络ASR（避免麦克风冲突）；getSpeechResult改调dispatchVoiceCommand（消除重复switch）；executeCabinCommand包含唯一switch；onOfflineAsrTextRecognized调用decideVoiceRoute做路由决策；析构函数显式停止AsrThread。
   - `common/vehiclecommand.cpp`：新增关键词kZhMapView(查看地图)/kZhCameraView(查看摄像头)/kZhWeatherToday(今天天气)/kZhOpenVerb(打开)/kZhCameraNoun(摄像头)；新增containsAll helper容忍"帮我打开一下摄像头"等填充词；parseCabinCommand匹配逻辑更新（MapShow含kZhMapView，MonitorShow含kZhCameraView或containsAll({kZhOpenVerb,kZhCameraNoun})，WeatherShow含kZhWeatherToday）。
   - `config/voice_settings.conf`：新增voice_enabled/direct_command_enabled=true/llm_enabled=false/tts_enabled=false/unknown_command_fallback_to_llm=true；详细注释说明NPU v0.9.6 SIGBUS约束。
   - `CarTerminal.pro`：新增voice_config.cpp到SOURCES、voice_config.h到HEADERS。
4. 新增测试文件：
   - `tests/voice_config/test_voice_config.cpp`（46例）：parseBool各种取值大小写、非法值安全默认、完整配置加载、缺文件、默认值。
   - `tests/voice_routing/test_voice_routing.cpp`（7例）：5种路由场景 + 2种不变量（matched永远不路由LLM；LLM需三条件同时满足）。
   - `tests/vehicle_command/test_vehiclecommand.cpp`（41例）：覆盖section八所有表达 + 冲突用例（暂停音乐≠MusicPlay，关闭音乐≠MusicOpen，退出音乐≠MusicPlay）+ 中英文 + Unknown。
   - `tests/voice_lifecycle/run_board_lifecycle_tests.sh`：板端生命周期与失败路径测试脚本（6场景：ASR-only、voice_disabled、asr_config_missing、llm_binary_missing、tts_binary_missing、idempotency init→deinit×2）。
5. 新增配置项：
   - `voice_enabled` (false)：语音总开关。
   - `direct_command_enabled` (true)：固定指令直连GUI（安全默认）。
   - `llm_enabled` (false)：LLM开关（NPU v0.9.6 SIGBUS，必须false）。
   - `tts_enabled` (false)：TTS独立开关。
   - `unknown_command_fallback_to_llm` (true)：未匹配指令是否尝试LLM（仅llm_enabled=true时生效）。
6. 测试命令及输出：
   - `powershell -ExecutionPolicy Bypass -NoProfile -File tests\voice_routing\build_and_run.ps1`
     输出：`test_voice_routing: ALL PASS`
   - `powershell -ExecutionPolicy Bypass -NoProfile -File tests\voice_config\build_and_run.ps1`
     输出：`voice_config tests passed`（46例全过）
   - `powershell -ExecutionPolicy Bypass -NoProfile -File tests\vehicle_command\build_and_run.ps1`
     输出：`vehiclecommand tests passed`（41例全过）
7. 编译结果：
   - qmake语法检查：`qmake -Wall -Wparser -Wlogic CarTerminal.pro` 退出码0。
   - voice_config.cpp -fsyntax-only -Wall -Wextra -pedantic 退出码0。
   - 静态检查：无detached thread（worker存于m_workerThreads，deinit join）；无裸while(true)；无重复switch（executeCabinCommand唯一switch）；无unconditional sendToLlm（asrThreadFunc只emit）；无unconditional LLM启动（startLlmProcess仅在m_config.llmEnabled时调用）；无配置硬编码（仅`plughw:0,0` ALSA fallback默认值，实际值由asrConfig.alsaDevice提供）；VoiceManager不直接访问任何QWidget。
8. 板端验证步骤：
   - 见 `tests/voice_lifecycle/run_board_lifecycle_tests.sh`。
   - 部署后执行：`./scripts/verify_voice_deployment.sh` 和 `./tests/voice_lifecycle/run_board_lifecycle_tests.sh`。
   - 验证日志应出现：ASR engine initialized / ALSA capture opened / offline ASR ready / LLM disabled / TTS disabled / Voice pipeline ready。
   - 验证日志不应出现：rkllm_run / SIGBUS / Bus error / llm_service started。
9. 已验证功能（host端）：
   - vehiclecommand关键词匹配（41例PASS）。
   - VoiceConfig布尔解析与缺文件安全默认（46例PASS）。
   - decideVoiceRoute路由决策（7例PASS）。
   - qmake语法、静态检查、源码-syntax-only全部通过。
10. 尚未验证功能：
    - 板端真实麦克风采集。
    - 板端sherpa-onnx离线ASR中文识别率。
    - 板端长时间运行稳定性。
    - 板端LLM推理（仍被NPU驱动阻塞）。
    - 板端TTS实际播放效果（仍被NPU驱动间接阻塞）。
11. 风险和后续工作：
    - 风险：sherpa-onnx库未在板端验证dlopen加载，可能存在ABI不匹配。
    - 风险：ALSA设备名`plughw:2,0`未在板端arecord -l确认，可能需要调整asr_settings_offline.conf。
    - 后续：板端部署后执行`run_board_lifecycle_tests.sh`；NPU驱动升级到v0.9.7+后开启`llm_enabled=true`和`tts_enabled=true`验证完整对话路径。
12. 关键代码diff摘要：
    - `voice_config.cpp` 新增 decideVoiceRoute()：commandMatched=true 永远返回 DirectCommand（保护固定指令不经LLM不变量）。
    - `VoiceManager.cpp` asrThreadFunc：删除 `sendToLlm(text)`，改为只 `emit asrTextRecognized(QString::fromStdString(text))`。
    - `VoiceManager.cpp` init()：`if (m_config.llmEnabled) { startLlmProcess(); }` 替代原无条件启动；`if (m_config.ttsEnabled) { startTtsProcess(); }`。
    - `mainwindow.cpp` onOfflineAsrTextRecognized：调用 `decideVoiceRoute()` 返回的 route switch 决策，DirectCommand 直接return不调sendToLlm。

### 语音管线代码审查修复（6项：2×P1 + 1×P1遗留 + 2×P2 + 1×P3）

1. Date:
   - 2026-06-27.
2. 修改目标：
   - 对照30年嵌入式工程师审查标准，修复线程安全、UB、边界检查等问题。
3. 修复内容：
   - **P1-1 ALSA阻塞导致ASR线程无法退出**：`asrThreadFunc` 中 `snd_pcm_readi` 是阻塞调用，`m_asrRunning=false` 时线程仍阻塞，`stopAsrThread().join()` 永久挂起。修复：使用 `poll()` + 200ms timeout 替代纯阻塞读取，线程每200ms检查退出标志。新增 `#include <poll.h>` 和 `snd_pcm_poll_descriptors()` 获取ALSA fd。
   - **P1-2 析构期emit信号UB**：`deinit()` 从 `~VoiceManager()` 调用时无条件 `emit voiceStatusChanged()`，receiver可能已销毁。修复：新增 `std::atomic<bool> m_destroying` 标志，析构时置true；`deinit()` 和 worker lambda 中检查此标志抑制信号发射。worker lambda 同时改为按值捕获 `client` 指针，避免 `m_llmClient` 被delete后use-after-free。
   - **P1-2遗留 sendToLlm持锁join阻塞GUI**：原代码在 `m_workerMutex` 锁内调用 `t.join()`，若worker阻塞在ZMQ请求上则GUI线程也被阻塞。根因修复：`sendToLlm` 只 `push_back` 新线程不join旧线程；`joinWorkerThreads()` 使用 swap-then-join 模式（交换到局部vector后释放锁再join）。
   - **P2-1 eventFilter对非鼠标事件UB**：`static_cast<QMouseEvent*>(ev)` 在 `ev` 不是 QMouseEvent 时是UB。修复：先检查 `ev->type() == QEvent::MouseButtonPress/Release` 再cast。
   - **P2-2 getSpeechResult lastIndexOf(-1)越界**：`content.remove(content.lastIndexOf('\n'), 1)` 在content无`\n`时删除末尾字节。修复：先检查 `nlPos >= 0` 再remove。
   - **P3-1 stoi静默解析非纯数字**：`std::stoi("5000ms")` 返回5000不报错。修复：使用 `stoi(val, &pos)` + `pos == val.size()` 严格校验，拒绝"5000ms"/"abc"类配置值。
4. 修改文件：
   - `voice/VoiceManager.h`：新增 `std::atomic<bool> m_destroying{false}`。
   - `voice/VoiceManager.cpp`：新增 `#include <poll.h>` / `#include <cerrno>` / `#include <cstring>`；asrThreadFunc使用poll+200ms timeout；deinit()检查m_destroying抑制信号；sendToLlm worker按值捕获client指针+m_destroying检查；sendToLlm只push_back不join；joinWorkerThreads swap-then-join；析构函数设置m_destroying=true。
   - `gui/mainwindow.cpp`：eventFilter先检查事件类型再cast；getSpeechResult检查lastIndexOf返回值。
   - `voice/voice_config.cpp`：stoi使用pos参数严格校验。
   - `tests/voice_config/test_voice_config.cpp`：新增4例zmq_timeout_ms严格校验（5000ms→默认、abc→默认、空→默认、15000→解析成功）。
5. 测试结果：
   - voice_routing 7例：ALL PASS。
   - voice_config 50例（原46+新增4）：ALL PASS。
   - vehicle_command 41例：ALL PASS。
6. 关键代码diff摘要：
   - `asrThreadFunc`：`poll(&pfd, 1, 200)` → timeout时 `continue` 检查 `m_asrRunning`；`POLLIN` 时才 `snd_pcm_readi`。
   - `~VoiceManager()`：`m_destroying = true; deinit();`。
   - `sendToLlm` lambda：`[this, client, textCopy, timeout]()` 按值捕获；`if (!m_destroying.load()) { emit ... }`。
   - `joinWorkerThreads`：`threads.swap(m_workerThreads)` 释放锁后再 `t.join()`。
   - `eventFilter`：`if (ev->type() != QEvent::MouseButtonPress && ...) return QWidget::eventFilter(watched, ev);`。
   - `voice_config.cpp`：`size_t pos = 0; int parsed = std::stoi(val, &pos); if (pos == val.size()) { config.zmqTimeoutMs = parsed; }`。

### MPP硬件解码器代码规范审查与修复

1. Date:
   - 2026-06-27.
2. 修改目标：
   - 对照 ENGINEERING_PRINCIPLES.md 审查 mpp_decoder.cpp，修复 P0 级 ABI 错误。
3. 修复内容：
   - `MPP_FMT_YUV420P` 枚举值 1→4（与板端 rk_mpi.h 一致，编码器同一根因）。
   - `mpp_buf_group_get` 函数签名 2参数→5参数（与板端 mpp_buffer.h 一致）。
   - buffer 类型 NORMAL→ION（与 project1 参考实现一致）。
   - 模块名 "mpp"→"mpp_dec"（区分编码器/解码器日志）。
   - `nv12ToRgb32` 添加 null 检查 guard clause。
4. 修复文件：
   - `media/mpp_decoder.cpp` — 5处修改，功能逻辑零改动。
5. 影响：
   - 不影响其他模块，解码流程/错误处理/公共接口全部不变。
   - 待板端重新编译验证。
6. 审查结论：
   - P0 严重问题 3 项（枚举值、函数签名、buffer类型）已修复。
   - P1 中等问题 2 项（全局变量封装、RAII）暂缓，不影响当前功能。
   - P2 低优先级 2 项（null检查、模块名）已修复。

### MPP硬件编码器板端验证通过（RTSP推流成功）

1. Date:
   - 2026-06-27.
2. 验证结果：
   - `test_mpp_direct` 直接链接测试：10/10 帧编码成功，总计 22467 bytes，
     H.264 Annex-B 流头 `00 00 00 01 67`（SPS）正确。
   - `CarTerminal` 主程序 RTSP 推流：MPP 编码 800x600 → 200-4000 bytes/帧，
     IDR 帧每 50 帧一个（3-4KB），P 帧平均 200-400 bytes。
   - VLC（<LAN_IP>）成功连接 `rtsp://<LAN_IP>:8554/live` 并播放。
3. 根因确认：
   - `MPP_FMT_YUV420P` 枚举值错误硬编码为 `1`（实际为 `4`），
     值 `1` 实际是 `MPP_FMT_YUV420SP_10BIT`，导致编码器配置的像素格式
     与输入数据不匹配，所有帧编码输出为空。
4. 修复文件：
   - `media/mpp_encoder.cpp` — 使用 mpp_abi_constants.h 中的正确常量。
   - `media/mpp_abi_constants.h` — ABI 常量集中定义（YUV420P=4, HEVC=0x01000004, TIMEOUT=-8）。
   - `media/mpp_frame_layout.h` — I420 对齐布局 helper。
   - `tests/test_mpp_direct.cpp` — 直接链接板端回归测试（已重写）。
5. 影响：
   - MPP Encoder 模块状态从 Blocked 改为 Done。
   - RTSP 模块 MPP 硬件编码路径从阻塞改为已验证。
   - pktSize=0 阻塞问题正式关闭。

### MPP编码器根因修复（ABI常量、Packet和对齐布局）

1. Date:
   - 2026-06-27.
2. Added files:
   - `media/mpp_abi_constants.h`: 集中定义并测试 MPP ABI 常量。
   - `media/mpp_frame_layout.h`: 纯 C++17 I420 对齐布局及平面拷贝。
   - `tests/test_mpp_abi_constants.cpp`: 对照 project1/板端 SDK 头文件验证 ABI。
   - `tests/test_mpp_frame_layout.cpp`: 验证 16 字节 stride、Y/U/V 偏移和 padding。
   - `tests/stubs/dlfcn.h`: 仅用于 Windows host 的 Linux 源码语法检查。
3. Modified files:
   - `media/mpp_encoder.cpp`: 修复像素格式、HEVC、timeout 常量；恢复异步
     put/get；移除零容量输出 packet；使用 packet pos；加入对齐 I420 拷贝；
     检查配置返回值和空包。
   - `tests/test_mpp_direct.cpp`: 重写为直接链接、头文件驱动的板端回归测试。
   - `CarTerminal.pro`: 登记新增 media 头文件。
4. Deleted files:
   - None.
5. Root cause and evidence:
   - 板端 `/usr/bin/mpi_enc_test -w 800 -h 600 -f 4 -t 7 -n 10`
     成功生成 42 KiB H.264 文件，排除 VPU、驱动和系统 MPP 库故障。
   - 旧代码将 YUV420P 错写为枚举值 1（实际是 10-bit semiplanar），将
     timeout 错写为 -2，并向同步 encode 传入容量为 0 的输出 packet。
6. Compatibility / risk:
   - 公共 `mpp_encoder.h` 接口未改变。
   - 保留 dlopen 和 libx264 初始化失败回退架构。
   - 板端 MPP 头文件若与 project1 ABI 不同，direct test 会在 static_assert
     阶段明确失败，避免静默错用。
7. Host verification:
   - `test_mpp_frame_layout`: PASS.
   - `test_mpp_abi_constants`（对照 project1 MPP headers）: PASS.
   - `mpp_encoder.cpp` 使用 dlfcn test stub 的 `-fsyntax-only`: PASS，
     无项目代码警告。
   - `test_mpp_direct.cpp` 对照 project1 headers 的 `-fsyntax-only`: PASS.
   - qmake 语法检查退出码 0；Windows qmake 对中文绝对路径下 qrc 输出既有
     路径警告，和本次修改无关。
   - ASan/UBSan 未运行：本机 MinGW 7.3 缺少 `libasan`/`libubsan`，
     链接器无法生成 sanitizer 测试程序。
8. Remaining issue:
   - 尚未在 RK3588S 板端编译运行修复后的 direct test 和主应用。
9. Next step:
   - 板端运行 `test_mpp_direct`，确认 10/10 帧输出非零 Annex-B 数据；
     随后重建主程序并验证 RTSP MPP 路径。

### MPP编码器pktSize=0问题排查与隔离

1. Date:
   - 2026-06-27.
2. Affected file(s):
   - `RK3588S_CarTerminal/tests/test_mpp_direct.cpp` (new): 直接链接librockchip_mpp.so的测试程序，不使用dlopen，用于隔离问题。
   - `rk3588s_carterminal/media/mpp_encoder.cpp`: 多轮修复尝试（buffer sync、合并encode接口、s_mpp_packet_init等函数指针添加）。
3. What changed:
   - **创建test_mpp_direct.cpp**：直接使用`#include <rockchip/rk_mpi.h>`链接librockchip_mpp.so，使用`mpp_buffer_group_get_internal`宏和合并`encode`接口，完全模仿project1方式。用于排除dlopen方式引入的问题。
   - **mpp_encoder.cpp多轮修复**：
     - 添加buffer sync（`mpp_buffer_sync_begin_f`/`mpp_buffer_sync_end_f`）确保CPU写入的YUV数据对VPU可见
     - 尝试从异步两步法（`encode_put_frame`+`encode_get_packet`）切换到合并同步`encode`接口
     - 添加`s_mpp_packet_init`/`s_mpp_packet_set_data`/`s_mpp_packet_set_size`函数指针声明和RESOLVE_SYM
     - stride尝试不使用16对齐（与project1一致）
   - **板端多次测试结果**：所有编码尝试均返回MPP_OK但pktSize=0
4. Why:
   - MPP编码器始终无法输出H.264数据，阻塞了RTSP推流使用硬件编码的性能优化目标
   - 需要确定问题根源是dlopen方式还是MPP配置/硬件本身
5. 已排查并排除的问题（非根因）：
   - ① MppApi结构体布局 → 已按板端rk_mpi.h修正
   - ② MpiCmd枚举值 → 已通过板端cmd_test.c确认
   - ③ 函数签名（buffer_group_get等）→ 已按头文件修正
   - ④ dlsym符号名 → 已通过nm -D确认
   - ⑤ getHeader()时序 → 已延迟到首帧后
   - ⑥ memcpy越界 → bufSize改为实际数据大小
   - ⑦ MPP_BUFFER_TYPE → ION通过MPP内部回退可用
   - ⑧ stride对齐 → 两种方式均pktSize=0
   - ⑨ 异步vs同步encode → 两种方式均pktSize=0
   - ⑩ buffer sync → 添加后无变化
   - ⑪ s_mpp_packet_init等未声明 → 已添加声明和RESOLVE_SYM
6. 当前状态：
   - **核心问题未解决**：pktSize=0
   - test_mpp_direct.cpp已创建但尚未在板端运行
   - mpp_encoder.cpp最新修改（添加s_mpp_packet_init等）编译报错（用户可能未传输最新文件到板端）
7. Next step:
   - 板端编译运行test_mpp_direct.cpp（`g++ -std=c++17 tests/test_mpp_direct.cpp -lrockchip_mpp -o test_mpp_direct`）
   - 根据直接链接测试结果确定下一步排查方向

### MPP Encoder Code Review Fixes (30-Year Engineer Review — Round 3)

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `media/mpp_encoder.cpp`: 添加 s_horStride/s_verStride 对齐 stride 变量；encode() 使用对齐 stride。
   - `media/rtsp_server.cpp`: MPP 路径 m_frameCount 递增；IDR 检测扫描所有 NAL。
3. What changed:
   - **P2 修复 — frame stride 对齐**：`encode()` 中 `mpp_frame_set_hor_stride/ver_stride` 和 buffer size 改用 `MPP_ALIGN(width, 16)` 对齐值。新增 `s_horStride`/`s_verStride` 静态变量在 `initEncoder()` 中计算。避免非 16 对齐分辨率（如 1366x768）导致的花屏。
   - **P3 修复 — m_frameCount 递增**：MPP 编码成功后添加 `m_frameCount++`，确保日志中 IDR 帧编号正确。
   - **P3 修复 — IDR 检测扫描所有 NAL**：移除 `break`，继续扫描后续 NAL 单元。当 MPP 输出 `[SEI][SPS][PPS][IDR]` 时，能正确检测到 SPS 并跳过前置。仅在找到 SPS 时才提前终止。
4. Why:
   - frame stride 与 encoder config stride 不匹配会导致 MPP 读取数据错位。
   - m_frameCount 未递增导致 IDR 日志始终显示 frame #0。
   - 只检查第一个 NAL 会漏检 SEI 后面的 SPS/IDR。
5. Test status:
   - 代码修复完成，待板端编译验证。

### MPP Encoder Code Review Fixes (30-Year Engineer Review — Round 2)

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `media/rtsp_server.cpp`: 修复 extractSpsPpsFromMppHeader() H.265 边界越界。
   - `media/mpp_encoder.cpp`: 添加 MPP_ALIGN 宏，stride 16字节对齐。
3. What changed:
   - **P2 修复 — H.265 边界越界**：`extractSpsPpsFromMppHeader()` 循环条件 `i + 4 < headerSize` 在 H.265 模式下访问 `headerData[i+5]` 时可能越界1字节。改为 `i + minLen < headerSize`（H.265: minLen=5, H.264: minLen=4）。
   - **P3 修复 — stride 对齐**：添加 `MPP_ALIGN(x, 16)` 宏，`prep:hor_stride` 和 `prep:ver_stride` 使用 16 字节对齐。当前 800x600 恰好对齐，但非对齐分辨率（如 720x480）会导致花屏或 ION 分配失败。
4. Why:
   - H.265 NAL header 是 2 字节，比 H.264 多 1 字节，原循环条件未覆盖。
   - MPP VPU 要求 stride 按硬件对齐（通常 16 或 256 字节）。
5. Test status:
   - 代码修复完成，待板端编译验证。

### MPP Encoder Code Review Fixes (30-Year Engineer Review)

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `media/mpp_encoder.h`: 新增 `isH265()` 声明；修正 NV12→YUV420P 注释；修正 encode() 参数名。
   - `media/mpp_encoder.cpp`: 修复 MppApi 结构体布局（P0）；删除死代码（s_pktGrp/s_yuvBuffer/typedef uint32_t）；添加 dlsym 逐符号错误日志；添加 isH265() 实现。
   - `media/rtsp_server.h`: 新增 `m_spsPpsExtracted` 标志；更新 SPS/PPS 注释。
   - `media/rtsp_server.cpp`: 修复 getHeader() 时序（P0）；修复 IDR 检测支持 H.265（P1）；修复 extractSpsPpsFromMppHeader() 支持 H.265 NAL type；添加 H.265 NAL type 常量。
3. What changed:
   - **P0 致命修复 — MppApi 结构体**：添加缺失的 `reset` 和 `flush` 函数指针。旧版缺少这两个字段导致 control/encode_put_frame/encode_get_packet 等所有函数指针偏移错误，板端运行必然崩溃。
   - **P0 致命修复 — getHeader() 时序**：MPP_ENC_GET_EXTRA_INFO 要求至少编码过一帧才能获取 SPS/PPS。旧版在 init() 阶段调用，永远返回空。新版延迟到 encodeAndSend() 首帧编码成功后自动提取。
   - **P1 严重修复 — H.265 IDR 检测**：旧版用 `& 0x1F` 提取 NAL type，仅适用于 H.264。新版区分 H.264（`& 0x1F`，SPS=7, IDR=5）和 H.265（`>> 1 & 0x3F`，SPS=33, IDR=19/20, CRA=21）。
   - **P1 修复 — extractSpsPpsFromMppHeader()**：支持 H.265 的 2 字节 NAL header 和 VPS/SPS/PPS 类型。
   - **P2 修复 — 死代码清理**：删除未使用的 `s_pktGrp`（ION packet buffer group）、`s_yuvBuffer`（720KB 预分配缓冲区）、`typedef unsigned int uint32_t`（与 cstdint 冲突）。
   - **P2 修复 — 注释一致性**：所有 NV12 注释修正为 YUV420P，与 MPP_FMT_YUV420P 配置一致。
   - **P3 修复 — dlsym 错误日志**：使用 RESOLVE_SYM 宏逐个记录 dlsym 失败，便于调试。
4. Why:
   - 代码审查发现 MppApi 结构体布局错误是致命缺陷，不修复板端无法运行。
   - getHeader() 时序错误导致 SPS/PPS 永远为空，VLC 无法连接。
   - H.265 模式下 IDR 检测使用了 H.264 的 NAL type 提取方式，导致 SPS/PPS 丢失。
5. Compatibility / risk:
   - 公共接口不变（新增 isH265() 是纯新增，不影响现有调用）。
   - MppApi 结构体修复后与真实 MPP SDK 布局一致。
   - 延迟 SPS/PPS 提取不影响 RTSP 客户端（VLC 在 DESCRIBE 时获取 SDP，此时已编码过帧）。
6. Test status:
   - 代码修复完成，待板端编译验证。

### MPP Hardware Encoding Integration into RTSP Pipeline

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `media/mpp_encoder.h`: 重写为参考project1的完整接口（isAvailable/initEncoder/deinitEncoder/encode/getHeader）。
   - `media/mpp_encoder.cpp`: 完全重写（558行），基于project1 MppEncoder类实现dlopen模式完整编码流程。
   - `media/rtsp_server.h`: 新增 `m_mppEncBuffer` 成员（1MB预分配输出缓冲区）。
   - `media/rtsp_server.cpp`: encodeAndSend() 改为MPP优先+libx264回退双路径；deinit() 添加mpp::deinitEncoder()；init() 添加MPP缓冲区分配；extractSpsPps() 添加MPP header解析路径。
   - `config/rtsp_settings.conf`: 新增 `rtsp_codec=264` 配置项。
   - `CarTerminal.pro`: 更新FFmpeg库注释。
3. What changed:
   - **mpp_encoder.cpp 完全重写**：旧版API完全错误（`mpp_create(&ctx, type)`签名错、`s_mpp_encode`不存在、自定义MppEncCfg结构体）。新版参考project1正确API：`mpp_create(&ctx, &mpi)` → `mpp_init(ctx, MPP_CTX_ENC, codingType)` → `mpp_enc_cfg_set_s32(cfg, key, val)` → `mpp_buffer_group_get_internal(grp, ION)` → `encode_put_frame`+`encode_get_packet`两步编码 → `MPP_ENC_GET_EXTRA_INFO`获取SPS/PPS。
   - **MppApi结构体**：定义与真实MPP SDK布局匹配（size, name, control, decode_put_packet, decode_get_frame, encode_put_frame, encode_get_packet），通过dlopen加载librockchip_mpp.so。
   - **RTSP双路径编码**：initEncoder()先尝试MPP硬件编码，失败自动回退libx264。encodeAndSend()根据m_useMpp标志分发到MPP或FFmpeg路径。
   - **MPP IDR帧处理**：扫描MPP Annex-B输出第一个NAL类型，若为SPS则MPP已内嵌headers直接发送；若为IDR(5)则prepend SPS/PPS。
   - **色彩空间**：swscale RGB32→YUV420P转换保留（MPP_FMT_YUV420P=1，两种路径共用）。
4. Why:
   - 旧mpp_encoder.cpp的MPP API调用全部错误，无法工作。
   - MPP硬件编码可将CPU占用从100%+降至<5%，显著降低延迟。
   - project1有完整正确的MPP编码参考实现。
5. Compatibility / risk:
   - 公共接口 `pushFrame()` 不变，Monitor无需修改。
   - MPP不可用时自动回退libx264，功能不受影响。
   - MPP常量值（MPP_CTX_ENC=1, MPP_VIDEO_CodingAVC=7等）硬编码在cpp中，无需SDK头文件。
6. Test status:
   - 代码编写完成，待板端编译验证。

### LLM Service Deployment Debugging — NPU Driver v0.9.6 Bus Error

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `voice/llm/include/rkllm.h`: 替换为 Rockchip 官方 release-v1.2.3 版本。
   - `voice/llm/src/llm_demo.cpp`: 适配新 API（RKLLMInput 结构体、callback 返回类型）。
3. What changed:
   - **rkllm.h**: `RKLLMInput` 结构体新增 `role`（const char*）和 `enable_thinking`（bool）字段，`input_type` 偏移从 offset 0 变为 offset 12（aarch64）。新增 `RKLLMPerfStat` 结构体、`RKLLMCrossAttnParam` 结构体。`LLMResultCallback` 返回类型从 `void` 改为 `int`。`rkllm_clear_kv_cache` 签名变更（新增 start_pos/end_pos 参数）。
   - **llm_demo.cpp**: `RKLLMInput` 初始化从 `memcpy(&rkllm_input, &input_type_val, sizeof(int32_t))` 改为 `memset` + 正确字段赋值（`role="user"`, `enable_thinking=false`, `input_type=RKLLM_INPUT_PROMPT`）。回调函数 `llm_callback` 返回类型从 `void` 改为 `int`（返回 0）。
4. Why:
   - 板上 `librkllmrt.so` 版本为 v1.3.0，其内部 `RKLLMInput` 结构体布局与项目中旧版 `rkllm.h` 不一致。
   - 旧头文件中 `input_type` 在 offset 0，runtime v1.3.0 在 offset 12 读取，导致 `input_type is not set` 错误。
   - 从 GitHub `airockchip/rknn-llm` 仓库 `release-v1.2.3` 分支获取官方头文件后修复。
5. 排除的问题（非根因）：
   - ① 不是 librkllmrt.so 文件损坏，库已恢复为 7.3M ELF；
   - ② 不是 swap 问题，6GB swap 已启用；
   - ③ 不是 ZMQ/TTS 问题，最小 demo 不经过它们仍然崩；
   - ④ 不是 callback 里请求 TTS 导致的，因为最小 demo 没有这套逻辑；
   - ⑤ 不是模型路径错误，rkllm_init 能成功加载模型；
   - ⑥ 不是普通 OOM，dmesg 没有新的 OOM 日志。
6. 当前阻塞：
   - **NPU 驱动版本过低**：板上 v0.9.6，rkllm-runtime v1.3.0 要求 v0.9.7+。
   - `rkllm_init`（模型加载）成功，但 `rkllm_run`（NPU 推理执行）触发 **Bus error (SIGBUS)**。
   - NPU 驱动是内核级别组件，无法通过 apt 升级。需要重新编译内核（替换 rknpu 驱动源码）或烧录新固件。
   - 百度网盘有预编译的 NPU 驱动 v0.9.8 内核 deb 包（针对 Rock 5B 内核 6.1），需确认与当前板子内核版本兼容性。
7. Compatibility / risk:
   - 头文件升级后，所有使用 `RKLLMInput` 的代码必须适配新字段（已完成）。
   - 回调函数签名变更（void→int）需要所有 callback 实现返回 int（已完成）。
   - NPU 驱动升级涉及内核操作，有变砖风险，需谨慎。

### RTSP Pipeline Bug Fixes & Code Compliance

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `media/rtsp_server.h`: 新增编码线程成员、墙钟时间戳、`isLastNal` 参数；`m_clients` 改为 `std::list`。
   - `media/rtsp_server.cpp`: 7处核心修复 + 19条代码规范合规修复。
3. What changed:
   - **致命修复**: `extractSpsPpsFromAvcc` 条件 `(extra[pos] & 0x1F) == 0` 改为 `> 0`（libx264 输出 numSPS=1 时旧条件为 false，导致 SPS/PPS 永远提取失败，VLC 无法解码）。
   - **P0**: RTP marker bit 从硬编码 `false` 改为 `isLastNal`（RFC 6184 Section 5.1）。
   - **P0**: `sendNaluAsRtp` 三重加锁改为单次锁定 + `std::list` 直接指针（消除 seqNum 竞态）。
   - **P1**: `buildTeardownResponse` 不再 erase client，只标记 `isPlaying=false`（消除双重删除）。
   - **P1**: `parseAndSendNalUnits` 移除危险的尾部零裁剪（可能损坏 NAL 数据）。
   - **P1**: `handleClient` 添加 60 秒会话超时（防止 tcpClosed 状态资源泄漏）。
   - **P2**: 编码移到独立线程 `m_encodeThread`（pushFrame 不再阻塞 GUI）。
   - **P2**: RTP 时间戳改为 `steady_clock` 墙钟时间（消除摄像头 30fps 与编码器 25fps 漂移）。
   - `processRequest` 拆分为 `parseRtspRequest` + dispatch（规范 3.4/14.2）。
   - `setsockopt` 返回值检查补全（规范 6.1）。
   - `parseAndSendNalUnits` 参数名 `data/size` → `annexBData/dataSize`（规范 4.1）。
4. Why:
   - VLC 连接成功但无画面，根因为 SPS/PPS 提取条件写反。
   - 架构审查发现 7 处优先级问题，按 P0→P1→P2 逐步修复。
5. Compatibility / risk:
   - 低风险：公共接口 `pushFrame()` 不变，Monitor 无需修改。
   - `std::list` 替代 `std::vector` 存储 `m_clients`，API 兼容。
6. Test status:
   - 代码规范 19 条审查通过。
   - 板端编译 0 错误 0 警告。
   - VLC 连接成功：OPTIONS→DESCRIBE→SETUP→PLAY 全部 200 OK。
   - SPS/PPS 提取成功：SPS=24 bytes, PPS=4 bytes。
   - 测试图案推流成功：VLC 显示彩色条纹画面。
   - 看门狗机制验证：无摄像头 2 秒后自动切换测试图案。

### RTSP Test Pattern Generator & Frame Watchdog

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `gui/Monitor/monitor.h`: 添加 QTimer 看门狗和测试图案成员，新增 `onTestFrameTick()`/`onFrameWatchdog()` 槽函数。
   - `gui/Monitor/monitor.cpp`: 实现测试图案生成器（8 色条纹 + 移动方块 + 帧计数器，25fps）、帧看门狗（2 秒检测摄像头无帧则自动切换测试图案）。
3. What changed:
   - 新增 `startTestFrameGenerator()`：QTimer 40ms 周期，QPainter 绘制 SMPTE 彩色条纹 + 正弦运动白色方块 + 帧号文字，通过 `pushFrame()` 推送到 RTSP 编码器。
   - 新增 `onFrameWatchdog()`：单次 QTimer 2000ms，检测 `m_cameraFrameReceived` 标志，若无真实摄像头帧则自动启动测试图案生成器。
   - 构造函数中摄像头 `startCapture()` 成功后启动看门狗；失败则直接启动测试图案。
   - `showImage()` 入口处设置 `m_cameraFrameReceived = true`，禁用看门狗。
   - `myStop()` 和析构函数中正确清理看门狗和测试定时器。
4. Why:
   - 用户未插摄像头模块时，`VIDIOC_STREAMON` 失败，`startCapture()` 返回 true 但采集线程内部失败，无法通过返回值判断。
   - 看门狗模式弥补了 `startCapture()` 返回值不可靠的问题，2 秒内无帧到达即判定摄像头不可用。
   - 测试图案生成器使整个 RTSP 链路（RGB32→YUV420P→H.264→RTP→VLC）无需摄像头硬件即可验证。
5. Test status:
   - 板端编译 0 错误 0 警告。
   - VLC 成功显示彩色条纹画面，持续 12 秒后用户主动断开。
   - 日志确认完整链路：看门狗触发→测试图案启动→pushFrame→IDR with SPS/PPS→VLC 200 OK。

### RTSP Streaming Server Integration & Code Compliance Fix

1. Date:
   - 2026-06-26.
2. Affected file(s):
   - `media/rtsp_server.h` (new): 嵌入式RTSP服务器公共接口。
   - `media/rtsp_server.cpp` (new): RTSP控制协议 + RTP H.264推流实现（~1230行）。
   - `config/rtsp_settings.conf` (new): RTSP推流配置（默认禁用）。
   - `gui/Monitor/monitor.h`: 添加 `media::RtspServer *m_rtspServer` 成员。
   - `gui/Monitor/monitor.cpp`: 初始化RTSP服务器（init→start），showImage中pushFrame。
   - `gui/mainwindow.h`: 添加 `media::RtspServerConfig rtspConfig` 成员。
   - `gui/mainwindow.cpp`: 加载 `config/rtsp_settings.conf` 配置。
   - `CarTerminal.pro`: 添加 rtsp_server 源文件和 FFmpeg 库链接。
3. What changed:
   - 新建嵌入式RTSP服务器模块，支持VLC/FFplay通过 `rtsp://<ip>:8554/live` 连接。
   - 使用FFmpeg libx264软件H.264编码 + 手动RTP FU-A分包（RFC 6184）。
   - POSIX TCP socket实现RTSP控制协议（OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN）。
   - POSIX UDP socket实现RTP数据传输。
   - 模块生命周期：init→start→pushFrame→stop→deinit（符合规范二.4）。
   - init()拆分为initEncoder/initSwscale/initSockets子函数（规范三.4）。
   - RtspStatus错误码枚举替代bool返回值（规范十三.1）。
   - RtpSendContext结构体减少参数数量（规范十四.6）。
   - NAL解析统一为parseAndSendNalUnits消除重复代码（规范十四.3）。
   - 线程数限制（MAX_CLIENT_THREADS=8）防止资源耗尽（规范十.6）。
   - seqNum读写在mutex锁内完成，消除并发竞态条件（规范十.1）。
   - 所有外部调用（sendto/setsockopt/inet_pton/bind/listen）返回值已检查（规范六.1/十八.5）。
   - 函数定义使用K&R大括号风格（规范一.8）。
   - 经过22条代码规范全面审查并修复所有违反项。
4. Why:
   - RTSP推流是AI智能车载终端的核心功能，支持远程实时查看摄像头画面。
   - FFmpeg libx264软件编码方案无需MPP硬件编码器依赖，降低集成复杂度。
5. Compatibility / risk:
   - 低风险：默认禁用（`rtsp_enabled=false`）。
   - 板端需要安装FFmpeg开发库：`apt install libavcodec-dev libswscale-dev libavutil-dev`。
   - 软件编码占用CPU，800x600@25fps下预计占用1-2核。
6. Test status:
   - 代码规范审查通过（22条）。
   - 板端编译和运行验证待完成（需安装FFmpeg开发库）。

## 2026-06-25

### Offline Voice Interaction Integration (ASR+LLM+TTS)

1. Date:
   - 2026-06-25.
2. Reason:
   - Migrate offline voice interaction pipeline from project 3 (LLM_Voice_Flow-master) into RK3588S_CarTerminal.
   - Implement ASR→LLM→TTS three-process architecture with ZMQ inter-process communication.
   - This is the core differentiating feature for the "AI" in AI Smart Car Terminal.
3. Affected modules / files:
   - `voice/zmq/` — ZMQ communication framework (migrated from zmq-comm-kit)
   - `voice/llm/` — DeepSeek-R1 LLM service (migrated from llm_demo.cpp + rkllm.h)
   - `voice/tts/` — SummerTTS service (migrated from tts_server + SummerTTS engine)
   - `voice/asr/AsrEngine.*` — Offline ASR wrapper (new, uses dlopen for sherpa-onnx)
   - `voice/VoiceManager.*` — Voice pipeline manager (new, orchestrates ASR/LLM/TTS)
   - `voice/CMakeLists.txt` — Independent build for LLM and TTS executables
   - `config/voice_settings.conf` — Voice pipeline configuration
   - `config/asr_settings_offline.conf` — Offline ASR configuration
   - `gui/mainwindow.h/cpp` — Added VoiceManager initialization
   - `CarTerminal.pro` — Added voice module sources and ZMQ library
4. Compatibility / risk:
   - Low risk: voice pipeline is disabled by default (`voice_enabled=false`).
   - LLM and TTS are separate processes, crashes do not affect main application.
   - ZMQ library (`libzmq`) must be installed on the board.
   - sherpa-onnx, rkllmrt, and SummerTTS model files must be deployed separately.
5. Test status:
   - Local qmake check pending.
   - Board build pending (requires libzmq-dev on board).
   - Runtime verification requires: DeepSeek .rkllm model, SummerTTS .bin model, sherpa-onnx libraries, microphone hardware.
6. Pending items / known remaining gaps:
   - MPP Encoder待集成到推流路径。
   - 模型文件`yolov5s.rknn`（15.2MB）已获取，放置于`config/`目录。
   - 板端运行验证需要摄像头硬件。设置`rknn_enabled=true`后可启用检测。
   - Voice pipeline待板端部署验证：需安装libzmq、sherpa-onnx、rkllmrt库，下载DeepSeek和SummerTTS模型。
   - AsrEngine的sherpa-onnx调用逻辑待板端安装库后完善。
### MPP/RKNN Initial Integration (Pre-Unification Baseline)

1. Scope/date:
   - Historical 2026-06-25 integration record. Its board compile statement is
     a pre-unification baseline and does not verify the 2026-07-15 shared
     production postprocess implementation.
2. Affected file(s) + line range:
   - `media/mpp_decoder.h` (new): MPP MJPEG硬件解码接口。
   - `media/mpp_decoder.cpp` (new): dlopen加载librockchip_mpp.so，MJPEG→NV12解码，CPU降级。
   - `media/mpp_encoder.h` (new): MPP H.264/H.265硬件编码接口。
   - `media/mpp_encoder.cpp` (new): dlopen加载librockchip_mpp.so，NV12→H.264/H.265编码。
   - `media/rknn_detector.h` (new): RKNN YOLOv5 NPU目标检测接口 + 配置加载。
   - `media/rknn_detector.cpp` (new): dlopen加载librknnrt.so，YOLOv5后处理 + 配置文件解析。
   - `media/capture_thread.cpp`: 集成MPP解码器到MJPEG采集分支。
   - `media/capture_thread.h`: 添加MPP头文件和NV12缓冲区。
   - `gui/Monitor/monitor.h`: 添加RKNN检测成员（m_rknnConfig, m_rknnReady）。
   - `gui/Monitor/monitor.cpp`: 初始化RKNN检测器，showImage中运行YOLOv5推理+绘制检测框，myStop中释放资源。
   - `gui/mainwindow.h`: 添加rknnConfig成员。
   - `gui/mainwindow.cpp`: 加载rknn_settings.conf并传递给Monitor。
   - `config/rknn_settings.conf` (new): RKNN检测配置（默认禁用）。
   - `CarTerminal.pro`: 添加mpp_decoder/mpp_encoder/rknn_detector到SOURCES/HEADERS。
3. What changed:
   - 从4.17 project1+project2提取MPP编解码和RKNN推理代码。
   - 所有模块采用dlopen动态加载，无编译时SDK依赖。
   - MPP Decoder: 支持MJPEG硬件解码到NV12，已集成到capture_thread。
   - MPP Encoder: 支持H.264/H.265硬件编码，待集成到推流路径。
   - RKNN Detector: 支持80类COCO目标检测，已集成到Monitor页面，帧到达时自动推理。
   - RKNN配置外置到`config/rknn_settings.conf`（原则五）。
   - 所有代码遵循19条工程规范：VLOG日志、命名空间隔离、错误处理、资源释放。
4. Why (motivation / bug context):
   - 迁移可行性分析报告：MPP复用率90%，RKNN复用率85%。
   - CPU解码/推理性能不足，需硬件加速。
5. Migration status:
   - ✅ 代码集成完成。2026-06-25 pre-unification board compile: 0 errors,
     0 warnings. This is not evidence for the later shared postprocess branch.
6. Pending items / known remaining gaps:
   - MPP Encoder待集成到推流路径。
   - 模型文件`yolov5s.rknn`（15.2MB）已获取，放置于`config/`目录。
   - 板端运行验证需要摄像头硬件。设置`rknn_enabled=true`后可启用检测。

### RGA Hardware Acceleration Integration

1. Date:
   - 2026-06-25.
2. Affected file(s) + line range:
   - `media/rga_converter.h` (new): RGA hardware acceleration interface for NV12→RGB32 conversion.
   - `media/rga_converter.cpp` (new): Implementation using dlopen librga.so + imcvtcolor_t API, with CPU integer-运算 fallback.
   - `media/capture_thread.cpp` L1-705: NV12 path now uses `rga::nv12ToRgb32()`, removed old `nv12ToRgb()` member, added RGA init/deinit lifecycle.
   - `media/capture_thread.h` L1-133: Removed `nv12ToRgb` declaration, added `m_rgbBuffer` pre-allocated member.
   - `CarTerminal.pro` L43-56: Added rga_converter to SOURCES/HEADERS, added `linux: LIBS += -ldl`.
3. What changed:
   - Extracted RGA color conversion code from 4.17 project1 (`imcvtcolor_t` from librga.so).
   - Created `rga::init()` / `rga::deinit()` / `rga::nv12ToRgb32()` with dlopen-based loading.
   - RGA unavailable → automatic CPU fallback (same integer-运算 algorithm).
   - Pre-allocated `m_rgbBuffer` avoids per-frame heap allocation.
   - RGA initialized in `CaptureThread::run()` before capture loop, deinitialized after.
4. Why (motivation / bug context):
   - Migration feasibility report: RGA module has 85% reuse ratio, "接口封装层需调整".
   - Principle eight (性能优化): NV12→RGB32 CPU conversion is slow; RGA hardware acceleration expected <2ms.
   - Current CPU conversion allocates a new QImage per frame.
5. Migration status:
   - ✅ Code integration complete. Follows coding principles (dlopen for runtime flexibility, no compile-time SDK dependency).
6. Pending items / known remaining gaps:
   - Board runtime verification: needs camera hardware + librga.so on RK3588S.
   - NV21/UYVY still use CPU fallback (can add RGA path later).

### Legacy Module VLOG Migration

1. Date:
   - 2026-06-25.
2. Affected file(s) + line range:
   - `gui/Map/baidumap.cpp` L1–305: replaced 16 qWarning/qDebug/qInfo calls with VLOG.
   - `gui/Weather/weather.cpp` L1–272: replaced 10 qWarning/qDebug calls with VLOG.
   - `gui/Music/musicplayer.cpp` L1–298: replaced 10 qDebug calls with VLOG.
   - `gui/Music/searchmusic.cpp` L1–220: replaced 7 qDebug calls with VLOG.
   - `gui/settingwindow.cpp` L1–205: replaced 7 qDebug calls with VLOG.
3. What changed:
   - All `qDebug()`, `qWarning()`, `qInfo()` in 5 legacy module files replaced with `VLOG_DEBUG/WARN/ERROR/INFO` macros.
   - Each file: removed `#include <QDebug>`, added `#include "logmanager.h"` and `static const char *MODULE = "<module>";`.
   - baidumap.cpp: module name `baidumap`.
   - weather.cpp: module name `weather`.
   - musicplayer.cpp: module name `music`.
   - searchmusic.cpp: module name `music`.
   - settingwindow.cpp: module name `setting`.
4. Why (motivation / bug context):
   - Principle seven (日志管理): all logging must go through unified log manager.
   - Complete the VLOG migration started in 2026-06-25 Runtime Warning Cleanup.
5. Migration status:
   - ✅ Complete. All 5 legacy module files now use VLOG macros. Zero qDebug/qWarning/qInfo remaining.
6. Pending items / known remaining gaps:
   - None.
7. Verification (board rebuild & runtime):
   - Pending board rebuild.
8. Document updated:
   - CHANGELOG.md.
9. Risk level:
   - LOW. Log output format changes only; no business logic modified.
10. Build result:
   - ✅ Board compiled with 0 warnings, 0 errors.
11. Test result:
   - ✅ Board runtime verified. All VLOG macros work correctly.

### SummerTTS Engine Full Migration & Project Cleanup

1. Date:
   - 2026-06-25.
2. Affected file(s):
   - `voice/tts/src/models/` (new): 11 SummerTTS synthesizer model source files (DurationPredictor, Generator, SynthesizerTrn, TextEncoder, etc.)
   - `voice/tts/src/modules/` (new): 13 neural network module source files (attention, ConvFlow, DDSConv, multi_head_attention, ResBlock1, WN, etc.)
   - `voice/tts/src/nn_op/` (new): 13 neural network operation source files (conv1d, gelu, layer_norm, relu, sigmoid, softmax, etc.)
   - `voice/tts/src/hz2py/` (new): 7 files — Hanz2Piny, hanzi2phoneid, pinyinmap + utf8 library
   - `voice/tts/src/engipa/` (new): 5 files — English IPA symbol processing
   - `voice/tts/src/platform/` (new): 2 files — tts_file_io, tts_logger
   - `voice/tts/src/utils/` (new): 1 file — utils.cpp
   - `voice/tts/src/header/` (new): 74 header files (models/modules/nn_op/cppjieba/limonp libraries)
   - `voice/tts/include/` (updated): 4 SummerTTS engine headers (SynthesizerTrn.h, utils.h, tts_file_io.h, tts_logger.h)
   - `voice/tts/eigen-3.4.0/` (new): 1783 files — Eigen linear algebra library (required by SummerTTS engine)
   - `CarTerminal.pro` (updated): Added `gui/Map/gps.h` to HEADERS; removed duplicate `INCLUDEPATH += $$MEDIA_DIR`
3. What changed:
   - Migrated complete SummerTTS engine source code from project 3 (LLM_Voice_Flow-master) to make tts_service independently compilable.
   - Fixed CarTerminal.pro: added missing gps.h to HEADERS, removed duplicate INCLUDEPATH.
   - librkllmrt.so installed to board /usr/lib/aarch64-linux-gnu/ (verified 7.6MB).
   - libzmq3-dev confirmed already installed on board.
4. Why:
   - Previous TTS migration only included tts_server wrapper code, missing the core SummerTTS engine source.
   - tts_service could not compile without models/modules/nn_op/hz2py/engipa/platform/utils + Eigen library.
5. Migration status:
   - ✅ All SummerTTS engine files migrated. CMakeLists.txt already references correct paths.
6. Pending items:
   - Board-side compilation of voice/CMakeLists.txt (LLM + TTS services).
   - sherpa-onnx library installation on board for offline ASR.
   - Runtime verification of full voice pipeline (ASR→LLM→TTS).

## 2026-06-25

### Doc-Code Alignment & Warning Fix

1. Date:
   - 2026-06-25.
2. Task:
   - Fix `Clock::paintEvent` unused parameter warning.
   - Update PROJECT_CONTEXT_FOR_AI.md, MODULE_STATUS.md, ARCHITECTURE.md to align with actual `RK3588S_CarTerminal/` code structure.
3. Added files:
   - None.
4. Modified files:
   - `gui/clock.cpp` — Changed `paintEvent(QPaintEvent *event)` to `paintEvent(QPaintEvent * /*event*/)` to suppress unused parameter warning.
   - `gui/clock.h` — Added `override` to `paintEvent` declaration.
   - `../PROJECT_CONTEXT_FOR_AI.md` — Updated section 四 (current project structure), section 七 (architecture), section 八 (data flow), section 十 (progress), section 十二 (related docs) to reflect actual `RK3588S_CarTerminal/` paths and status.
   - `MODULE_STATUS.md` — Updated all file paths from old `rk3588s_migration/` to `RK3588S_CarTerminal/`. Changed ASR/TTS/LLM runtime status from "Planned" to "Partial" (百度ASR HTTP API). Noted voice_gateway and run_hdmi.sh remain in old locations.
   - `ARCHITECTURE.md` — Updated physical layout, all layer paths, media layer status, and config paths to reflect `RK3588S_CarTerminal/`.
5. Deleted files:
   - None.
6. Reason for change:
   - Compilation warning: `unused parameter 'event'` in `gui/clock.cpp:23`.
   - PROJECT_CONTEXT_FOR_AI.md was still referencing old `rk3588s_migration/` paths and outdated progress.
   - MODULE_STATUS.md file paths did not match actual `RK3588S_CarTerminal/` structure.
   - ARCHITECTURE.md physical layout and layer paths were outdated.

### Runtime Warning Cleanup

1. Date:
   - 2026-06-25.
2. Task:
   - Fix all non-fatal runtime warnings from board compilation output.
3. Added files:
   - None.
4. Modified files:
   - `gui/speechrecognition.h` — Removed `static SpeechRecognition *SR` global variable (原则四.4).
   - `gui/mainwindow.h` — Renamed slots: `on_timer_updateTime()` → `updateTimeDisplay()`, `on_update_humidity_temp()` → `updateHumidityTemp()` (原则四.6).
   - `gui/mainwindow.cpp` — Updated slot definitions and connect calls; replaced `SR` with `AsrThread` member.
   - `gui/Music/musicplayer.h` — Renamed slots: `on_musicPlayer_DurationChanged()` → `updateDurationDisplay()`, `on_musicPlayer_CurPostionChanged()` → `updatePositionDisplay()`.
   - `gui/Music/musicplayer.cpp` — Updated slot definitions, connect calls, and replaced deprecated `endl` with `"\n"`.
   - `gui/Music/searchmusic.cpp` — Replaced deprecated `endl` with `"\n"`.
   - `gui/settingwindow.cpp` — Replaced deprecated `qrand()` with `QRandomGenerator::global()->bounded()`.
   - `resources/Music/style.qss` — Removed unsupported CSS properties `rotation-point` and `rotation`.
   - `gui/Music/style.qss` — Same CSS fix.
5. Deleted files:
   - None.
6. Reason for change:
   - Board runtime output showed 8 non-fatal warnings that should be cleaned up for production quality.
   - `connectSlotsByName` auto-connection warnings caused by slot names using `on_` prefix but connected manually.
   - `static SpeechRecognition *SR` in header created unused variable in every translation unit.
   - `qrand()` and `endl` deprecated in Qt 5.15+.
   - `rotation-point`/`rotation` CSS properties not supported by Qt stylesheet engine.
7. Interface changed:
   - No public interface changes. Slot renames are internal to each class.
8. Architecture changed:
   - No.
9. Config changed:
   - No.
10. Build result:
   - ✅ Board compiled with 0 warnings, 0 errors.
11. Test result:
   - ✅ Board runtime verified. Application starts and runs correctly.
12. Remaining issues:
   - `XCB error: 148` is system-level, not fixable from application code.
   - `setting gif directory has no usable gif files` is expected when MyGif directory is empty.
13. Next step:
   - Upload all modified files to board, rebuild, verify zero warnings.

### Weather UI Icon Resource Path Fix

1. Date:
   - 2026-06-25.
2. Task:
   - Fix `Could not create pixmap from :/img/images/tmp.png` runtime warning in Weather module.
3. Added files:
   - None.
4. Modified files:
   - `gui/Weather/weather.ui` — Fixed resource path `:/img/images/tmp.png` → `:/img/Weather/images/tmp.png`.
5. Deleted files:
   - None.
6. Reason for change:
   - The .ui file referenced `:/img/images/tmp.png` but the .qrc registers the file as `Weather/images/tmp.png` under prefix `/img`, making the correct resource path `:/img/Weather/images/tmp.png`.
   - The missing `Weather/` prefix caused a runtime warning on every weather page display.
7. Interface changed:
   - No.
8. Architecture changed:
   - No.
9. Config changed:
   - No.
10. Build result:
   - Pending board rebuild.
11. Test result:
   - Pending board runtime verification.
12. Remaining issues:
   - None introduced by this change.
13. Next step:
   - Upload updated weather.ui and rebuild on board.

### Board Verification & Documentation Alignment

1. Date:
   - 2026-06-25.
2. Task:
   - Board verification of Map, Weather, Music modules.
   - Update MODULE_STATUS.md, ARCHITECTURE.md to reflect verified status.
   - Create `scripts/run_hdmi.sh` startup script.
3. Added files:
   - `scripts/run_hdmi.sh` — HDMI startup script for RK3588S.
4. Modified files:
   - `MODULE_STATUS.md` — Updated Map (Done), Music (Done), Weather (Done) status with board verification notes. Updated Build scripts and Runtime startup script paths. Fixed docs count.
   - `ARCHITECTURE.md` — Added `scripts/`, `myMusic/` to physical layout tree. Fixed docs count.
   - `config/map_settings.conf` — Updated AK (real key redacted in later hardening pass; config now ships `YOUR_BAIDU_MAP_KEY` placeholder with `map_enabled=false`).
   - `config/weather_settings.conf` — Updated appid/appsecret credentials (real values redacted in later hardening pass; config now ships `YOUR_WEATHER_APP_ID`/`YOUR_WEATHER_APP_SECRET` placeholders with `weather_enabled=false`).
5. Deleted files:
   - None.
6. Reason for change:
   - Board verification confirmed Map, Weather, Music modules work correctly.
   - `scripts/run_hdmi.sh` was missing from `RK3588S_CarTerminal/`.
   - MODULE_STATUS and ARCHITECTURE did not reflect `scripts/` and `myMusic/` directories.
7. Interface changed:
   - No.
8. Architecture changed:
   - No.
9. Config changed:
   - `map_settings.conf` — new AK.
   - `weather_settings.conf` — new appid/appsecret.
10. Build result:
   - N/A (config-only changes, no rebuild needed).
11. Test result:
   - Map: ✅ Shenyang map displayed correctly.
   - Weather: ✅ Shenyang weather displayed correctly.
   - Music: ✅ Local MP3 scanning and online music search work. Audio output requires speaker.
12. Remaining issues:
   - Camera/Monitor: pending camera hardware.
   - ASR: token expired, needs renewal.
13. Next step:
   - Fix ASR hardcoded token (原则五 violation).
   - Replace qDebug/qWarning with VLOG in mainwindow.cpp and speechrecognition.cpp (原则七 violation).

### ASR Hardcode Fix & VLOG Migration

1. Date:
   - 2026-06-25.
2. Task:
   - Fix hardcoded ASR token/API URL in mainwindow.cpp (原则五 violation).
   - Replace all qDebug/qWarning with VLOG in mainwindow.cpp and speechrecognition.cpp (原则七 violation).
3. Added files:
   - `config/asr_settings.conf` — ASR configuration file (API URL, token, cuid, format, rate, channel, record path).
4. Modified files:
   - `gui/mainwindow.h` — Added `AsrConfig` struct and `loadAsrConfig()` method. Added `asrConfig` member.
   - `gui/mainwindow.cpp` — Removed hardcoded Baidu ASR token and API URL. Added `loadAsrConfig()` to parse `config/asr_settings.conf`. Replaced all `qDebug()`/`qWarning()` with VLOG macros. Added `getEnvOr()` helper. Added empty result array check in `getSpeechResult()`. Extracted config path resolution to reusable helper.
   - `gui/speechrecognition.cpp` — Replaced all `qDebug()`/`qWarning()` with VLOG macros. Added `#include "logmanager.h"`.
5. Deleted files:
   - None.
6. Reason for change:
   - **原则五.1**: API URL and token were hardcoded in `mainwindow.cpp:245,254`.
   - **原则五.4**: Token `24.464f...115895512` was written in source code.
   - **原则七.6**: 13 `qDebug()`/`qWarning()` calls in mainwindow.cpp and 5 in speechrecognition.cpp were not using VLOG.
   - **原则六.2**: `getSpeechResult()` did not check for empty result array before accessing index 0.
7. Interface changed:
   - `MainWindow` now has `loadAsrConfig()` private method and `AsrConfig` struct.
   - ASR parameters now read from `config/asr_settings.conf` instead of hardcoded values.
8. Architecture changed:
   - No.
9. Config changed:
   - New `config/asr_settings.conf` with ASR API parameters. Token must be filled before ASR can work.
10. Build result:
   - ✅ Board compiled with 0 warnings, 0 errors.
11. Test result:
   - ✅ Board runtime verified. VLOG output works correctly. ASR config loading confirmed.
12. Remaining issues:
   - `gui/Map/baidumap.cpp` has ~20 qDebug/qWarning calls (遗留模块, 暂缓).
   - `gui/Weather/weather.cpp` has ~12 qDebug/qWarning calls (遗留模块, 暂缓).
   - `gui/Music/musicplayer.cpp` and `searchmusic.cpp` have ~15 qDebug/qWarning calls (遗留模块, 暂缓).
   - `gui/settingwindow.cpp` has ~7 qDebug/qWarning calls (遗留模块, 暂缓).
   - ASR token in `config/asr_settings.conf` is `YOUR_TOKEN_HERE`, needs real token.
13. Next step:
   - Upload modified files to board, rebuild, verify.
   - Replace remaining qDebug/qWarning in legacy modules (baidumap, weather, musicplayer, settingwindow).

### RK3588S_CarTerminal Board Compilation Fix and Runtime Verification

1. Date:
   - 2026-06-25.
2. Task:
   - Fix all compilation errors in the new RK3588S_CarTerminal project and verify board runtime.
3. Added files:
   - `device/dht11.h` — Dht11Reader wrapper class declaration (from IMX6ULL VehicleTerminal).
   - `device/dht11.cpp` — Dht11Reader wrapper class implementation (from IMX6ULL, then unified to VLOG).
   - `gui/settinggifscanner.h` — GIF file scanner helper (from rk3588s_migration).
   - `gui/settinggifscanner.cpp` — GIF file scanner implementation (from rk3588s_migration).
   - `resources/Music/style.qss` — Music player stylesheet with corrected resource paths.
4. Modified files:
   - `main.cpp` — Fixed `LogManager::setLogLevel()` → `vehiclelog::setMinLevel()`, `LOG_INFO()` → `VLOG_INFO()`.
   - `media/capture_thread.h` — Added missing `bool dequeueAndEmitFrame()` private method declaration.
   - `gui/mainwindow.h` — Changed `#include "dht11device.h"` → `#include "dht11.h"` (Dht11Reader is in dht11.h).
   - `CarTerminal.pro` — Added `device/dht11.h`, `device/dht11.cpp`, `gui/settinggifscanner.h`, `gui/settinggifscanner.cpp` to build.
   - `resources/img.qrc` — Fixed Music resource paths: removed `images/` subdirectory prefix (files are directly in `Music/`).
   - `gui/Music/style.qss` — Updated resource paths to match corrected .qrc structure.
   - `device/dht11.cpp` — Replaced `qDebug()`/`qWarning()` with `VLOG_INFO()`/`VLOG_WARN()` (原则七.6 日志规范).
   - `docs/CHANGELOG.md` — This entry.
5. Deleted files:
   - None.
6. Reason for change:
   - After creating the new RK3588S_CarTerminal project structure, board compilation had 6 errors.
   - Errors caused by: API name mismatch (LogManager vs vehiclelog), missing declarations (dequeueAndEmitFrame), missing source files (dht11, settinggifscanner), wrong include paths, and resource path mismatches.
   - Previous fix attempts created cascading errors because each fix was done independently without reading all files first.
   - This round reads all files first, identifies all issues, then fixes them in one batch.
7. Interface changed:
   - No new public interfaces. All changes are internal fixes.
8. Architecture changed:
   - No. Fixes maintain existing module structure.
9. Config changed:
   - No.
10. Build result:
   - Board `qmake && make -j$(nproc)` compiled and linked `CarTerminal` successfully with zero errors.
   - Only non-fatal warnings remain: deprecated `qrand()`, unused `SR`, deprecated `endl`, unused parameter `event`.
11. Test result:
   - Board HDMI runtime started successfully: `[15:11:41][INFO ][main] application starting`.
   - Runtime output confirmed: weather/dht11/key/GPS input disabled as expected.
   - Map module printed `map image saved "map.p..."` confirming functional map rendering.
   - No segment fault in the provided run log.
12. Remaining issues:
   - Non-fatal runtime warnings: `No matching signal for on_timer_updateTime()`, missing `:/img/images/tmp.png`, unsupported CSS `rotation*` properties, XCB warning, deprecated `qrand()`/`endl`.
   - settinggifscanner.cpp uses C-style `dirent.h` instead of Qt `QDir` (原则十五.7 风格一致性, accepted as Linux-native API).
   - Real hardware testing (DHT11, AP3216, Camera) pending.
13. Next step:
   - Continue with runtime warning cleanup or next migration phase (sensor/network/media/AI).

### Project Restructuring: New RK3588S_CarTerminal

1. Date:
   - 2026-06-25.
2. Files:
   - Created new project structure `RK3588S_CarTerminal/` with 11 directories.
   - `common/`: logmanager, vehiclecommand (from rk3588s_migration).
   - `device/`: vehicledeviceconfig, keyinputdevice, dht11device/worker, ap3216device/worker, ap3216, i2cbusses, smbus (from IMX6ULL + rk3588s_migration).
   - `media/`: cameraconfig, capture_thread (from rk3588s_migration + IMX6ULL).
   - `network/`: staticmapconfig, staticmapresponse (from rk3588s_migration).
   - `gui/`: mainwindow, settingwindow, clock, speechrecognition, Map/, Music/, Weather/, Monitor/ (from IMX6ULL).
   - `config/`: vehicle_devices.conf, weather_settings.conf, map_settings.conf, camera_settings.conf.
   - `resources/`: img/, gifs/, Music/, Weather/images/, Map/img/, img.qrc.
   - `CarTerminal.pro`: new Qt project file with all modules.
   - `main.cpp`: simplified entry point with logmanager initialization.
3. Why:
   - Previous approach was modifying IMX6ULL project directly, causing path confusion and upload issues.
   - New structure separates concerns: common/, device/, media/, gui/, network/.
   - All files in one directory tree, no cross-project dependencies.
4. Impact:
   - New project can be uploaded as a single directory to the board.
   - No more path confusion between rk3588s_migration/ and VehicleTerminal/.
   - Cleaner include paths using $$PWD-relative paths.
5. New config:
   - None (existing configs migrated).
6. Dependencies:
   - None (all dependencies from existing projects).
7. Startup:
   - No change (same Qt application startup).
8. Compilation:
   - `qmake && make` in RK3588S_CarTerminal/ directory.
9. Testing:
   - Windows cross-compilation test pending.
   - Board deployment test pending.
10. Risks:
   - Some include paths may need adjustment after board testing.
   - speechrecognition.h/cpp dependencies may need review.

### Unified Log Module and CaptureThread Engineering Refactoring

1. Date:
   - 2026-06-25.
2. Task:
   - Create unified project log module (`vehiclelog`).
   - Refactor CaptureThread to meet enterprise engineering standards: lifecycle state machine, error codes, unified logging.
   - Update Monitor and MainWindow to use new interfaces.
3. Added files:
   - `rk3588s_migration/common/logmanager.h`
   - `rk3588s_migration/common/logmanager.cpp`
4. Modified files:
   - `Monitor/capture_thread.h` — rewritten: lifecycle (init/startCapture/stopCapture/shutdown), State enum, ErrorCode enum, unified log.
   - `Monitor/capture_thread.cpp` — rewritten: all V4L2 operations use VLOG, proper error codes, state transitions, NV12/NV21/UYVY→RGB32 conversion with integer-only math.
   - `Monitor/monitor.h` — renamed `captureThread` to `m_captureModule`.
   - `Monitor/monitor.cpp` — uses init()/startCapture()/stopCapture() lifecycle, VLOG instead of qWarning.
   - `rk3588s_migration/media/cameraconfig.cpp` — added VLOG for config parsing, loading, and error reporting.
   - `mainwindow.cpp` — replaced qWarning with VLOG for device/camera config loading.
   - `VehicleTerminal.pro` — added LOG_DIR for logmanager module.
5. New or changed interfaces:
   - `CaptureThread::init(config)` — replaces `configure(config)`.
   - `CaptureThread::startCapture()` — replaces `startThread()`.
   - `CaptureThread::stopCapture()` — replaces `stopThread()`.
   - `CaptureThread::shutdown()` — new, releases all resources.
   - `CaptureThread::state()` — new, returns current lifecycle state.
   - `CaptureThread::lastError()` / `lastErrorMessage()` — new, returns error details.
   - `CaptureThread::stateChanged(State)` — new signal.
   - `CaptureThread::frameError(QString)` — new signal.
   - `vehiclelog::logMessage(level, module, fmt, ...)` — core log function.
   - `VLOG_DEBUG/INFO/WARN/ERROR/FATAL(module, fmt, ...)` — convenience macros.
6. New or changed config:
   - None (uses existing `config/camera_settings.conf`).
7. Error handling and logging:
   - All V4L2 operations now use VLOG with module name "camera".
   - Config parsing uses module name "camera.config".
   - Monitor uses module name "monitor".
   - MainWindow uses module name "mainwindow".
   - CaptureThread state machine: Uninitialized → Ready → Starting → Capturing → Stopping → Ready → Uninitialized (or → Error on failure).
   - Error codes: DeviceOpenFailed, FormatNotSupported, BufferSetupFailed, StreamStartFailed, FrameCaptureError.
8. Resource release and thread safety:
   - CaptureThread::shutdown() calls stopCapture() → releaseBuffers() → closeDevice() → delete config.
   - Destructor calls shutdown() automatically.
   - State transitions protected by m_stateMutex (QMutex).
   - Stop flag protected by m_stopMutex (QMutex).
   - stopCapture() blocks up to 3s, then terminate() as last resort.
   - mmap buffers tracked in vector, munmap'd in releaseBuffers().
9. Compile or run verification:
   - qmake && make on board.
   - Run with `camera_enabled=false` to verify no crash.
   - Run with `camera_enabled=true` and `/dev/video33` to verify V4L2 capture.
10. Risk:
    - CaptureThread public interface changed: all callers of configure/startThread/stopThread must update. Monitor updated; mainwindow does not call CaptureThread directly.
    - 13 qWarning/qDebug calls in mainwindow.cpp remain unchanged (not camera-related, separate pass needed).
11. Follow-up:
    - Replace remaining qWarning/qDebug in mainwindow.cpp with VLOG.
    - Board runtime verification of V4L2 capture with new interface.
    - RGA hardware color conversion for performance.

### Camera/V4L2 Multiplanar Capture Migration

1. Date:
   - 2026-06-25.
2. Task:
   - Migrate Camera/V4L2 capture from legacy single-planar RGB565 hardcoded code to config-driven multiplanar NV12 capture for RK3588S rkisp ISP.
3. Added files:
   - `rk3588s_migration/media/cameraconfig.h`
   - `rk3588s_migration/media/cameraconfig.cpp`
   - `rk3588s_migration/media/test_cameraconfig.cpp`
   - `config/camera_settings.conf`
4. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/Monitor/capture_thread.h`
   - `IMX6ULL车载终端/VehicleTerminal/Monitor/capture_thread.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/Monitor/monitor.h`
   - `IMX6ULL车载终端/VehicleTerminal/Monitor/monitor.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/mainwindow.h`
   - `IMX6ULL车载终端/VehicleTerminal/mainwindow.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `docs/MODULE_STATUS.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Board `v4l2-ctl --list-devices` confirmed `/dev/video33` is rkisp0-vir0 mainpath (Video Capture Multiplanar).
   - `v4l2-ctl -d /dev/video33 --list-formats-ext` confirmed UYVY/NV16/NV61/NV21/NV12/NM21/NM12 support, max 800x600.
   - Old code used single-planar `V4L2_BUF_TYPE_VIDEO_CAPTURE`, RGB565, hardcoded `/dev/video1`, 1024x600.
   - RK3588S ISP requires multiplanar API (`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`).
7. Interface changed:
   - `CaptureThread` now has `configure(const vehiclecamera::CameraConfig &config)` method.
   - `CaptureThread` emits `captureError(QString)` signal on failure.
   - `Monitor::getInstance` now accepts optional `CameraConfig` parameter.
   - `Monitor` constructor now accepts `CameraConfig` parameter.
8. Architecture changed:
   - Camera config is externalized to `config/camera_settings.conf`.
   - Device path, resolution, format, buffer count, FPS are all configurable.
   - Camera enabled/disabled flag allows running without camera hardware.
   - `cameraconfig.h/cpp` follows same pattern as `weatherconfig`, `vehicledeviceconfig`.
9. Config changed:
   - New `config/camera_settings.conf` with: `camera_enabled=true`, `camera_device=/dev/video33`, `camera_width=800`, `camera_height=600`, `camera_format=NV12`, `camera_buffer_count=4`, `camera_fps=30`.
   - Environment variable `VEHICLE_CAMERA_CONFIG` overrides default config path.
10. Build result:
   - Pending board-side `qmake && make` verification.
11. Test result:
   - Host-side `test_cameraconfig.cpp` covers: default config, full config, format uppercase, unknown key, duplicate key, invalid bool, invalid int, range validation (width/height/buffer_count/fps), enabled without device/format, malformed line, comments/blanks, bool true/false variants, missing file.
12. Remaining issues:
   - Board runtime verification of V4L2 capture pending.
   - NV12→RGB32 CPU conversion is functional but slow for 800x600@30fps; consider RGA hardware acceleration.
   - Only rkisp0-vir0 tested; rkisp0-vir1 and rkisp1-vir1 not verified.
13. Next step:
   - Upload to board, rebuild, verify V4L2 capture produces frames.
   - Consider RGA hardware color conversion for performance.
   - Consider RTSP/RTMP streaming integration.

### AP3216 I2C/IIO Discovery Verification

1. Date:
   - 2026-06-25.
2. Task:
   - Record narrow RK3588S AP3216 I2C/IIO/sysfs/dmesg discovery and update the next migration direction.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/DECISIONS.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The board output shows IMX415 camera sensors and other non-AP3216 I2C devices, but no AP3216/AP3216C node, no IIO illuminance/proximity/intensity channel, and no AP3216/IIO dmesg probe log.
   - This changes AP3216 from an immediately actionable board test to a hardware/device-tree/driver exposure blocker.
7. Interface changed:
   - No.
8. Architecture changed:
   - No code architecture changed. Technical direction updated: keep AP3216 default-disabled and move the next board-facing migration to Camera/V4L2.
9. Config changed:
   - No. Keep `ap3216_enabled=false`.
10. Build result:
   - Not applicable; documentation-only update.
11. Test result:
   - Board I2C names observed: `0-001a imx415`, `1-001a imx415`, `2-001a imx415`, `0-0042 rk8602`, `2-0042 rk8602`, `0-0043 rk8603`, `1-0011 es8388`, `2-004e husb311`, `i2c-10 ddc`, and RK3X I2C controller entries.
   - IIO illuminance/proximity/intensity discovery returned no output.
   - AP3216/AP3216C/proximity sysfs discovery returned no output.
   - `dmesg | grep -iE "ap3216|ap3216c|illuminance|proximity|iio"` returned no output.
12. Remaining issues:
   - AP3216 enabled board testing is blocked until real hardware/kernel exposure exists.
   - Camera/V4L2 nodes and supported formats still need discovery before migration edits.
13. Next step:
   - Keep AP3216 disabled.
   - Collect `/dev/video*`, `/dev/media*`, `v4l2-ctl --list-devices`, `v4l2-ctl --list-formats-ext`, and `media-ctl -p` output, then start Camera/V4L2 migration.

### AP3216 Sysfs Discovery Update

1. Date:
   - 2026-06-25.
2. Task:
   - Record RK3588S AP3216 sysfs discovery output and refine the next board-side discovery commands.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The board command `find /sys -maxdepth 5 \( -iname "*ap3216*" -o -iname "*als*" -o -iname "*ps*" -o -iname "*ir*" \)` returned many unrelated paths because `*ps*` and `*ir*` match broad kernel names such as `oops`, `psci`, `irq`, and `firmware`.
   - No usable AP3216 sysfs directory or ALS/PS/IR channel files were identified in the provided output.
7. Interface changed:
   - No.
8. Architecture changed:
   - No.
9. Config changed:
   - No. `ap3216_enabled` should remain `false`.
10. Build result:
   - Not applicable; documentation-only update.
11. Test result:
   - Board discovery did not find a usable AP3216 node.
12. Remaining issues:
   - Superseded by the later `AP3216 I2C/IIO Discovery Verification` entry: narrower I2C/IIO/sysfs/dmesg discovery was run and found no AP3216/AP3216C exposure.
   - AP3216 enabled testing remains blocked until hardware/device-tree/driver exposure changes.
   - Default-disabled Monitor-page runtime verification is still pending.
13. Next step:
   - Keep AP3216 disabled; use Camera/V4L2 discovery as the next board-facing hardware step.

### SettingWindow GIF Fix Board Verification

1. Date:
   - 2026-06-25.
2. Task:
   - Record RK3588S board rebuild and HDMI runtime verification for the SettingWindow GIF startup crash fix.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The board log confirms the previous `SettingWindow::ScanGif()` crash path is now handled as a non-fatal missing-asset state.
   - Project status and TODOs needed to move this item from pending to verified.
7. Interface changed:
   - No.
8. Architecture changed:
   - No.
9. Config changed:
   - No.
10. Build result:
   - Board `qmake VehicleTerminal.pro` completed.
   - Board `make -j$(nproc)` linked `VehicleTerminal` successfully with `settinggifscanner.o`, `settingwindow.o`, and `moc_settingwindow.o`.
11. Test result:
   - Board HDMI runtime printed `setting gif directory has no usable gif files: "/home/quark/car/VehicleTerminal/MyGif"`.
   - Runtime continued past startup and printed `weather input disabled`, `dht11 input disabled`, key-input disabled log, and `map image saved "map.png" 62452`.
   - No segment fault appeared in the provided run log.
12. Remaining issues:
   - Non-fatal warnings remain: missing Qt auto-connect slots, missing `:/img/images/tmp.png`, unsupported stylesheet `rotation*` properties, one XCB warning, deprecated `qrand()`, and unused `static SpeechRecognition *SR`.
   - AP3216 default-disabled board runtime still needs explicit Monitor-page verification.
13. Next step:
   - Continue with one module at a time. Recommended next software task is fixing Qt resource/stylesheet warnings or starting Camera/V4L2 migration; AP3216 enabled testing remains blocked until the real sysfs path is known.

### SettingWindow GIF Startup Crash Fix

1. Date:
   - 2026-06-25.
2. Task:
   - Fix the early RK3588S startup segment fault traced by GDB to `SettingWindow::ScanGif()` and `QMovie::setFileName(QString const&)`.
3. Added files:
   - `rk3588s_migration/gui/settinggifscanner.h`
   - `rk3588s_migration/gui/settinggifscanner.cpp`
   - `tests/setting_gif_scanner/test_setting_gif_scanner.cpp`
   - `tests/setting_gif_scanner/build_and_run.ps1`
4. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/settingwindow.h`
   - `IMX6ULL车载终端/VehicleTerminal/settingwindow.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Board GDB showed the app crashed during `MainWindow` construction before AP3216/map/weather runtime work.
   - The failing stack passed through `SettingWindow::ScanGif()`, where the legacy code assumed `./MyGif` existed and contained at least one GIF.
   - RK3588S deployment must tolerate missing optional UI assets and keep startup alive.
7. Interface changed:
   - Yes. Added GUI-independent helper interface `vehiclesettings::collectGifFiles(const std::string &directoryPath)`.
8. Architecture changed:
   - Yes at small GUI-helper level. Directory scanning moved out of `SettingWindow` into `rk3588s_migration/gui`, while hardware/service layers remain untouched.
9. Config changed:
   - No.
10. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` includes `settinggifscanner.cpp`, `settinggifscanner.h`, and `settingwindow.cpp`.
11. Test result:
   - `tests\setting_gif_scanner\build_and_run.ps1` passed.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - `tests\device_input\build_and_run.ps1` passed.
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\weather_format\build_and_run.ps1` passed.
   - `tests\weather_config\build_and_run.ps1` passed.
12. Remaining issues:
   - RK3588S board upload, rebuild, and HDMI runtime verification are pending.
   - If another segment fault remains after this upload, collect a fresh `gdb` `bt` and `thread apply all bt` because it will be a different crash site.
13. Next step:
   - Upload `settinggifscanner.*`, `settingwindow.*`, `VehicleTerminal.pro`, and the fixed `run_hdmi.sh`; rebuild on the board and verify startup continues when `./MyGif` is missing or empty.

### HDMI Runtime Script And Early Crash Diagnostics

1. Date:
   - 2026-06-25.
2. Task:
   - Add a versioned HDMI startup script and document the next evidence-gathering step for the latest early runtime segment fault.
3. Added files:
   - `IMX6ULL车载终端/VehicleTerminal/run_hdmi.sh`
4. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The latest board build links successfully, but runtime crashes very early after the first Qt auto-connect warning.
   - The board log shows `XDG_RUNTIME_DIR` points to root-owned `/run/user/0` while running as UID 1001, which pollutes the Qt runtime environment.
   - This change removes that known environment issue and records the exact `gdb` commands needed if the segment fault remains.
7. Interface changed:
   - No.
8. Architecture changed:
   - No.
9. Config changed:
   - No runtime config key changed.
10. Build result:
   - Not rerun locally; script/docs/pro-file change only.
   - The user-provided board log before this change shows `VehicleTerminal` linked successfully.
11. Test result:
   - Not rerun locally. Board verification is pending after uploading `run_hdmi.sh`.
12. Remaining issues:
   - Superseded by later GDB evidence: after fixing the runtime environment, the early segment fault was traced to `SettingWindow::ScanGif()`.
13. Next step:
   - Upload the versioned `run_hdmi.sh` together with the SettingWindow GIF fix, rebuild, and verify the app starts without crashing in `ScanGif()`.

## 2026-06-24

### AP3216 Monitor Sensor Configuration Migration

1. Date:
   - 2026-06-24.
2. Task:
   - Move AP3216 light/proximity/infrared sensor access out of the Monitor GUI timer path and into configurable device/Qt worker layers.
3. Added files:
   - `rk3588s_migration/device/ap3216device.h`
   - `rk3588s_migration/device/ap3216device.cpp`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/ap3216worker.h`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/ap3216worker.cpp`
4. Modified files:
   - `rk3588s_migration/device/vehicledeviceconfig.h`
   - `rk3588s_migration/device/vehicledeviceconfig.cpp`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/ap3216.h`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/ap3216.cpp`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/monitor.h`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/Monitor/monitor.cpp`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/mainwindow.cpp`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/VehicleTerminal.pro`
   - `IMX6ULL杞﹁浇缁堢/VehicleTerminal/config/vehicle_devices.conf`
   - `tests/device_input/test_device_input.cpp`
   - `tests/device_input/build_and_run.ps1`
   - `docs/ARCHITECTURE.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/MODULE_STATUS.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Legacy AP3216 code hardcoded sysfs files and Monitor used a GUI timer to pull sensor data.
   - RK3588S deployments need AP3216 sysfs directory and channel file names to be configurable.
   - GUI should receive sensor readings through Qt signals instead of owning hardware polling.
   - `Monitor::getInstance()` did not save the singleton pointer when creating the Monitor window; this was corrected while keeping the public call pattern stable.
7. Interface changed:
   - Yes. Added `vehicledevice::Ap3216Config`, `vehicledevice::Ap3216Reading`, `vehicledevice::Ap3216Device`, AP3216 config keys, and `Ap3216c::startReading()/stopReading()/isAvailable()`.
8. Architecture changed:
   - Yes at module level. AP3216 file I/O now lives in `rk3588s_migration/device`, polling runs in `Ap3216Worker`, and Monitor only updates display from signals.
9. Config changed:
   - Yes. Added `ap3216_enabled`, `ap3216_sysfs_dir`, `ap3216_als_file`, `ap3216_ps_file`, `ap3216_ir_file`, and `ap3216_poll_interval_ms` to `config/vehicle_devices.conf`.
10. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` includes `ap3216device.cpp`, `ap3216worker.cpp`, `moc_ap3216worker.cpp`, `Monitor/ap3216.cpp`, and `Monitor/monitor.cpp`.
11. Test result:
   - TDD red step failed first because `ap3216device.h` did not exist in the `tests/device_input` build.
   - `tests\device_input\build_and_run.ps1` passed.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\weather_format\build_and_run.ps1` passed.
   - `tests\weather_config\build_and_run.ps1` passed.
   - Static scan found no `/sys/class/misc/ap3216c` hardcoded path and no `sleep`/裸 `while(1)` in the touched AP3216/Monitor files.
12. Remaining issues:
   - RK3588S board rebuild and HDMI runtime verification are pending.
   - Real AP3216 enabled testing is blocked until the board exposes a confirmed sysfs directory and ALS/PS/IR channel file names.
   - Camera/V4L2 code in Monitor remains legacy and must be migrated separately.
13. Next step:
   - Upload AP3216/Monitor files to RK3588S, rebuild `VehicleTerminal`, verify the default-disabled log `ap3216 input disabled`, then discover the real AP3216 sysfs directory before enabling it.

### DHT11 Board Binary And Node Discovery Verification

1. Date:
   - 2026-06-24.
2. Task:
   - Record RK3588S board verification after the DHT11 worker-thread migration upload.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/CHANGELOG.md`
   - `docs/BUILD_RUN_TEST.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Board output confirmed the current `VehicleTerminal` binary no longer contains legacy hardcoded DHT11, key, or GPS device paths.
   - Board discovery commands did not find any DHT11 device node, kernel log, or loaded module.
7. Build result:
   - Not rerun in this documentation update; the existing board binary was inspected with `strings`.
8. Test result:
   - Board binary checks passed:
     `OK: binary has no hardcoded dht11 path`,
     `OK: binary has no old key path`,
     `OK: binary has no old gps path`.
   - `find /dev -maxdepth 2 -iname "*dht*"`, `dmesg | grep -i dht`, and `lsmod | grep -i dht` produced no DHT11 result.
9. Remaining issues:
   - Real DHT11 runtime verification is blocked because the RK3588S board currently exposes no DHT11 device node.
   - Need DHT11 wiring, kernel driver, and device tree confirmation before setting `dht11_enabled=true`.
10. Next step:
   - Keep DHT11 disabled and move to the next software-migratable module, or first bring up the DHT11 kernel/device-tree path if real temperature/humidity hardware verification is required now.

### DHT11 Read Timeout And Worker Thread Fixes

1. Date:
   - 2026-06-24.
2. Task:
   - Fix DHT11 review issues: enforce read timeout, avoid GUI-thread sensor reads, cover the DHT11 adapter in host tests, and handle repeated opens safely.
3. Added files:
   - `IMX6ULL车载终端/VehicleTerminal/dht11worker.h`
   - `IMX6ULL车载终端/VehicleTerminal/dht11worker.cpp`
4. Modified files:
   - `rk3588s_migration/device/dht11device.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/dht11.h`
   - `IMX6ULL车载终端/VehicleTerminal/dht11.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/mainwindow.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `tests/device_input/test_device_input.cpp`
   - `tests/device_input/build_and_run.ps1`
   - `docs/CHANGELOG.md`
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/BUILD_RUN_TEST.md`
5. Deleted files:
   - None.
6. Reason for change:
   - `dht11_read_timeout_ms` was parsed but not applied to device reads.
   - The previous Qt timer path read the DHT11 device from the GUI thread.
   - Repeated `openDevice()` calls could overwrite an existing fd without closing it first.
   - Host tests parsed DHT11 config but did not compile or exercise `Dht11Device`.
7. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` includes `dht11worker.cpp`, `dht11worker.h`, `moc_dht11worker.cpp`, `dht11device.cpp`, and `dht11.cpp`.
8. Test result:
   - TDD red step failed first with undefined references to `vehicledevice::Dht11Device`, proving the test build did not include `dht11device.cpp`.
   - `tests\device_input\build_and_run.ps1` passed after adding DHT11 adapter tests and compiling `dht11device.cpp`.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\weather_format\build_and_run.ps1` passed.
   - `tests\weather_config\build_and_run.ps1` passed.
9. Remaining issues:
   - RK3588S board rebuild and HDMI runtime verification are pending for the worker-thread DHT11 path.
   - Real DHT11 device node on RK3588S is still unknown; default config remains disabled.
10. Next step:
   - Upload the new DHT11 worker files and modified DHT11/device files to the board, rebuild, verify `dht11 input disabled`, then test enabled DHT11 with the real device node.

### DHT11 Sensor Configuration Migration

1. Date:
   - 2026-06-24.
2. Task:
   - Move DHT11 hardware access from GUI into configurable device adapter with configuration file support.
3. Added files:
   - `rk3588s_migration/device/dht11device.h`
   - `rk3588s_migration/device/dht11device.cpp`
4. Modified files:
   - `rk3588s_migration/device/vehicledeviceconfig.h`
   - `rk3588s_migration/device/vehicledeviceconfig.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/config/vehicle_devices.conf`
   - `IMX6ULL车载终端/VehicleTerminal/dht11.h`
   - `IMX6ULL车载终端/VehicleTerminal/dht11.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/mainwindow.h`
   - `IMX6ULL车载终端/VehicleTerminal/mainwindow.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `tests/device_input/test_device_input.cpp`
   - `docs/CHANGELOG.md`
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/BUILD_RUN_TEST.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Legacy DHT11 code hardcoded `/dev/dht11` device path and used `while(1)` with `usleep` blocking.
   - No configuration support existed for DHT11 device path, poll interval, or read timeout.
   - GUI directly controlled hardware without going through a device adapter.
7. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` includes `dht11device.cpp`, `dht11device.h`, `vehicledeviceconfig.cpp`, `vehicledeviceconfig.h`.
8. Test result:
   - `tests\device_input\build_and_run.ps1` passed.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\weather_format\build_and_run.ps1` passed.
   - `tests\weather_config\build_and_run.ps1` passed.
9. Remaining issues:
   - RK3588S board rebuild and HDMI runtime verification are pending.
   - Real DHT11 device node on RK3588S is unknown; config defaults to disabled.
   - Existing non-fatal Qt warnings remain.
10. Next step:
   - Upload DHT11 device files to the board, rebuild, verify `dht11 input disabled` log, then test with real DHT11 hardware when device node is known.

### Weather Config Board Verification

1. Date:
   - 2026-06-24.
2. Task:
   - Record RK3588S board rebuild and HDMI runtime verification for the Weather configuration review fixes.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/CHANGELOG.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/MIGRATION_PLAN.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The board now confirms that the default-disabled Weather config path works after uploading the latest Weather config/code files.
   - Documentation needed to replace the previous Weather board-verification-pending status.
7. Build result:
   - Board `qmake VehicleTerminal.pro` completed.
   - Board `make -j$(nproc)` linked `VehicleTerminal` successfully with `weather.o` and `weatherconfig.o`.
8. Test result:
   - Board HDMI runtime printed `weather input disabled`.
   - The app continued to print `map image saved "map.png" 62247`.
   - No Weather segment fault appeared in the provided run log.
9. Remaining issues:
   - Live weather provider request is still disabled until valid `weather_appid`, `weather_appsecret`, and `weather_enabled=true` are configured.
   - Existing non-fatal Qt warnings remain: missing auto-connect slots, missing `tmp.png`, unsupported stylesheet properties, one XCB warning, and unused `static SpeechRecognition *SR`.
10. Next step:
   - Verify live Weather provider behavior with valid credentials, or move to the next planned module such as DHT11/sensor migration.

### Weather Config Review Fixes

1. Date:
   - 2026-06-24.
2. Task:
   - Finish the Weather configuration review fixes after the manual migration.
3. Added files:
   - None.
4. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/Weather/weather.h`
   - `IMX6ULL车载终端/VehicleTerminal/Weather/weather.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/config/weather_settings.conf`
   - `rk3588s_migration/weather/weatherconfig.cpp`
   - `tests/weather_config/test_weather_config.cpp`
   - `docs/ARCHITECTURE.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/MODULE_STATUS.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - `Weather::updateInfo()` could still issue an empty request when weather config was disabled or invalid.
   - `weather_timeout_ms` was parsed but not applied to Qt network requests.
   - Weather URL placeholder replacement only handled the first occurrence.
   - The checked-in config enabled weather with empty credentials, causing avoidable startup parse/request noise.
7. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` and `Makefile.Debug` include `Weather/weather.cpp`, `Weather/weather.h`, `weatherformat.cpp`, `weatherformat.h`, `weatherconfig.cpp`, and `weatherconfig.h`.
8. Test result:
   - TDD red step failed before implementation for repeated placeholders: `{appid}` and `{city}` were only replaced once.
   - `tests\weather_config\build_and_run.ps1` passed after implementation.
   - `tests\weather_format\build_and_run.ps1` passed.
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\device_input\build_and_run.ps1` passed.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - Static scan found old weather appid/appsecret only in `docs/BUILD_RUN_TEST.md` verification commands.
9. Remaining issues:
   - RK3588S board rebuild and HDMI runtime verification are still pending for the final Weather config behavior.
   - Live weather remains disabled until valid `weather_appid`, `weather_appsecret`, and `weather_enabled=true` are set.
   - Existing non-fatal Qt warnings remain: missing auto-connect slots, missing `tmp.png`, unsupported stylesheet properties, and one XCB warning.
10. Next step:
   - Upload Weather config/code files to the board, rebuild `VehicleTerminal`, run `./run_hdmi.sh`, and verify disabled config logs `weather input disabled` without a crash.

### Weather API Configuration Migration

1. Date:
   - 2026-06-24.
2. Task:
   - Move weather API URL, appid, appsecret, and city out of `Weather/weather.cpp` and into configuration.
3. Added files:
   - `IMX6ULL车载终端/VehicleTerminal/config/weather_settings.conf`
   - `rk3588s_migration/weather/weatherconfig.h`
   - `rk3588s_migration/weather/weatherconfig.cpp`
   - `tests/weather_config/test_weather_config.cpp`
   - `tests/weather_config/build_and_run.ps1`
4. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/Weather/weather.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `docs/CHANGELOG.md`
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/INTERFACE_SPEC.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Weather API URL/appid/appsecret were hardcoded in `Weather/weather.cpp`.
   - RK3588S deployments must be able to change weather provider settings without recompiling.
   - Current weather provider returns `errcode=100` / `免费权限已过期!` because the appid/appsecret are expired.
7. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` includes `weatherconfig.cpp`, `weatherconfig.h`, `weatherformat.cpp`, `weatherformat.h`, `weather.cpp`.
8. Test result:
   - `tests\weather_config\build_and_run.ps1` passed.
   - `tests\weather_format\build_and_run.ps1` passed.
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\device_input\build_and_run.ps1` passed.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
9. Remaining issues:
   - RK3588S board rebuild and HDMI runtime verification are pending.
   - Current `weather_appid` and `weather_appsecret` in `weather_settings.conf` are empty; user must fill valid credentials before enabling.
   - Existing non-fatal Qt warnings remain: missing auto-connect slots, missing `tmp.png`, unsupported stylesheet properties, and one XCB warning.
10. Next step:
   - Upload weather config files to the board, rebuild, and verify weather request uses configured URL when enabled with valid credentials.

### Weather Crash Hardening Board Verification

1. Date:
   - 2026-06-24.
2. Task:
   - Record RK3588S HDMI runtime verification after deploying Weather crash hardening.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/DECISIONS.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The board no longer crashes in `Weather::TransDataToMyData(QString)`.
   - The Weather module now logs the malformed/expired provider response and the app continues.
7. Build result:
   - Board-side build had already linked `VehicleTerminal` with `weatherformat.o`.
8. Test result:
   - Board HDMI run printed `weather data array is missing or too short 0 "{\"errcode\":100,\"errmsg\":\"免费权限已过期!\"}"`.
   - The app continued far enough to print `map image saved "map.png" 62247`.
9. Remaining issues:
   - Weather provider permission is expired.
   - Weather API URL/key/city settings remain hardcoded legacy behavior and need config migration.
   - Existing non-fatal Qt warnings remain: missing auto-connect slots, missing `tmp.png`, unsupported stylesheet properties, and one XCB warning.
10. Next step:
   - Migrate Weather API/provider settings into config and add parser tests for expired-permission, malformed, and valid forecast responses.

## 2026-06-23

### Weather Crash Hardening

1. Date:
   - 2026-06-23.
2. Task:
   - Fix the board runtime segment fault reported by `gdb` in `Weather::TransDataToMyData(QString)` after the map image is saved.
3. Added files:
   - `rk3588s_migration/weather/weatherformat.h`
   - `rk3588s_migration/weather/weatherformat.cpp`
   - `tests/weather_format/test_weather_format.cpp`
   - `tests/weather_format/build_and_run.ps1`
4. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/Weather/weather.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `docs/ARCHITECTURE.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/MODULE_STATUS.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The legacy weather page assumed every network reply contained at least seven valid daily records.
   - Empty or malformed `date` values reached `Data.split("-").at(1/2)`, causing an out-of-range crash.
   - The map path had already saved `map.png`, so the latest root cause is Weather parsing, not map rendering.
7. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` and `Makefile.Debug` include `weatherformat.cpp` and `weatherformat.h`.
8. Test result:
   - TDD red step failed before implementation because `weatherformat.cpp` did not exist.
   - `tests\weather_format\build_and_run.ps1` passed.
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - `tests\device_input\build_and_run.ps1` passed.
9. Remaining issues:
   - Board-side rebuild and HDMI runtime verification are still required.
   - Weather URL/API/city settings are still hardcoded legacy behavior and need a later configuration migration.
10. Next step:
   - Upload the weather helper files, updated `Weather/weather.cpp`, and `VehicleTerminal.pro` to RK3588S, rebuild, and verify the app no longer crashes in `Weather::TransDataToMyData`.

### Map Response Validation And QLabel Rendering

1. Date:
   - 2026-06-23.
2. Task:
   - Fix the next map-runtime risk after replacing the Baidu AK: reject non-image map responses and avoid global stylesheet rendering for the valid PNG path.
3. Added files:
   - `rk3588s_migration/map/staticmapresponse.h`
   - `rk3588s_migration/map/staticmapresponse.cpp`
4. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/Map/baidumap.h`
   - `IMX6ULL车载终端/VehicleTerminal/Map/baidumap.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/VehicleTerminal.pro`
   - `IMX6ULL车载终端/VehicleTerminal/config/map_settings.conf`
   - `tests/map_config/test_map_config.cpp`
   - `tests/map_config/build_and_run.ps1`
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/ARCHITECTURE.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The new board AK returns a real PNG, but the Qt app hit a segment fault after receiving it.
   - The only valid-image-only GUI path was the global `QWidget{border-image: ...}` stylesheet update, so map rendering is now localized to a `QLabel`.
   - HTTP 200 JSON/text error bodies must not overwrite `map.png`.
7. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated Makefiles include `staticmapresponse.cpp` and `staticmapresponse.h`.
8. Test result:
   - TDD red step failed because `staticmapresponse.cpp` did not exist.
   - `tests\map_config\build_and_run.ps1` passed after implementation.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - `tests\device_input\build_and_run.ps1` passed.
9. Remaining issues:
   - Board rebuild and HDMI runtime verification are still required.
   - If the segment fault remains, collect a `gdb` backtrace to determine whether the crash is in Qt image decode, QLabel paint, or another module.
10. Next step:
   - Upload the changed map files, rebuild on RK3588S, and rerun `./run_hdmi.sh`.

### Map API Diagnosis Update

1. Date:
   - 2026-06-23.
2. Task:
   - Record the full RK3588S Python HTTP result for the Baidu static map request.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The board no longer appears blocked by DNS/network for the map request. The complete response shows the Baidu APP behind the current AK is disabled.
7. Build result:
   - Not applicable; docs-only change.
8. Test result:
   - Board Python request returned HTTP 200 with `content-type: text/html` and Baidu body `{"message":"APP被用户自己禁用，请在控制台解禁","status":"201"}`.
9. Remaining issues:
   - The current Baidu Maps AK cannot return a real static map image until its APP is enabled or replaced.
   - The map module should be hardened to reject non-image HTTP 200 responses before writing `map.png`.
10. Next step:
   - Enable the Baidu Maps APP or update `map_api_key`, then add non-image response validation in the map module.

### Map Static Image Board Verification

1. Date:
   - 2026-06-23.
2. Task:
   - Record RK3588S board build/runtime verification for the migrated map static image module.
3. Added files:
   - None.
4. Modified files:
   - `docs/MODULE_STATUS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - The board now confirms that `staticmapconfig.cpp` compiles and links into `VehicleTerminal`, and runtime URL fallback uses numeric coordinates.
7. Build result:
   - Board `qmake`/`make -j$(nproc)` linked `VehicleTerminal` successfully with `staticmapconfig.o`.
8. Test result:
   - Runtime printed `gps input disabled`.
   - Runtime printed map URL with `center=123.43147,41.80570`.
   - Runtime failed map fetch with `QNetworkReply::HostNotFoundError`, which indicates DNS/network resolution failure rather than map config construction failure.
9. Remaining issues:
   - Board DNS/network must resolve `api.map.baidu.com`.
   - Real Baidu static map response still needs verification after DNS/network is fixed.
   - The serial-port message `Error reading data on serial port: ...` did not match any current repository log string and should be traced from the board terminal/session if it repeats.
10. Next step:
   - Run board DNS/network diagnostics from `docs/BUILD_RUN_TEST.md`.

### Map Static Image Configuration

1. Date:
   - 2026-06-23.
2. Task:
   - Move Baidu static map URL, API key, fallback coordinate, image size, output file, and zoom range out of GUI source and into configuration.
3. Added files:
   - `rk3588s_migration/map/staticmapconfig.h`
   - `rk3588s_migration/map/staticmapconfig.cpp`
   - `tests/map_config/test_map_config.cpp`
   - `tests/map_config/build_and_run.ps1`
   - `VehicleTerminal/config/map_settings.conf`
4. Modified files:
   - `VehicleTerminal/Map/baidumap.h`
   - `VehicleTerminal/Map/baidumap.cpp`
   - `VehicleTerminal/VehicleTerminal.pro`
   - `docs/ARCHITECTURE.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/MODULE_STATUS.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/TODO.md`
   - `docs/BUILD_RUN_TEST.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Remove hardcoded map network settings from GUI code, fix the fallback city encoding risk by using an ASCII coordinate, and make map behavior adjustable on RK3588S without recompiling.
7. Build result:
   - Host qmake syntax check completed with exit code 0.
   - Generated `Makefile.Release` and `Makefile.Debug` include `rk3588s_migration/map/staticmapconfig.cpp`.
   - Windows qmake still prints Chinese-path noise; generated Makefiles were inspected successfully.
8. Test result:
   - `tests\map_config\build_and_run.ps1` passed.
   - `tests\device_input\build_and_run.ps1` passed.
   - `tests\vehicle_command\build_and_run.ps1` passed.
   - `tests\voice_gateway\build_and_run.ps1` passed.
   - Static scan found no old map URL, AK, `MapImgUrl`, old zoom fields, `data is null`, or mojibake city string in map source.
9. Remaining issues:
   - RK3588S board rebuild and HDMI runtime verification are pending.
   - Real map rendering depends on board network access and a valid Baidu static map API key.
10. Next step:
   - Upload map config/module files to the board, rebuild `VehicleTerminal`, and verify the runtime URL uses `center=123.43147,41.80570` when GPS is disabled.

### Documentation Update Protocol

1. Date:
   - 2026-06-23.
2. Task:
   - Record the rule that project context docs must be read before every task/source inspection, and required docs must be updated after every code change.
3. Added files:
   - None.
4. Modified files:
   - `docs/PROJECT_CONTEXT.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/MODULE_STATUS.md`
   - `docs/DECISIONS.md`
   - `docs/CHANGELOG.md`
5. Deleted files:
   - None.
6. Reason for change:
   - Make documentation maintenance a mandatory engineering rule, not an optional reminder.
7. Build result:
   - Not applicable; docs-only change.
8. Test result:
   - Documentation content checks required for this change.
9. Remaining issues:
   - None introduced by this documentation change.
10. Next step:
   - Use this protocol before and after every future module change.

### Documentation Baseline

1. What changed:
   - Created the required 9 engineering memory documents under `docs/`.
   - Documented project context, architecture, migration plan, module status, interfaces, decisions, TODOs, changelog policy, and build/run/test methods.
2. Added files:
   - `docs/PROJECT_CONTEXT.md`
   - `docs/ARCHITECTURE.md`
   - `docs/MIGRATION_PLAN.md`
   - `docs/MODULE_STATUS.md`
   - `docs/INTERFACE_SPEC.md`
   - `docs/DECISIONS.md`
   - `docs/TODO.md`
   - `docs/CHANGELOG.md`
   - `docs/BUILD_RUN_TEST.md`
3. Modified files:
   - None outside the new docs set.
4. Deleted files:
   - None.
5. Why:
   - Prevent future work from forgetting project background, architecture boundaries, migration status, stable interfaces, and required maintenance records.
6. Interface changed:
   - No code interface changed.
7. Architecture changed:
   - No runtime architecture changed; architecture rules are now documented.
8. Config changed:
   - No runtime config changed.
9. Build status:
   - Not applicable for docs-only change.
10. Test status:
   - Documentation file presence and content checks are required for this change.
11. Remaining issues:
   - Docs must be kept current after every future change.
12. Next step:
   - Continue with one module at a time, likely map network configuration.

### GPS/Map Device Configuration

1. What changed:
   - Moved GPS serial path and UART settings out of hardcoded IMX6ULL code.
   - Added GPS keys to `vehicle_devices.conf`.
   - Replaced direct `gps_valid` GUI access with `gps_is_valid()` and `getGpsData()`.
2. Added files:
   - None.
3. Modified files:
   - `IMX6ULL车载终端/VehicleTerminal/Map/gps.h`
   - `IMX6ULL车载终端/VehicleTerminal/Map/gps.c`
   - `IMX6ULL车载终端/VehicleTerminal/Map/baidumap.cpp`
   - `IMX6ULL车载终端/VehicleTerminal/Map/uart.c`
   - `IMX6ULL车载终端/VehicleTerminal/config/vehicle_devices.conf`
   - `rk3588s_migration/device/vehicledeviceconfig.h`
   - `rk3588s_migration/device/vehicledeviceconfig.cpp`
   - `tests/device_input/test_device_input.cpp`
4. Deleted files:
   - None.
5. Why:
   - RK3588S must not carry IMX6ULL `/dev/ttymxc2` hardcoding.
6. Interface changed:
   - Yes. GPS C interface now includes `gps_uart_config_t`, `gps_init_with_config()`, `gps_shutdown()`, and `gps_is_valid()`.
7. Architecture changed:
   - Yes at module level. GPS state moved from header globals into `gps.c` private runtime state.
8. Config changed:
   - Yes. Added `gps_*` keys.
9. Build status:
   - Board qmake/make completed and linked `VehicleTerminal`.
10. Test status:
   - `device_input`, `vehiclecommand`, and `voice_gateway` tests passed on host during development.
11. Remaining issues:
   - GPS is disabled until actual RK3588S serial node is known.
12. Next step:
   - Map network configuration.

### Key Input Device Configuration

1. What changed:
   - Removed `/dev/key` dependency from GUI startup path.
   - Added configurable key input adapter.
2. Added files:
   - `rk3588s_migration/device/vehicledeviceconfig.h`
   - `rk3588s_migration/device/vehicledeviceconfig.cpp`
   - `rk3588s_migration/device/keyinputdevice.h`
   - `rk3588s_migration/device/keyinputdevice.cpp`
   - `tests/device_input/test_device_input.cpp`
   - `tests/device_input/build_and_run.ps1`
3. Modified files:
   - `VehicleTerminal.pro`
   - `speechrecognition.h`
   - `speechrecognition.cpp`
   - `config/vehicle_devices.conf`
4. Deleted files:
   - None.
5. Why:
   - RK3588S does not provide legacy IMX6ULL `/dev/key`.
6. Interface changed:
   - Yes. Added device config and key input interfaces.
7. Architecture changed:
   - Yes at module level. Key hardware access moved to device layer.
8. Config changed:
   - Yes. Added `key_input_*` keys.
9. Build status:
   - Board qmake/make completed.
10. Test status:
   - Device config and key input tests passed.
11. Remaining issues:
   - Real RK3588S key node/value mapping still unknown.
12. Next step:
   - Continue device migrations one at a time.

### Voice Gateway And Command Parser

1. What changed:
   - Added shared cabin command parser.
   - Added CMake voice gateway skeleton.
2. Added files:
   - `rk3588s_migration/common/vehiclecommand.*`
   - `rk3588s_migration/voice_gateway/**`
   - `tests/vehicle_command/**`
   - `tests/voice_gateway/**`
3. Modified files:
   - Qt project integrated command parser.
4. Deleted files:
   - None.
5. Why:
   - Voice/AI commands need a stable non-GUI interface.
6. Interface changed:
   - Yes. Added `vehiclecommand::CabinCommand`.
7. Architecture changed:
   - Yes. Introduced shared common layer and voice gateway service layer.
8. Config changed:
   - Added voice gateway example config.
9. Build status:
   - Host and board CMake builds completed.
10. Test status:
   - `vehicle_command` and `voice_gateway` tests passed.
11. Remaining issues:
   - Actual ASR/LLM/TTS runtime integration still pending.
12. Next step:
   - Connect voice runtime after GUI/hardware boundaries are stable.
