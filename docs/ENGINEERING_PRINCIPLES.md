# 企业级嵌入式工程原则

**适用范围：** 本项目所有代码迁移、重构、新增、修改操作。
**执行要求：** 每次修改代码前必须先阅读本文件；每次修改代码后必须在末尾"修改记录"章节追加本次修改的合规审查记录。

---

## 一、总体原则

1. 优先保持项目整体架构清晰，不允许为了快速实现功能而把代码写乱。
2. 尽量沿用原开源项目中成熟、稳定、可复用的代码逻辑，不要无意义重写。
3. 迁移代码时必须先理解原模块职责，再决定是复用、封装、裁剪还是重构。
4. 不允许把多个模块强行拼接在一起，必须保持高内聚、低耦合。
5. 所有新增代码必须与现有项目风格统一，保证整个项目像一个人写的。
6. 不允许为了炫技引入复杂设计，优先使用简单、稳定、可读、可维护的实现。
7. 修改范围必须可控，不允许大面积无关重构。
8. 每次修改后必须整理修改内容、影响范围和后续注意事项。

## 二、模块设计原则

1. 一个模块只负责一类明确功能，例如语音模块、地图模块、摄像头模块、串口模块、显示模块、配置模块、日志模块。
2. 模块之间不能互相乱调用，统一由 AppController、ModuleManager 或事件机制进行调度。
3. 模块内部实现可以复杂，但对外接口必须简单清晰。
4. 每个核心模块建议至少提供以下接口：
   - `init()`：初始化模块
   - `start()`：启动模块
   - `stop()`：停止模块
   - `shutdown()`：释放资源
   - `getStatus()`：获取模块状态
   - `handleCommand()` 或 `process()`：处理业务输入
5. 平台相关代码、硬件相关代码、业务逻辑代码必须分离。
6. 驱动层只负责硬件访问，服务层只负责功能封装，应用层只负责任务调度。
7. 不允许在 UI、语音、地图、摄像头等模块中互相直接写死调用关系。

## 三、代码结构原则

1. `main.cpp` 必须保持简洁，只负责创建 App 对象、初始化、运行和退出。
2. 不允许把配置读取、日志初始化、硬件操作、业务逻辑、UI 刷新全部堆在 `main.cpp` 中。
3. 一个文件只负责一个主要类或一个明确模块。
4. 一个函数只做一件事，函数过长必须拆分。
5. 避免嵌套过深，优先使用提前返回、错误前置判断、guard clause。
6. 避免连续多层 if-else，可使用状态机、策略函数、命令映射表或独立处理函数拆分逻辑。
7. 不允许出现巨大函数、巨大类、巨大 switch 或难以维护的嵌套逻辑。
8. 公共工具类放在 `common/` 或 `utils/` 中，业务模块放在 `modules/` 中，应用调度放在 `app/` 中。

## 四、命名规范原则

1. 命名必须表达真实含义，禁止使用 `a`、`b`、`data`、`temp`、`flag`、`test`、`handle`、`doWork` 等模糊名称。
2. 类名使用名词或名词短语，例如 `CameraModule`、`MapModule`、`VoiceService`、`ConfigManager`。
3. 函数名使用动词或动宾结构，例如 `loadConfig()`、`startPreview()`、`requestMapImage()`、`handleVoiceCommand()`。
4. 布尔变量使用 `is`、`has`、`can`、`should` 开头，例如 `isCameraOpened`、`hasValidMapImage`、`canStartVoiceEngine`。
5. 配置变量必须带模块语义，例如 `mapApiKey`、`serialBaudRate`、`cameraDevicePath`。
6. 错误码和状态枚举必须命名清晰，例如 `MapError::InvalidApiKey`、`SystemState::Listening`。
7. 命名风格必须全项目统一，不允许同一项目中混用多种命名习惯。

## 五、禁止硬编码原则

1. 路径、设备号、端口号、波特率、API Key、模型路径、图片路径、服务器地址等不允许写死在源码中。
2. 所有可变参数必须放入配置文件，例如 `app.conf`、`map_settings.conf`、`camera.conf`、`serial.conf`。
3. 代码中只读取配置，不直接写死具体环境参数。
4. 不允许把密钥、账号、令牌等敏感信息写入源码或日志。
5. 针对 RK3588S、Ubuntu、HDMI、摄像头、串口等平台差异，应通过配置文件或平台适配层隔离。

## 六、错误处理原则

1. 所有外部操作都必须判断返回值，包括文件读取、网络请求、串口打开、摄像头打开、音频设备打开、模型加载、图片保存、线程创建。
2. 不允许默认任何外部资源一定存在或一定可用。
3. 失败时必须返回明确错误码或错误状态，不能只返回 `false` 而不说明原因。
4. 错误处理必须分级：可恢复错误给出警告，严重错误停止当前模块，致命错误终止系统启动。
5. 不允许静默失败，不允许失败后继续执行可能导致崩溃的逻辑。
6. 对网络返回内容必须校验状态码、Content-Type 和业务错误码，不能只判断 HTTP 200。
7. 对文件路径、JSON 字段、数组下标、指针对象、设备节点都必须做合法性检查。

## 七、日志规范原则

1. 所有关键流程必须有日志，包括启动、初始化、配置加载、模块启动、模块停止、异常情况、资源释放。
2. 日志必须能定位问题，不能只写 `error`、`failed`、`ok` 这类无意义信息。
3. 日志至少包含模块名、日志等级、动作说明、失败原因和关键参数。
4. 日志等级统一使用 `DEBUG`、`INFO`、`WARN`、`ERROR`、`FATAL`。
5. 正常流程使用 `INFO`，调试细节使用 `DEBUG`，可恢复异常使用 `WARN`，功能失败使用 `ERROR`，系统无法继续运行使用 `FATAL`。
6. 不允许用杂乱的 `printf` 代替统一日志系统。
7. 日志中不得输出完整密钥、隐私信息或敏感参数。

## 八、接口封装原则

1. 模块对外接口必须稳定、简单、语义清晰。
2. 外部调用者不应关心模块内部实现细节。
3. 地图模块内部可以处理 HTTP、JSON、图片缓存，但外部只调用 `updateMapImage()`、`showLocation()` 等接口。
4. 摄像头模块内部可以处理 OpenCV、V4L2、设备路径，但外部只调用 `startPreview()`、`captureFrame()` 等接口。
5. 语音模块内部可以处理唤醒、识别、播报，但外部只调用 `startListening()`、`speak()`、`getCommand()` 等接口。
6. 不允许在模块外部直接操作模块内部变量或资源句柄。
7. 必须通过接口、事件、回调或消息队列进行模块协作。

## 九、资源管理原则

1. 谁申请资源，谁负责释放。
2. 打开的文件、串口、摄像头、音频设备、网络连接、线程、定时器、模型资源必须在 `shutdown()` 或析构函数中释放。
3. 不允许存在没有退出条件的死循环。
4. 线程必须有明确启动、停止、退出和回收机制。
5. 图片缓存、日志文件、临时文件不能无限增长。
6. C++ 代码优先使用 RAII 思想管理资源。
7. 禁止出现资源泄漏、重复释放、悬空指针和野指针风险。

## 十、线程安全原则

1. 多线程必须有明确边界，不能随意共享全局变量。
2. UI 线程、摄像头线程、语音线程、串口线程、网络线程、推理线程之间必须通过安全机制通信。
3. 共享数据必须加锁，或使用线程安全队列、消息队列、事件机制。
4. UI 更新必须回到 UI 线程执行。
5. 不允许在线程中直接操作其他模块内部状态。
6. 不允许无限创建线程。
7. 每个线程必须支持安全退出。
8. 避免复杂锁嵌套，防止死锁。

## 十一、状态机原则

1. 对复杂业务流程必须使用明确状态管理。
2. 系统状态建议包括 `INIT`、`IDLE`、`LISTENING`、`RECOGNIZING`、`EXECUTING`、`SHOWING_CAMERA`、`SHOWING_MAP`、`ERROR`、`SHUTDOWN`。
3. 状态切换必须集中管理并输出日志。
4. 不允许用大量分散的 bool 变量表示系统状态。
5. 不允许让多个模块各自维护互相冲突的状态。
6. 状态机应清晰表达当前系统处于什么状态、允许执行什么动作、下一步如何切换。

## 十二、注释原则

1. 注释解释"为什么这样做"，不要重复代码表面含义。
2. 不写无意义注释，例如 `i++ // i 加 1`。
3. 对特殊兼容逻辑、硬件限制、平台差异、异常处理原因必须写注释。
4. 对迁移自开源项目的关键逻辑，应说明来源模块和保留原因。
5. 注释必须简洁，不允许大段废话。
6. 修改旧代码时，如果行为发生变化，必须同步更新注释。

## 十三、返回值和错误码原则

1. 简单函数可以返回 `bool`，但复杂模块必须返回明确错误码。
2. 错误码必须能表达失败原因，例如 `ConfigMissing`、`DeviceOpenFailed`、`NetworkFailed`、`InvalidResponse`、`PermissionDenied`。
3. 调用方必须处理错误码，不能忽略返回值。
4. 错误码、日志和用户提示之间应保持一致。
5. 不允许出现失败原因只能靠猜的情况。

## 十四、防止代码臃肿和嵌套过深的原则

1. 优先使用提前返回，减少多层 if 嵌套。
2. 一个函数只处理一个层级的逻辑，细节下沉到私有函数。
3. 重复逻辑必须抽取为公共函数或工具类。
4. 不允许为了一个小功能写过度复杂的继承体系。
5. 优先组合，谨慎继承。
6. 不允许出现过长参数列表，可使用 `Config`、`Options`、`Context` 等结构体封装。
7. 不允许一个类承担过多职责。
8. 不允许业务逻辑、硬件操作、UI 更新混在同一个函数里。
9. 能用清晰顺序流程解决的问题，不要引入复杂设计模式。
10. 代码首先保证可读性，其次才是抽象程度。

## 十五、迁移开源代码原则

1. 迁移前必须先分析原开源代码的模块职责、依赖关系、输入输出和运行流程。
2. 不要直接复制整个项目，应只迁移当前项目真正需要的部分。
3. 优先保留原项目中稳定、清晰、成熟的实现逻辑。
4. 对与当前项目风格不一致的代码，应进行适度封装，而不是粗暴混用。
5. 迁移代码必须去除无关功能、无关依赖和无关文件。
6. 迁移后必须统一命名、日志、错误处理、配置读取和资源释放方式。
7. 不允许让不同开源项目的风格在同一项目中并存，最终代码风格必须统一。
8. 对开源代码中的关键修改必须记录修改原因，避免后续无法追溯。

## 十六、文档和修改记录原则

每次修改代码后，必须同步整理以下内容：

1. 修改了哪些文件
2. 每个文件修改了什么
3. 为什么要这样修改
4. 是否影响其他模块
5. 是否新增配置项
6. 是否新增依赖
7. 是否改变启动方式
8. 是否需要重新编译
9. 是否完成基本测试
10. 是否存在遗留问题

建议每次修改后更新：

- `CHANGELOG.md`：记录本次修改
- `TODO.md`：记录后续待完成事项
- `DECISIONS.md`：记录关键设计决策
- `MODULE_STATUS.md`：更新模块状态
- `BUILD_RUN_TEST.md`：当编译/运行方式变化时更新
- `ARCHITECTURE.md`：当架构变化时更新

## 十七、输出要求

### 修改代码前必须说明：

1. 本次修改目标
2. 涉及模块
3. 计划修改文件
4. 是否需要新增文件
5. 是否会影响现有功能

### 修改代码后必须输出：

1. 修改文件列表
2. 核心修改说明
3. 新增或变更的接口
4. 新增或变更的配置项
5. 错误处理和日志处理说明
6. 资源释放和线程安全说明
7. 编译或运行验证方式
8. 本次修改是否存在风险
9. 后续建议

## 十八、禁止事项

1. 禁止把所有功能堆在 `main.cpp`
2. 禁止硬编码路径、密钥、设备号、端口号
3. 禁止跨模块直接乱调用
4. 禁止无日志的错误处理
5. 禁止忽略外部操作返回值
6. 禁止无限循环且无退出机制
7. 禁止多线程共享数据不加保护
8. 禁止一次性大面积重构无关文件
9. 禁止引入不必要的大型依赖
10. 禁止写只有自己能看懂的代码
11. 禁止为了减少文件数量而牺牲模块清晰度
12. 禁止为了抽象而抽象，导致代码臃肿
13. 禁止出现多个模块互相包含、互相控制的网状依赖
14. 禁止修改后不更新文档和修改记录

## 十九、最终目标

始终按照企业级嵌入式项目标准进行代码迁移和重构。最终代码应满足：

1. 模块职责清楚
2. 接口边界清楚
3. 命名清晰
4. 格式统一
5. 配置集中
6. 日志可定位
7. 错误可追踪
8. 资源可释放
9. 线程可退出
10. 状态可管理
11. 修改可记录
12. 后续可维护
13. 功能可扩展
14. 换板子或换设备时不需要推倒重来

**核心原则：不要写"能跑但难维护"的代码，要写"功能明确、结构清楚、长期可维护"的工程代码。**

---

## 修改记录

> 每次修改代码后，在此章节追加一条记录，格式如下：

### [日期] 修改标题

- **修改目标：**
- **涉及模块：**
- **修改文件：**
- **新增文件：**
- **是否影响现有功能：**
- **原则合规检查：** 逐条检查是否违反第几条原则，如有违反说明原因和后续补救计划。
- **遗留问题：**

---

### 2026-06-25 统一日志模块 + CaptureThread 工程化重构

- **修改目标：** 建立项目统一日志系统；将 CaptureThread 重构为企业级标准（生命周期、状态机、错误码、统一日志）。
- **涉及模块：** 日志（新增）、摄像头采集（重构）、Monitor 页面（适配）、MainWindow（日志替换）、构建系统（更新）。
- **修改文件：**
  - `Monitor/capture_thread.h` — 重写
  - `Monitor/capture_thread.cpp` — 重写
  - `rk3588s_migration/media/cameraconfig.cpp` — 加入 VLOG
  - `Monitor/monitor.h` — 适配新接口
  - `Monitor/monitor.cpp` — 适配新接口 + VLOG
  - `mainwindow.cpp` — 设备/摄像头配置加载改用 VLOG
  - `VehicleTerminal.pro` — 加入 LOG_DIR
  - `docs/CHANGELOG.md` — 记录修改
  - `docs/DECISIONS.md` — 新增 D017、D018
  - `docs/MODULE_STATUS.md` — 更新 Camera/Monitor/Log 状态
- **新增文件：**
  - `rk3588s_migration/common/logmanager.h`
  - `rk3588s_migration/common/logmanager.cpp`
- **是否影响现有功能：** 是 — CaptureThread 公开接口变更，Monitor 已适配。
- **原则合规检查：**
  - 原则二.4（模块接口）：✅ CaptureThread 现有 init/startCapture/stopCapture/shutdown
  - 原则四（命名）：✅ m_captureModule、m_stopRequested、m_stateMutex 等语义清晰
  - 原则五（硬编码）：✅ 设备路径从配置读取
  - 原则六（错误处理）：✅ ErrorCode 枚举 + lastErrorMessage
  - 原则七（日志）：✅ VLOG 替代 printf/perror/qWarning
  - 原则九（资源管理）：✅ shutdown() 释放 mmap/fd/config，析构自动调用
  - 原则十（线程安全）：✅ m_stateMutex + m_stopMutex，stopCapture 有超时
  - 原则十一（状态机）：✅ State 枚举 + transitionTo/transitionToError
  - 原则十六（文档）：✅ CHANGELOG/DECISIONS/MODULE_STATUS 已更新
  - 违反项：mainwindow.cpp 仍有 13 处 qWarning/qDebug 未改（原则七.6），因修改范围限制暂缓。
- **遗留问题：**
  - mainwindow.cpp 13 处 qWarning/qDebug 待后续替换为 VLOG
  - NV12→RGB32 CPU 转换性能待优化（RGA 硬件加速）
  - 板端运行验证待完成

---

### 2026-06-28 TTS→WAV→ASR 闭环测试链路合规审查与违规修复

- **修改目标：** 对照本规范对 2026-06-28 新增的 TTS→WAV→ASR 闭环测试链路代码进行合规审查，修复全部违规项与一处逻辑 Bug。
- **涉及模块：** TTS 引擎（TTSModel 接口扩展）、ASR 引擎（AsrEngine 接口扩展）、WAV I/O（RAII 重构）、CLI 测试工具（tts_wav_test/asr_wav_test）。
- **修改文件：**
  - `voice/tts/include/TTSModel.h` — 新增 `static constexpr kSampleRate/kChannels/kBitsPerSample` 静态常量，作为 TTS 输出规格的权威来源。
  - `voice/asr/AsrEngine.h` — 新增 `bool hasGetResultTextApi() const;` 声明。
  - `voice/asr/AsrEngine.cpp` — 实现 `hasGetResultTextApi()`，返回 `m_fnGetResultText != nullptr`。
  - `voice/audio/wav_io.cpp` — 用 `std::unique_ptr<FILE, decltype(&fclose)>` RAII 封装 FILE*，删除所有手写 fclose 重复调用。
  - `voice/tools/tts_wav_test.cpp` — 引用 `TTSModel::kSampleRate` 等常量替代魔法数字 16000/1/16。
  - `voice/tools/asr_wav_test.cpp` — 重写：拆分 main() 为 `parseArgs`/`loadAndReportWav`/`initAsrEngine`/`runRecognition`/`checkResult` 五个函数；用具名常量 `kRequiredSampleRate`/`kAsrChunkSamples` 等替代魔法数字；修复 `RESULT_API` 误用 `hasInputFinishedApi()` 的 Bug，改用 `hasGetResultTextApi()`。
- **新增文件：** 无（本次为违规修复，不新增文件）。
- **是否影响现有功能：** 否。TTSModel 新增静态常量不影响现有 `tts_service`（AudioPlayer.cpp 仍用裸 16000，后续可引用常量但本次不改以控制范围）；AsrEngine 新增查询方法不影响现有调用；wav_io RAII 重构行为等价；CLI 工具内部重构不影响外部接口。
- **原则合规检查：**
  - 原则五.1（禁止硬编码）：✅ 根源修复——TTSModel 暴露 kSampleRate 等权威常量，调用方引用而非重写 16000；asr_wav_test 用 kRequiredSampleRate 等具名常量。AudioPlayer.cpp:35 仍硬编码 16000（历史代码，本次不改以控制范围，已记录待后续统一）。
  - 原则三.4 / 十四.2（函数过长）：✅ asr_wav_test main() 从 220 行拆分为 5 个函数，main 仅编排 30 行。
  - 原则七.6（统一日志）：⚠️ CLI 工具仍用 cout/cerr（脚本需解析 KEY=VALUE 格式，VLOG 会污染解析）。判定：脚本解析行保留 cout 合理；AsrEngine 内部已用 VLOG。wav_io 作为无依赖纯 I/O 模块不带日志依赖合理，错误通过 errorMessage 上报。
  - 原则九.6（RAII 资源管理）：✅ wav_io.cpp 用 FileGuard RAII 封装 FILE*，消除手写 fclose 重复。
  - 原则六（错误处理）：✅ 修复 RESULT_API Bug——原本误用 `hasInputFinishedApi()` 判断 `RESULT_API`，现用 `hasGetResultTextApi()`，两个可选 API 独立查询。
  - 原则十二（注释）：✅ 新增常量带"为什么"注释（模型固有、sherpa-onnx streaming 要求）。
  - 原则十六（文档与修改记录）：✅ 本记录即为合规追加。
- **遗留问题：**
  - AudioPlayer.cpp:35 仍硬编码 16000，待后续统一引用 `TTSModel::kSampleRate`（本次不改以控制修改范围）。
  - CLI 工具用 cout/cerr 而非 VLOG，判定为合理（脚本解析需求），不视为违规。
  - 板端编译与运行验证待执行。

---

### 2026-07-18 内存池 + 线程池基础设施

- **修改目标：** 落地「工程优化与稳定性增强 - 内存池」第一阶段；同时提供通用线程池为后续短时 CPU 密集任务铺路。
- **涉及模块：** common（新增）、media/FileFrameSource、media/TestPatternSource、构建系统（CarTerminal.pro）。
- **修改文件：**
  - `common/frame_buffer_pool.h` — 新增
  - `common/frame_buffer_pool.cpp` — 新增
  - `common/thread_pool.h` — 新增
  - `common/thread_pool.cpp` — 新增
  - `media/file_frame_source.h` — 加 pool-enabled 构造函数与 m_pool 成员
  - `media/file_frame_source.cpp` — readFrame 优先从池借出
  - `media/test_pattern_source.h` — 同上
  - `media/test_pattern_source.cpp` — 同上
  - `CarTerminal.pro` — SOURCES/HEADERS 加入新文件
- **新增文件：**
  - `tests/common/test_frame_buffer_pool.cpp` — 7 场景单测
  - `tests/common/test_thread_pool.cpp` — 9 场景单测
  - `tests/common/test_file_frame_source_with_pool.cpp` — 3 场景集成测试
  - `tests/common/build_and_run_common_tests.sh` — 一键构建运行脚本
- **是否影响现有功能：** 否。FrameBufferPool 与 ThreadPool 是新增模块；
  FileFrameSource/TestPatternSource 通过新增构造函数接入池，原有构造函数
  与行为完全保持不变（向后兼容）。既有测试全部回归通过。
- **原则合规检查：**
  - 原则二.4（模块接口）：✅ FrameBufferPool 暴露 acquire/prewarm/shrink/
    statistics；ThreadPool 暴露 submit/waitIdle/stop/statistics。
  - 原则四（命名）：✅ FrameBufferPool、ThreadPool、Releaser、returnBlock、
    m_activeWorkers 等语义清晰。
  - 原则五（硬编码）：✅ worker 数、池容量、minFree/maxBlocks 均由构造参数
    决定，无魔法数字。
  - 原则六（错误处理）：✅ 池满走 overflow 路径不抛异常；线程池 stop 后
    submit 返回 broken future 由调用方 catch。
  - 原则七（日志）：✅ 新模块未引入 LogManager 依赖（保持单测可独立编译），
    通过 statistics() 接口暴露运行态供调用方自行记录日志。
  - 原则九.6（RAII 资源管理）：✅ shared_ptr Releaser 自动归还、unique_ptr
    管理空闲链表、ThreadPool 析构自动 join。
  - 原则十（线程安全）：✅ FrameBufferPool/ThreadPool 所有公有接口可从
    任意线程调用，内部 mutex + atomic 保护。
  - 原则十二（注释）：✅ 头文件顶部解释设计目标、容量策略、生命周期约束；
    waitIdle 谓词感知 m_stopping 的原因有详细注释。
  - 原则十四（避免臃肿）：✅ ThreadPool::submit 用模板 + lambda 类型擦除，
    避免为每种返回值写特化；FrameBufferPool 用 Releaser 避免侵入
    FramePacket。
  - 原则十六（文档）：✅ CHANGELOG/DECISIONS/MODULE_STATUS 同步更新，
    本记录即为合规追加。
- **遗留问题：**
  - 主程序 CarTerminal.pro 已包含 thread_pool.cpp，业务代码已由下方
    RKNN 后处理并行化条目接入。✅ 已完成。
  - 板端 Linux 编译运行验证 ✅ 已完成（2026-07-18，QuarkPi-CA2 全绿）。
  - CaptureThread（V4L2 主路径）暂未接内存池：其内部使用预分配
    m_rgbBuffer/m_nv12Buffer，已有 reuse 模式，接入池收益有限，后续视
    压测结果决定是否接入。

---

### 2026-07-18 RKNN 后处理 3 路 decode 并行化（接入 ThreadPool）

- **修改目标：** 把 RKNN YOLOv5 后处理中 3 个独立的 `decodeYoloLayer()`
  调用（stride 8/16/32）用 ThreadPool 并行执行，NMS 仍串行；`detect()`
  同步接口不变，调用方零改动。
- **涉及模块：** media/rknn_postprocess（新增并行路径）、media/rknn_detector
  （接入静态 ThreadPool）、新增单元测试。
- **修改文件：**
  - `media/rknn_postprocess.h` — 新增 `decodeOutputsParallel()` 声明 +
    `concurrency::ThreadPool` 前向声明（头文件不引入 thread_pool.h 依赖）
  - `media/rknn_postprocess.cpp` — 提取内部 helper `prepareDecodeLayers()`
    + `finalizeDetections()`，消除串行/并行两路径的校验与 NMS 代码重复；
    `decodeOutputs()` 改用 helper，行为完全不变；
    新增 `decodeOutputsParallel()`：3 层 decode 通过 `pool.submit()` 并行，
    `std::future` 等待全部完成；任一失败整体返回 false
    （`PreparedLayer::anchors` 改用 `std::array<int, 6>` 以便 C++17 lambda
    值捕获；`perLayerOk` 用 `std::vector<char>` 规避 `vector<bool>` proxy 坑点）
  - `media/rknn_detector.cpp` — 新增静态 `unique_ptr<ThreadPool> s_decodePool`
    （3 worker）；`initDetector()` 创建 pool，`clearDetectorState()` 先销毁
    pool 再释放缓冲；`detect()` 优先调用 `decodeOutputsParallel()`，pool 缺失
    时降级为串行 `decodeOutputs()`（防御性回退）
- **新增文件：**
  - `tests/rknn/test_rknn_postprocess_parallel.cpp` — 8 场景并行等价性测试
    （空输入、单候选、跨层候选、NMS 抑制、乱序输出、NaN 拒绝、重复 stride
    拒绝、200 次压测验证并行与串行结果完全一致）
  - `tests/rknn/build_and_run_rknn_tests.sh` — 板端一键构建运行脚本
    （同时跑既有 9 场景 + 新增 8 场景）
  - `tests/rknn/_win_project_root.h` — Windows g++ 测试用的宏注入文件
    （绕过 PowerShell 中文路径转义问题）
- **是否影响现有功能：** 否。`detect()` 同步接口不变；既有 9 场景 96 断言
  测试零改动全绿作为回归基线；Monitor.cpp 两处调用点零改动。
- **原则合规检查：**
  - 原则二.4（模块接口）：✅ `decodeOutputsParallel()` 接收 `ThreadPool&`，
    仅新增一个公有接口，原 `decodeOutputs()` 保留为回归基线 + 降级路径。
  - 原则四（命名）：✅ `decodeOutputsParallel`、`prepareDecodeLayers`、
    `finalizeDetections`、`PreparedLayer` 等语义清晰。
  - 原则五（硬编码）：✅ 3 worker 数与 3 个 stride 层对齐，写在 detector
    内部不暴露魔法数字。
  - 原则六（错误处理）：✅ 任一并行任务失败整体返回 false，取第一个失败
    层的 error；`future::get()` 异常转译为通用错误，公共契约不泄漏异常。
  - 原则九.6（RAII 资源管理）：✅ `unique_ptr<ThreadPool>` 保证析构自动 join
    workers；pool 销毁先于输入缓冲释放（`clearDetectorState` 顺序明确）。
  - 原则十（线程安全）：✅ 每层 task 只写自己的 slot（`perLayer[i]`/
    `perLayerError[i]`/`perLayerOk[i]`），无共享写入；
    `OutputTensorView.data` 只读；主线程 `future::get()` 等待全部完成。
  - 原则十二（注释）：✅ 关键判断（为何不并行 NMS）写有注释；
    `vector<char>` 替代 `vector<bool>` 的理由有注释。
  - 原则十四（避免臃肿）：✅ 提取 `prepareDecodeLayers()` helper 消除串行/
    并行两路径 200+ 行校验代码重复；lambda 类型擦除避免为每种返回值写特化。
  - 原则十六（文档）：✅ CHANGELOG/DECISIONS D027/MODULE_STATUS 同步更新，
    本记录即为合规追加。
- **遗留问题：**
  - 板端 RK3588S 多核（4×A76 + 4×A55）实际并行收益待 NPU 推理路径整体
    联调后压测验证（当前仅验证正确性，未压测帧率提升）。

---

### 2026-07-18 FrameBroker 改用固定容量环形缓冲区（稳态零堆分配）

- **修改目标：** 把 `FrameBroker::LaneState` 内部的 `std::queue<FramePacket>`
  替换为固定容量环形缓冲区（`std::vector<FramePacket>` + head/tail/count），
  稳态运行零堆分配；保留 mutex（锁非瓶颈）。
- **涉及模块：** media/frame_broker（内部实现重构）、新增专项单元测试。
- **修改文件：**
  - `media/frame_broker.h` — `LaneState` 改用 `std::vector<FramePacket> slots`
    + head/tail/count；类注释补充设计决策（为何保留 mutex）
  - `media/frame_broker.cpp` — 构造函数预分配每个 lane 的 slots 到容量大小；
    `publish()` drop_oldest 改为 `slots[head] = FramePacket{}` + head 推进，
    新帧拷贝赋值到 `slots[tail]`（const 引用无法 move，但 shared_ptr 拷贝廉价）；
    `take()` 用 `std::move` 把 slot 内容移出，立即 `FramePacket{}` 重置槽位；
    `setLaneEnabled(false)` / `reset()` 遍历有效槽位 `FramePacket{}` 重置，
    释放所有 shared_ptr 引用
- **新增文件：**
  - `tests/media/test_frame_broker_ring.cpp` — 8 场景专项测试
    （capacity-1 边界、多周期 wrap-around、take move 语义、丢弃槽位 buffer 释放、
    disable/reset buffer 释放、1-producer/3-consumer 500 帧压测、enable/disable
    循环压测）
  - `tests/media/build_and_run_frame_broker_tests.sh` — 板端一键脚本
    （同时跑既有 8 场景 + 新增 8 场景）
- **是否影响现有功能：** 否。公共接口（`publish/take/setLaneEnabled/
  requestStop/reset/...`）签名零改动；既有 8 场景测试零改动全绿；
  MediaPipeline/Monitor 等调用方零改动。
- **原则合规检查：**
  - 原则二.4（模块接口）：✅ 公共接口全部保留，纯内部实现重构。
  - 原则五（硬编码）：✅ 容量由构造参数决定，无魔法数字。
  - 原则六（错误处理）：✅ 所有槽位释放路径都显式 `FramePacket{}` 重置，
    保证 shared_ptr 引用不泄漏。
  - 原则九.6（RAII 资源管理）：✅ `take()` 用 `std::move` 移出 slot 后立即
    重置槽位，shared_ptr 引用及时释放（buffer 尽早回收）。
  - 原则十（线程安全）：✅ 保留 `std::mutex` + `std::condition_variable`，
    线程安全契约与原实现一致（mutex 竞争 <1μs，远小于 33ms 帧间隔，非瓶颈）。
  - 原则十二（注释）：✅ 头文件注释解释"为何保留 mutex 不追求 lock-free"
    的工程理由（lock-free MPMC ring 复杂度不划算 + shared_ptr 原子引用计数
    本身就不是 lock-free）。
  - 原则十四（避免臃肿）：✅ 固定容量 vector + 槽位复用，避免 `std::queue`
    每次 push/pop 的堆操作；代码逻辑清晰可读。
  - 原则十六（文档）：✅ CHANGELOG/DECISIONS D028/MODULE_STATUS 同步更新，
    本记录即为合规追加。
- **遗留问题：**
  - 无。Windows + 板端双平台 16 场景全绿，行为完全不变。
