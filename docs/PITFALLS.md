# Development Environment Pitfalls (陷阱表)

> 本文件记录 RK3588S_CarTerminal 项目在 QuarkPi-CA2 板端开发过程中踩过的
> 环境配置陷阱。每条陷阱有唯一编号（P###），与 `scripts/check_dev_env.sh`
> 中的检测函数一一对应。
>
> **使用方式**：
> - 新机器 / 新克隆 / 重置环境后，先运行 `./scripts/check_dev_env.sh` 自查
> - 失败时按本文件的"解决方案"修复，再用"验证命令"复核
> - `scripts/verify_no_peripheral.sh` 是功能验收（开发后），与本文件的前置
>   检查互补

## 分类索引

| 类别 | 陷阱编号 | 主题 |
|---|---|---|
| 构建系统 | P001 | qmake vs CMake 混用 |
| 构建系统 | P002 | 系统时间未同步（未来 mtime） |
| ALSA 配置 | P003 | ALSA 设备选错（plughw:2,0 vs 3,0） |
| ALSA 配置 | P004 | USB 麦克风 Capture Switch 未启用 |
| ALSA 配置 | P005 | Mic 音量 100% 导致 clipping |
| ALSA 配置 | P006 | VoiceManager 用 snd_pcm_start 而非 priming readi |
| ASR 时序 | P007 | 应用启动后 <6s 就说话 |
| ASR 时序 | P008 | 命令后 <3s 静音 |
| ASR 时序 | P009 | 尾静音 <2.4s |
| ASR 配置 | P010 | sherpa 版本/SHA 不匹配 |
| ASR 配置 | P011 | 解码方法 ≠ modified_beam_search |
| ASR 配置 | P012 | 热词 score ≠ 2.5 |
| ASR 配置 | P013 | asr_hotwords.txt 缺失 |
| ASR 配置 | P014 | 测试时 TTS 反馈未关闭 |
| ZMQ | P015 | ZMQ 用 localhost 而非 127.0.0.1 |
| ZMQ | P016 | /etc/hosts 缺少 localhost 条目 |
| ZMQ | P017 | TTS mute window 配置错误 |
| NPU | P018 | llm_enabled=true 但 NPU 驱动 <v0.9.7 |
| 工程纪律 | P019 | ASR_DBG 调试代码残留 |

---

## P001 — qmake vs CMake 混用

**症状**：执行 `cmake .. && make` 后找不到 `CarTerminal` 二进制；或 qmake
生成的 Makefile 在 `./build/` 找不到。

**根因**：项目主程序使用 **qmake**（`CarTerminal.pro`），二进制生成在项目根
目录；voice/ 子模块使用 CMake。两套构建系统不能交叉使用。

**解决方案**：
```bash
# 主程序（CarTerminal） — 必须 qmake
cd ~/RK3588S_CarTerminal
qmake CarTerminal.pro
make -j8
# 二进制位置：./CarTerminal（项目根目录）

# voice/ 子模块（llm_service/tts_service/tests） — 必须 CMake
mkdir -p ~/build_voice
cmake -S ~/RK3588S_CarTerminal/voice -B ~/build_voice
cmake --build ~/build_voice -j8
```

**验证命令**：
```bash
ls -la ~/RK3588S_CarTerminal/CarTerminal   # 主程序
ls -la ~/build_voice/llm_service ~/build_voice/tts_service
```

**对应检测**：`check_dev_env.sh` 函数 `check_qmake_build_system`

---

## P002 — 系统时间未同步（未来 mtime）

**症状**：`make -j8` 无限循环，反复重新编译已编译的文件；或 `make` 报
"File has modification time in the future"。

**根因**：RK3588S 板子无 RTC 电池，断电后系统时间回到 1970；若开发机时间
同步到板子时把"未来时间"传到文件 mtime，`make` 会认为源文件比 .o 新，
反复重编。

**解决方案**：
```bash
# 方案 A：NTP 同步
sudo apt-get install -y ntpdate
sudo ntpdate ntp.aliyun.com

# 方案 B：手动设置（无网络时）
sudo date -s "2026-07-18 14:30:00"

# 修复已污染的 mtime
find ~/RK3588S_CarTerminal -type f \( -name "*.cpp" -o -name "*.h" \
  -o -name "*.pro" -o -name "*.ui" -o -name "*.qrc" \) -exec touch {} \;
```

**验证命令**：
```bash
date                                    # 当前时间是否合理
find ~/RK3588S_CarTerminal -maxdepth 3 -type f \
  -exec stat -c "%Y %n" {} \; | sort -rn | head -3
# 最新的 mtime 不应超过当前时间 +2 秒
```

**对应检测**：`check_dev_env.sh` 函数 `check_system_time_sync`

---

## P003 — ALSA 设备选错（plughw:2,0 vs 3,0）

**症状**：ASR 收到全静音，`ALSA capture stats` 日志中 `maxAbs=0`；或
`arecord` 报 "I/O error"。

**根因**：QuarkPi-CA2 板载 ES8323 codec 的录音路径 I2C 通讯故障
（`arecord -l` 显示 card 2），实际可用的是 USB 摄像头麦克风
（card 3，立体声）。配置错把 `plughw:2,0` 当作麦克风会收到空数据。

**解决方案**：
```bash
# 1. 确认 USB 麦克风是 card 3
arecord -l | grep -E "^card"

# 2. 修改 config/asr_settings_offline.conf
sed -i 's|^asr_alsa_device=.*|asr_alsa_device=plughw:3,0|' \
  ~/RK3588S_CarTerminal/config/asr_settings_offline.conf

# 3. 验证能录到声音（录 2 秒，回放听）
arecord -D plughw:3,0 -d 2 -f dat /tmp/test.wav
aplay /tmp/test.wav
```

**验证命令**：
```bash
grep "^asr_alsa_device=" ~/RK3588S_CarTerminal/config/asr_settings_offline.conf
# 期望输出：asr_alsa_device=plughw:3,0
```

**对应检测**：`check_dev_env.sh` 函数 `check_alsa_device`

---

## P004 — USB 麦克风 Capture Switch 未启用

**症状**：ALSA stream 状态是 RUNNING，但 `maxAbs=0`，ASR 完全识别不到。

**根因**：USB 麦克风的 `Mic Capture Switch` 默认是 mute 状态，需要用
`amixer` 的 `cap` 关键字显式启用（不是 `unmute`）。

**解决方案**：
```bash
# 启用 capture（注意：必须用 cap，不是 unmute）
amixer -c 3 sset 'Mic' 35% cap

# 验证
amixer -c 3 contents
```

**验证命令**：
```bash
amixer -c 3 contents | grep -A2 "Mic Capture"
# 期望看到：values=on
```

**对应检测**：`check_dev_env.sh` 函数 `check_mic_hardware`（同时覆盖 P004+P005）

---

## P005 — Mic 音量 100% 导致 clipping

**症状**：ASR 识别率极差，把"打开地图"识别成"打开筒筒"、"大"等乱码。

**根因**：USB 麦克风音量 100% 时波形削顶（clipping），sherpa-onnx 收到
失真音频。安全范围是 30-50%。

**解决方案**：
```bash
# 35% 是经验值（30-50% 都可）
amixer -c 3 sset 'Mic' 35% cap

# 如果之前没启用 cap，这一条同时启用
```

**验证命令**：
```bash
amixer -c 3 sget 'Mic' | grep -E "Volume|values"
# 期望：Front Left/Right Volume 都是 35% 左右
```

**对应检测**：`check_dev_env.sh` 函数 `check_mic_hardware`（同时覆盖 P004+P005）

---

## P006 — VoiceManager 用 snd_pcm_start 而非 priming readi

**症状**：ALSA `poll()` 永不返回 POLLIN，`ALSA capture stats` 日志中
`readCount=0`；`/proc/asound/card3/pcm0c/sub0/status` 显示 state=PREPARED
而非 RUNNING。

**根因**：plughw 设备是 lazy-start，`snd_pcm_start()` 返回 0 但底层 hw
stream 仍停留在 PREPARED；必须用阻塞式的 `snd_pcm_readi()` 拉一次数据
才能让 stream 真正进入 RUNNING 状态。

**解决方案**：保持 `voice/VoiceManager.cpp` 中的 priming readi 模式：

```cpp
// 正确：priming readi
snd_pcm_sframes_t primed = snd_pcm_readi(m_alsaHandle, buf, frames);
if (primed > 0) {
    VLOG_INFO(MODULE, "ALSA stream primed (first readi=%ld)", primed);
}

// 错误：不要改回 snd_pcm_start
// snd_pcm_start(m_alsaHandle);  // 不会真正启动 plughw
```

**验证命令**：
```bash
grep -E "ALSA stream primed|snd_pcm_start" ~/RK3588S_CarTerminal/voice/VoiceManager.cpp
# 期望：能找到 "ALSA stream primed"，且 m_alsaHandle 上下文无 snd_pcm_start
```

**对应检测**：`check_dev_env.sh` 函数 `check_priming_readi`

---

## P007 — 应用启动后 <6s 就说话

**症状**：应用启动后立即说"打开地图"，日志中无 ASR 识别结果。

**根因**：CarTerminal 启动需要初始化 Qt GUI、VoiceManager、ASR engine、
ALSA stream priming，整套流程约需 6 秒。在此之前 ASR 线程尚未 ready，
说的话会被丢弃。

**解决方案**：启动后**等待至少 6 秒**再开始说话。日志出现
`application ready` 后再开始。

**验证命令**：
```bash
# 启动应用并观察日志
./CarTerminal 2>&1 | tee /tmp/app.log
# 另一个终端
sleep 6 && grep -E "application ready|ASR thread started" /tmp/app.log
```

**对应检测**：`check_dev_env.sh` 函数 `check_asr_timing_docs`（文档提示）

---

## P008 — 命令后 <3s 静音

**症状**：说了"打开地图"但 ASR 不返回结果，等到说下一句时才触发上一句。

**根因**：sherpa-onnx 的端点检测需要确认语音结束。说完一句指令后必须
保持至少 3 秒静音，端点检测才会触发并返回识别结果。

**解决方案**：每句指令后默数 3 秒再说下一句。

**验证命令**：观察日志中 `ASR final result:` 出现的时机，应在说完后 3s 左右。

**对应检测**：`check_dev_env.sh` 函数 `check_asr_timing_docs`（文档提示）

---

## P009 — 尾静音 <2.4s

**症状**：识别结果不完整，"打开地图"只识别为"打开"。

**根因**：sherpa-onnx 默认尾静音端点阈值是 2.4 秒。如果尾静音不够长，
端点检测不会触发，已识别的部分不会作为最终结果返回。

**解决方案**：保持 `config/asr_settings_offline.conf` 中的
`asr_endpoint_silence`（如果有）≥ 2.4，或测试时确保说完后静音 ≥3 秒。

**验证命令**：
```bash
grep -E "endpoint|silence" ~/RK3588S_CarTerminal/config/asr_settings_offline.conf
```

**对应检测**：`check_dev_env.sh` 函数 `check_asr_timing_docs`（文档提示）

---

## P010 — sherpa 版本/SHA 不匹配

**症状**：`AsrEngine::getResult()` 返回乱码或 SIGSEGV；日志中
`RESULT_ABI_COMPATIBLE=0`。

**根因**：`voice/asr/sherpa_onnx_abi_1_13_3.h` 中的
`OnlineRecognizerResult` 结构体布局仅对 sherpa-onnx 1.13.3
（Git SHA `330609da`）有效。其他版本的结构体布局可能不同，按 1.13.3
读取会导致 UB。

**解决方案**：
```bash
# 检查板端 sherpa 版本
strings /usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so | grep -E "1\.[0-9]+\.[0-9]+"

# 必须是：1.13.3
# Git SHA 必须是：330609da
```

**验证命令**：
```bash
sha256sum /usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so
# 期望：c713c6c6ca7fcaff01607768d3a163f73aae0c83e476bc1bcbce7839b9695507
```

**对应检测**：`check_dev_env.sh` 函数 `check_sherpa_version`

---

## P011 — 解码方法 ≠ modified_beam_search

**症状**：识别率下降，热词加权失效。

**根因**：sherpa-onnx 的 `greedy_search` 不支持上下文偏置
（contextual biasing），热词加权需要 `modified_beam_search` 才能生效。

**解决方案**：
```bash
sed -i 's|^asr_decoding_method=.*|asr_decoding_method=modified_beam_search|' \
  ~/RK3588S_CarTerminal/config/asr_settings_offline.conf
```

**验证命令**：
```bash
grep "^asr_decoding_method=" ~/RK3588S_CarTerminal/config/asr_settings_offline.conf
# 期望：asr_decoding_method=modified_beam_search
```

**对应检测**：`check_dev_env.sh` 函数 `check_decoding_method`

---

## P012 — 热词 score ≠ 2.5

**症状**：识别率下降，TTS 回声误触发 ASR。

**根因**：score=1.5 是 sherpa 默认值，对本项目的车载指令集偏弱；
score=2.5 是经验值，能抑制 TTS 回声和环境噪声的误触发，同时保证
车载指令识别率 ≥6/6。

**解决方案**：
```bash
sed -i 's|^asr_hotwords_score=.*|asr_hotwords_score=2.5|' \
  ~/RK3588S_CarTerminal/config/asr_settings_offline.conf
```

**验证命令**：
```bash
grep "^asr_hotwords_score=" ~/RK3588S_CarTerminal/config/asr_settings_offline.conf
# 期望：asr_hotwords_score=2.5
```

**对应检测**：`check_dev_env.sh` 函数 `check_hotwords_score`

---

## P013 — asr_hotwords.txt 缺失

**症状**：AsrEngine 初始化失败，日志报 "hotwords file not found"。

**根因**：AsrEngine 初始化要求 `config/asr_hotwords.txt` 存在且非空
（UTF-8 无 BOM，每行一个热词，空格分词）。

**解决方案**：
```bash
# 确认文件存在且非空
ls -la ~/RK3588S_CarTerminal/config/asr_hotwords.txt

# 如果缺失，从 example 复制
cp ~/RK3588S_CarTerminal/config/asr_hotwords.example.txt \
   ~/RK3588S_CarTerminal/config/asr_hotwords.txt
```

**验证命令**：
```bash
file ~/RK3588S_CarTerminal/config/asr_hotwords.txt
# 期望：ASCII/UTF-8 text，无 BOM
head -3 ~/RK3588S_CarTerminal/config/asr_hotwords.txt
```

**对应检测**：`check_dev_env.sh` 函数 `check_hotwords_file`

---

## P014 — 测试时 TTS 反馈未关闭

**症状**：测试 ASR 识别率时，TTS 反馈的语音被麦克风录到，形成回环，
识别结果全是 TTS 的播报内容。

**根因**：`tts_feedback_for_command=true` 时，每命中一条指令会播报
"已为您打开地图"等反馈，麦克风可能录到这些声音。虽然 VoiceManager
有 `tts_mute_ms` 静音窗口，但调试 ASR 识别率时应彻底关闭 TTS 反馈。

**解决方案**（仅测试 ASR 识别率时）：
```bash
sed -i 's|^tts_feedback_for_command=.*|tts_feedback_for_command=false|' \
  ~/RK3588S_CarTerminal/config/voice_settings.conf
# 测完后改回 true
```

**验证命令**：
```bash
grep "^tts_feedback_for_command=" ~/RK3588S_CarTerminal/config/voice_settings.conf
```

**对应检测**：`check_dev_env.sh` 函数 `check_tts_feedback_for_testing`

---

## P015 — ZMQ 用 localhost 而非 127.0.0.1

**症状**：ZMQ `recv()` 阻塞 30 秒后超时，日志报
`ZmqCommunicationError`。

**根因**：板端 `/etc/hosts` 可能缺少 `localhost` 条目，导致
`localhost` 无法解析。`127.0.0.1` 是 IP 字面量，不依赖
`/etc/hosts`。

**解决方案**：
```bash
sed -i 's|tcp://localhost:|tcp://127.0.0.1:|g' \
  ~/RK3588S_CarTerminal/config/voice_settings.conf
```

**验证命令**：
```bash
grep -E "^(llm_address|tts_address|tts_status_address)=" \
  ~/RK3588S_CarTerminal/config/voice_settings.conf
# 期望：三个地址都是 tcp://127.0.0.1:xxxx
```

**对应检测**：`check_dev_env.sh` 函数 `check_zmq_address`

---

## P016 — /etc/hosts 缺少 localhost 条目

**症状**：同 P015，但即使配置改回 `localhost` 也会失败。

**根因**：Debian 最小安装的 `/etc/hosts` 可能没有
`127.0.0.1 localhost`。重烧固件后此修改会丢失。

**解决方案**：
```bash
echo "127.0.0.1 localhost" | sudo tee -a /etc/hosts
```

**验证命令**：
```bash
grep "localhost" /etc/hosts
# 期望：127.0.0.1 localhost
```

**对应检测**：`check_dev_env.sh` 函数 `check_etc_hosts`

---

## P017 — TTS mute window 配置错误

**症状**：TTS 播报期间 ASR 录到 TTS 声音形成回环；或 TTS 播完后 ASR
被静音 30 秒无法识别下一句。

**根因**：
- `tts_feedback_mute_ms=0` → 无静音，回环
- `tts_feedback_mute_ms=30000` → TTS 进程崩溃时静音窗无法解除
- 正确值是 `3000`（3 秒，覆盖 6 字短句的合成+播放）

**解决方案**：
```bash
sed -i 's|^tts_feedback_mute_ms=.*|tts_feedback_mute_ms=3000|' \
  ~/RK3588S_CarTerminal/config/voice_settings.conf
```

**验证命令**：
```bash
grep "^tts_feedback_mute_ms=" ~/RK3588S_CarTerminal/config/voice_settings.conf
# 期望：tts_feedback_mute_ms=3000
```

**对应检测**：`check_dev_env.sh` 函数 `check_tts_mute_window`

---

## P018 — llm_enabled=true 但 NPU 驱动 <v0.9.7

**症状**：应用启动后 SIGBUS 崩溃，`dmesg` 报
`rknpu_job_subcore_commit_pc` NULL pointer dereference。

**根因**：rkllm-runtime v1.3.0 要求 RKNPU 内核驱动 ≥v0.9.7；
QuarkPi-CA2 板载驱动是 v0.9.6，`rkllm_init` 成功但 `rkllm_run`
触发 SIGBUS。

**解决方案**：
```bash
# 1. 立即禁用 LLM
sed -i 's|^llm_enabled=.*|llm_enabled=false|' \
  ~/RK3588S_CarTerminal/config/voice_settings.conf

# 2. 确认 NPU 驱动版本
sudo cat /sys/kernel/debug/rknpu/version
# 如果是 v0.9.6 或更低，保持 llm_enabled=false

# 3. 升级驱动（需要重烧内核或固件，见 docs/TODO.md "Blocked" 章节）
```

**验证命令**：
```bash
grep "^llm_enabled=" ~/RK3588S_CarTerminal/config/voice_settings.conf
# 期望：llm_enabled=false
sudo cat /sys/kernel/debug/rknpu/version
```

**对应检测**：`check_dev_env.sh` 函数 `check_llm_disabled`

---

## P019 — ASR_DBG 调试代码残留

**症状**：日志中出现大量 `ASR_DBG dbgPollCnt=xxx dbgReadCnt=xxx
dbgMaxAbs=xxx` 调试输出，干扰正常日志阅读。

**根因**：调试 ALSA 问题时临时插入的 `ASR_DBG` 宏未清理。

**解决方案**：清理 `voice/VoiceManager.cpp` 中所有 `ASR_DBG`、
`dbgPollCnt`、`dbgReadCnt`、`dbgMaxAbs` 相关代码。

**验证命令**：
```bash
grep -c "ASR_DBG" ~/RK3588S_CarTerminal/voice/VoiceManager.cpp
# 期望：0
```

**对应检测**：`check_dev_env.sh` 函数 `check_asr_dbg_clean`

---

## 与 `verify_no_peripheral.sh` 的关系

| 文件 | 用途 | 运行时机 | 失败处理 |
|---|---|---|---|
| `scripts/check_dev_env.sh` | 环境前置检查 | 开发前 / 新克隆后 | 修复后重跑 |
| `scripts/verify_no_peripheral.sh` | 功能验收 | 开发后 / 提交前 | 修复代码后重跑 |

两个脚本共享部分检测逻辑（如红线 1-5），但侧重点不同：
- `check_dev_env.sh` 覆盖**环境层**（系统时间、ALSA 设备、sherpa 版本、
  /etc/hosts、NPU 驱动、Mic 物理开关等）
- `verify_no_peripheral.sh` 覆盖**代码层**（构建、CTest、闭环测试）

---

## 维护规则

1. **新增陷阱**：踩到新坑时，按 P### 编号递增添加到本文件，并在
   `scripts/check_dev_env.sh` 添加对应检测函数。
2. **修改陷阱**：解决方案变化时同步更新本文件和检测脚本。
3. **删除陷阱**：陷阱彻底消除（如升级驱动后）时，本文件保留历史
   记录（标记为 Resolved），但检测脚本可以删除对应函数。
4. **与 project_memory.md 的关系**：`project_memory.md` 是 AI 内部
   记忆，本文件是开发者可见文档；两者保持同步，但以本文件为权威。
