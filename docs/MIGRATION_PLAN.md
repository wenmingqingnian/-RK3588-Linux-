# Migration Plan

## Migration Direction

The migration is incremental. Do not rewrite the whole application at once.

Each step must move one module from legacy coupling toward:

```text
configuration -> service/device adapter -> stable interface -> GUI usage
```

## Source Mapping

| Target module | Main source project | Source path | Target direction |
|---|---|---|---|
| Qt cockpit GUI | Project A | `IMX6ULL车载终端/VehicleTerminal/` | Keep UI, remove hardware/AI coupling |
| Key input | Project A | `驱动/gpio_key`, old `/dev/key` usage | `rk3588s_migration/device` |
| GPS/map | Project A | `VehicleTerminal/Map/` | Configurable GPS device and static map network config |
| DHT11/sensors | Project A | `dht11.*`, `驱动/dht11_driver` | Device config + driver adapter |
| AP3216 | Project A | `Monitor/ap3216.*`, I2C helpers | Device config + driver adapter |
| Camera/V4L2 | Project A | `Monitor/capture_thread.*` | Media module, V4L2 adapter |
| Music/weather GUI | Project A | `Music/`, `Weather/` | Keep GUI; isolate weather formatting/response safety first, then network/config |
| Voice gateway | Project B | ZMQ and voice modules | `rk3588s_migration/voice_gateway` *(RETIRED 2026-07-18, A2 — superseded by `RK3588S_CarTerminal/voice/`)* |
| ASR/TTS/LLM | Project B | `voice/`, model scripts | Future `voice` and `ai` services |
| RTSP | Project B plus media design | media/transport pieces | Future `media/rtsp` module |

## Migration Phases

### Phase 1: Stable Skeleton

Status: in progress.

Goals:

- Keep existing Qt app compiling.
- Add shared command parser.
- Add voice gateway skeleton.
- Add device configuration foundation.
- Move hardware paths out of GUI module by module.

Completed:

- `vehiclecommand` common parser.
- `voice_gateway` CMake project. *(Historical — retired 2026-07-18, A2; the active voice CMake project is `RK3588S_CarTerminal/voice/CMakeLists.txt`.)*
- Key input configuration.
- GPS serial configuration.
- Static map network configuration.
- Static map response validation and QLabel-based map rendering path.
- Weather date formatting and malformed response crash hardening, verified on RK3588S HDMI.
- Weather API configuration parser and disabled-request guard, host verified; RK3588S default-disabled runtime path verified, live provider verification pending.
- AP3216 config parsing and sysfs adapter host tests.

### Phase 2: Device and Sensor Isolation

Goals:

- Move DHT11, AP3216, LED/key, and other sensor code into device/service adapters.
- Keep GUI read-only with respect to hardware control.
- Replace global state and hardcoded paths with config.

Current progress:

- DHT11 device adapter and Qt worker-thread path are implemented and host verified.
- AP3216 sysfs adapter and Qt worker-thread path are implemented and host verified.
- AP3216 enabled board verification is blocked: RK3588S I2C/IIO/dmesg discovery currently exposes no AP3216/AP3216C node or illuminance/proximity/intensity channel. Keep default disabled unless hardware/device-tree/driver exposure changes.

### Phase 3: Media and Camera

Goals:

- Move V4L2 capture out of GUI.
- Make camera device path, resolution, pixel format, and buffer count configurable.
- Add frame delivery API for GUI preview.
- Prepare RTSP push/preview path.

Current board evidence:

- I2C discovery shows IMX415 camera sensors at `0-001a`, `1-001a`, and `2-001a`.
- `v4l2-ctl --list-devices` confirmed `/dev/video33` is rkisp0-vir0 mainpath (Video Capture Multiplanar).
- `v4l2-ctl -d /dev/video33 --list-formats-ext` confirmed UYVY/NV16/NV61/NV21/NV12/NM21/NM12 support, max 800x600.
- `media-ctl -p` shows `rockchip-csi2-dphy0` → `rockchip-mipi-csi2` (12 pads) → `rkcif` stream/scale/tools entities.

Completed:

- `capture_thread.*` rewritten: V4L2 MPLANE API, NV12/NV21/UYVY→RGB32 conversion, config-driven, proper cleanup.
- `rk3588s_migration/media/cameraconfig.*` added with host tests.
- `config/camera_settings.conf` added (device=/dev/video33, format=NV12, 800x600, 4 buffers, 30fps).
- Monitor page and mainwindow updated to pass CameraConfig.

Remaining:

- Board runtime verification of V4L2 capture.
- RGA hardware color conversion for performance.
- RTSP/RTMP streaming integration.

### Phase 4: Voice and AI

Goals:

- Integrate ASR, LLM, and TTS as services.
- Keep model paths in config.
- Use stable command interface to control GUI/service behavior.
- Prepare RKNN/NPU inference modules.

### Phase 5: System Integration

Goals:

- Add startup scripts.
- Add logging policy.
- Add system health/status page.
- Add board-specific build scripts.
- Add long-running stability tests.

## Mandatory Extra Reading By Task

Always read the 9 fixed docs first. Then read these task-specific files:

| Task | Extra reading |
|---|---|
| Change camera | `configs/camera_config.yaml`, `src/media/camera/`, `src/media/v4l2/`, or current legacy `IMX6ULL车载终端/VehicleTerminal/Monitor/capture_thread.*` if target paths do not exist yet |
| Change voice | `configs/voice_config.yaml`, `src/voice/`, Project B voice scripts, `rk3588s_migration/voice_gateway/` *(RETIRED 2026-07-18, A2 — read `RK3588S_CarTerminal/voice/` instead)* |
| Change sensors | `configs/device_config.yaml`, `src/drivers/`, Project A sensor code, current `rk3588s_migration/device/` |
| Change GUI | `src/gui/`, current `IMX6ULL车载终端/VehicleTerminal/`, `docs/INTERFACE_SPEC.md` |
| Change AI | `configs/model_config.yaml`, `src/ai_vision/`, Project B model scripts |
| Change RTSP | `src/media/rtsp/`, `configs/app_config.yaml`, current media/camera code |
| Change service layer | `src/services/`, `docs/INTERFACE_SPEC.md`, current `rk3588s_migration/voice_gateway/domain/` *(RETIRED 2026-07-18, A2 — service layer now lives in `RK3588S_CarTerminal/voice/`)* |
| Change build | `CMakeLists.txt`, `scripts/build_rk3588s.sh`, `docs/BUILD_RUN_TEST.md`, current `.pro` or CMake files |

Note: some target paths such as `configs/` and `src/` do not exist yet. Until they exist, read the closest current legacy or migration path listed above.

## Per-Module Migration Checklist

Before editing:

- Read all 9 fixed docs.
- This requirement also applies before reading source code for a task.
- Confirm the single module scope.
- Find all hardcoded paths and global state in that module.
- Identify current build target and test command.

During editing:

- Add or update tests before behavior changes when feasible.
- Keep public interfaces stable unless an interface change is explicitly required.
- Add config keys only with validation and documentation.
- Keep resource ownership clear.

After editing:

- Build or syntax-check the touched target.
- Run relevant tests.
- Update `docs/CHANGELOG.md` with date, task, added files, modified files, deleted files, reason, build result, test result, remaining issues, and next step.
- Update `docs/MODULE_STATUS.md` for every touched module.
- Update `docs/TODO.md` if new follow-up issues appear.
- Update `docs/INTERFACE_SPEC.md` if interfaces, data structures, message formats, or config contracts changed.
- Update `docs/DECISIONS.md` if a new technical direction or major implementation choice was made.
- Update `docs/BUILD_RUN_TEST.md` if new build, run, or test commands were added.
