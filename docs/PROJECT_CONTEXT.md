# Project Context

> 2026-06-28 最新综合进度和板端语音验证证据见 [`../../docs/PROJECT_PROGRESS_2026-06-28.md`](../../docs/PROJECT_PROGRESS_2026-06-28.md)。

## Project Goal

This repository is migrating two legacy projects into an RK3588S-based AI vehicle multimedia cockpit terminal for the 正点原子 QuarkPi-CA2 board.

Target product:

- Platform: RK3588S / QuarkPi-CA2.
- Display first: HDMI.
- GUI: Qt 5 Widgets, based on the existing `IMX6ULL车载终端/VehicleTerminal`.
- AI and voice: migrate from `基于deepseek与rk3576的模块化离线智能语音交互系统`.
- Hardware/device access: move out of GUI and into configurable device/service modules.
- Long-term direction: AI voice interaction, multimedia, camera/V4L2, RTSP, sensors, vehicle status, local AI inference, and stable engineering build/test flow.

## Source Projects

### Project A: IMX6ULL Vehicle Terminal

Path:

```text
IMX6ULL车载终端/
```

Main useful parts:

- Qt GUI application: `IMX6ULL车载终端/VehicleTerminal/`
- Existing pages: map, monitor, music, weather, clock, settings.
- Legacy drivers and examples: GPIO key, DHT11, AP3216, GPS UART, camera/V4L2.

Main risks:

- IMX6ULL device paths are not valid on RK3588S.
- Legacy code has global state in headers.
- Some hardware control is coupled to GUI classes.
- Some paths, city names, device nodes, and API keys are hardcoded.

### Project B: DeepSeek/RK3576 Modular Offline Voice System

Path:

```text
基于deepseek与rk3576的模块化离线智能语音交互系统/
```

Main useful parts:

- Voice/LLM module structure.
- ZMQ communication kit.
- Offline ASR/TTS/LLM integration ideas.
- Sherpa-ONNX and model-related scripts.

Main risks:

- Not all code is directly portable to RK3588S.
- Model paths and runtime dependencies must be configurable.
- Voice pipeline must not call GUI directly.

## Current Migration Workspace

New RK3588S migration code lives under:

```text
rk3588s_migration/
```

Current submodules:

- `common/`: shared command parser.
- `device/`: configurable hardware/device support.
- `voice_gateway/`: **RETIRED (2026-07-18, task A2)** — historical voice command gateway skeleton, absent from the current workspace. The active voice stack lives under `RK3588S_CarTerminal/voice/` (ASR/LLM/TTS/ZMQ).

The existing Qt app remains in:

```text
IMX6ULL车载终端/VehicleTerminal/
```

Qt is being migrated incrementally by linking to `rk3588s_migration` modules.

## 当前阻塞状态（2026-06-27更新）

### ~~MPP硬件编码器 — pktSize=0~~ ✅ 已解决
- **状态**：✅ 已解决（2026-06-27）
- **根因**：`MPP_FMT_YUV420P`枚举值错误硬编码为1（实际为4）
- **验证**：test_mpp_direct 10/10帧成功，主程序RTSP推流VLC播放正常

### NPU驱动版本 — Bus error
- **状态**：❌ 未解决（需升级NPU驱动到v0.9.7+）
- **现象**：rkllm_run触发SIGBUS
- **影响**：Voice LLM Service不可用

## User Engineering Rules

All future code changes must follow these rules:

1. Do not write single-file grab-bag code.
2. Do not put all logic in `main`.
3. Do not put hardware control in GUI.
4. Do not hardcode model paths, device paths, or GPIO numbers.
5. Do not call across layers directly.
6. Do not pass state through global variables.
7. Do not replace event mechanisms with `sleep` busy waits.
8. Do not write naked `while(1)` loops.
9. Do not omit error handling.
10. Do not omit logs.
11. Do not omit resource release.
12. Do not omit build scripts.
13. Do not omit test methods.
14. Do not casually introduce third-party libraries.
15. Do not break existing interfaces.
16. Do not do unrelated refactors.
17. Do not modify multiple modules at once.
18. Do not generate uncompilable pseudocode.
19. Follow enterprise-level code style: high cohesion and low coupling.

## Mandatory Read Before Every Task

Before any future task, code reading, code change, or documentation change, read these files first:

```text
docs/PROJECT_CONTEXT.md
docs/ARCHITECTURE.md
docs/MIGRATION_PLAN.md
docs/MODULE_STATUS.md
docs/INTERFACE_SPEC.md
docs/DECISIONS.md
docs/TODO.md
docs/CHANGELOG.md
docs/BUILD_RUN_TEST.md
```

Then read task-specific files listed in `docs/MIGRATION_PLAN.md`.

This rule also applies before inspecting source code. Read the project context docs first, then read task-related source files.

## Mandatory Record After Code Changes

After every code change, update these files:

```text
docs/CHANGELOG.md
docs/MODULE_STATUS.md
```

`docs/CHANGELOG.md` must record:

1. Date.
2. Task.
3. Added files.
4. Modified files.
5. Deleted files.
6. Reason for change.
7. Build result.
8. Test result.
9. Remaining issues.
10. Next step.

`docs/MODULE_STATUS.md` must update the status of every module touched by the change.

`docs/TODO.md` must be appended if new follow-up issues are found.

Update these files when applicable:

- `docs/INTERFACE_SPEC.md` if interfaces changed.
- `docs/DECISIONS.md` if a new technical direction or major implementation choice is made.
- `docs/BUILD_RUN_TEST.md` if new build, run, or test commands are added.
