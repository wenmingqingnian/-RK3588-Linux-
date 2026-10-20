# Build Run Test

## 2026-07-17 Current-Worktree Verification Notice

The commands and pass statements later in this document include historical evidence. They must not be reported as verification of the current worktree unless rerun against the current files and logs are retained.

Current audit state:

| Check | State |
|---|---|
| Post-change Windows host build | `BUILD_NOT_EXECUTED_ENVIRONMENT_UNSUPPORTED` (no Qt compiler pair on Windows host) |
| Post-change host/unit/source-contract tests | RKNN source-contract, alarm-manager, buzzer, and 2026-07-17 media-pipeline tests (frame_packet, test_pattern_source, file_frame_source, frame_broker, media_pipeline, v4l2_capture_utils) passed; all exit 0 |
| RK3588S/AArch64 build | ✅ Native QuarkPi-CA2 qmake + full link passed on 2026-07-17 19:13:37 (`BUILD_RC=0`), including the media-pipeline additions (`frame_broker.cpp`, `media_pipeline.cpp`, `file_frame_source.cpp`, `test_pattern_source.cpp`) and the GUI throttle fix in `monitor.cpp`. |
| Current board build/run | ✅ RKNN CTest 4/4 (2026-07-16) + 2026-07-17 media-pipeline board combinations **A–E all PASSED** on QuarkPi-CA2. See `docs/MODULE_STATUS.md` section "2026-07-17 Board Validation Complete (Authoritative)" for the full matrix and log file references. |
| RKNN kernel-driver risk | ⚠ RKNPU driver 0.9.6 NULL-pointer dereference observed once (BSP layer, not application defect). Scenario B re-ran cleanly. Keep `rknn_enabled=false` by default. |

Use the active project path `RK3588S_CarTerminal`; the legacy `rk3588s_migration/voice_gateway/` component has been **formally retired (2026-07-18, task A2)** — the build commands in the next two sections are kept only as historical reference and MUST NOT be executed. The current voice stack lives under `RK3588S_CarTerminal/voice/` and is built via CMake (see "Build Qt VehicleTerminal On Board" + `voice/CMakeLists.txt`).

The exact current validation matrix and expected evidence are maintained in:

- `../../reports/audit_build_test_20260715.md`
- `../../reports/audit_board_validation_20260715.md`
- `../../reports/audit_pending_manual_review_20260715.md`

## Host Environment

Current workspace root:

```text
<LOCAL_SRC_DIR>
```

Host shell:

```text
PowerShell
```

Use `rg` for search whenever possible.

## Board Environment

Observed board:

```text
QuarkPi-CA2 RK3588S
Debian GNU/Linux
Linux 5.10.209 aarch64
Qt 5.15.2 packages installed
```

Board working directory:

```text
/home/quark/car
```

Typical board layout:

```text
/home/quark/car/VehicleTerminal
/home/quark/car/rk3588s_migration
```

## Build Voice Gateway On Board (RETIRED 2026-07-18, task A2)

> **DO NOT RUN.** The `rk3588s_migration/voice_gateway/` path is absent from
> the current workspace and the runner has been formally retired. The current
> voice stack lives under `RK3588S_CarTerminal/voice/` and is built via the
> CMake target defined in `voice/CMakeLists.txt` (see
> `docs/MODULE_STATUS.md` "Voice LLM Service" / "Voice TTS Service" /
> "Voice ASR Engine" rows for the active build instructions and board
> verification status). The commands below are retained only as historical
> reference for tracing the original Project B skeleton.

```bash
# HISTORICAL ONLY — path does not exist, commands will fail.
cd <PROJECT_DIR>
cmake -S rk3588s_migration/voice_gateway -B build_voice_gateway
cmake --build build_voice_gateway -j$(nproc)
./build_voice_gateway/voice_gateway_app rk3588s_migration/voice_gateway/config/voice_gateway.example.conf
```

Historical expected config log (for reference only):

```text
[INFO] loaded voice gateway config: llm=tcp://127.0.0.1:6666, tts=tcp://127.0.0.1:7777, status=tcp://127.0.0.1:6677
```

## Build Voice Gateway With ZeroMQ (RETIRED 2026-07-18, task A2)

> **DO NOT RUN.** Same retirement as above. ZMQ is now integrated directly
> in `RK3588S_CarTerminal/voice/zmq/` (see `Voice ZMQ` row in
> `docs/MODULE_STATUS.md`).

```bash
# HISTORICAL ONLY — path does not exist, commands will fail.
cmake -S rk3588s_migration/voice_gateway -B build_voice_gateway \
  -DVOICE_GATEWAY_ENABLE_ZMQ=ON \
  -DVOICE_GATEWAY_ZMQ_INCLUDE_DIR=<zmq-comm-kit/include> \
  -DVOICE_GATEWAY_ZMQ_COMPONENT_LIBRARY=<libzmq_component.so>
cmake --build build_voice_gateway -j$(nproc)
```

## Build Qt VehicleTerminal On Board

```bash
cd <PROJECT_DIR>/VehicleTerminal
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
```

If stale objects are suspected:

```bash
cd <PROJECT_DIR>/VehicleTerminal
make clean 2>/dev/null
rm -f *.o moc_*.o moc_*.cpp qrc_*.cpp VehicleTerminal
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
```

## Run Qt GUI On HDMI

```bash
cd <PROJECT_DIR>/VehicleTerminal
export DISPLAY=:0
export QT_XCB_GL_INTEGRATION=none
export QT_OPENGL=software
./VehicleTerminal
```

Recommended script:

```bash
#!/bin/sh

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
cd "$APP_DIR" || exit 1

export DISPLAY="${DISPLAY:-:0}"
export QT_XCB_GL_INTEGRATION="${QT_XCB_GL_INTEGRATION:-none}"
export QT_OPENGL="${QT_OPENGL:-software}"

runtime_uid=$(id -u)
fallback_runtime="/tmp/runtime-${runtime_uid}"

if [ -z "${XDG_RUNTIME_DIR:-}" ] || \
   [ "$XDG_RUNTIME_DIR" = "/run/user/0" ] || \
   [ ! -d "$XDG_RUNTIME_DIR" ] || \
   [ ! -w "$XDG_RUNTIME_DIR" ]; then
    export XDG_RUNTIME_DIR="$fallback_runtime"
fi

if ! mkdir -p "$XDG_RUNTIME_DIR"; then
    echo "failed to create XDG_RUNTIME_DIR: $XDG_RUNTIME_DIR" >&2
    exit 1
fi

if ! chmod 700 "$XDG_RUNTIME_DIR"; then
    echo "failed to chmod XDG_RUNTIME_DIR: $XDG_RUNTIME_DIR" >&2
    exit 1
fi

exec ./VehicleTerminal
```

Current versioned script path:

```text
<LEGACY_PROJECT>/VehicleTerminal/run_hdmi.sh
```

After uploading it to the board:

```bash
cd <PROJECT_DIR>/VehicleTerminal
chmod +x run_hdmi.sh
./run_hdmi.sh
```

If runtime still hits a segment fault, collect the backtrace with the same HDMI environment:

```bash
cd <PROJECT_DIR>/VehicleTerminal
export DISPLAY=:0
export QT_XCB_GL_INTEGRATION=none
export QT_OPENGL=software
export XDG_RUNTIME_DIR=/tmp/runtime-$(id -u)
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
gdb --args ./VehicleTerminal
run
bt
thread apply all bt
quit
```

## Verify Old Device Paths Are Removed

```bash
cd <PROJECT_DIR>/VehicleTerminal
strings ./VehicleTerminal | grep "/dev/key" || echo "OK: binary has no old key path"
strings ./VehicleTerminal | grep "/dev/ttymxc2" || echo "OK: binary has no old gps path"
strings ./VehicleTerminal | grep "/dev/dht11" || echo "OK: binary has no hardcoded dht11 path"
strings ./VehicleTerminal | grep "/sys/class/misc/ap3216c" || echo "OK: binary has no hardcoded ap3216 sysfs path"
```

Expected:

```text
OK: binary has no old key path
OK: binary has no old gps path
OK: binary has no hardcoded dht11 path
OK: binary has no hardcoded ap3216 sysfs path
```

## Host Tests

Run from workspace root:

```powershell
powershell.exe -ExecutionPolicy Bypass -File tests\vehicle_command\build_and_run.ps1
powershell.exe -ExecutionPolicy Bypass -File tests\voice_gateway\build_and_run.ps1
# NOTE (2026-07-18, A2): tests\voice_gateway\ is part of the retired
# rk3588s_migration/voice_gateway/ skeleton; this line is retained for
# historical traceability only and is no longer executed in the current
# verification flow. Active voice tests live under
# RK3588S_CarTerminal/voice/tests/ (ctest-registered).
powershell.exe -ExecutionPolicy Bypass -File tests\device_input\build_and_run.ps1
powershell.exe -ExecutionPolicy Bypass -File tests\map_config\build_and_run.ps1
powershell.exe -ExecutionPolicy Bypass -File tests\weather_format\build_and_run.ps1
powershell.exe -ExecutionPolicy Bypass -File tests\weather_config\build_and_run.ps1
powershell.exe -ExecutionPolicy Bypass -File tests\setting_gif_scanner\build_and_run.ps1
```

Expected:

```text
vehiclecommand tests passed
voice_gateway tests passed
# NOTE (2026-07-18, A2): historical output of the retired
# tests\voice_gateway\build_and_run.ps1 runner; no longer produced.
device_input tests passed
map_config tests passed
weather_format tests passed
weather_config tests passed
setting_gif_scanner tests passed
```

## Host qmake Syntax Check

Windows qmake may print path noise when Chinese paths are involved. Use relative output path:

```powershell
New-Item -ItemType Directory -Force -Path build_vehicleterminal_qmake_check | Out-Null
Push-Location '<LEGACY_PROJECT>/Monitor sensor migration, also verify:

```powershell
Select-String -LiteralPath 'build_vehicleterminal_qmake_check\Makefile.Release' -Pattern 'ap3216device.cpp','ap3216worker.cpp','moc_ap3216worker.cpp','Monitor/ap3216.cpp','Monitor/monitor.cpp'
```

For the Settings GIF startup fix, also verify:

```powershell
Select-String -LiteralPath 'build_vehicleterminal_qmake_check\Makefile.Release' -Pattern 'settinggifscanner.cpp','settinggifscanner.h','settingwindow.cpp'
```

## Upload Settings GIF Startup Fix To Board

PowerShell upload commands:

```powershell
ssh quark@<BOARD_IP> "mkdir -p /home/quark/car/rk3588s_migration/gui /home/quark/car/VehicleTerminal"
scp "rk3588s_migration/gui/settinggifscanner.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/gui/
scp "rk3588s_migration/gui/settinggifscanner.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/gui/
scp "<LEGACY_PROJECT>/VehicleTerminal/settingwindow.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/settingwindow.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/VehicleTerminal.pro" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/run_hdmi.sh" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
```

Then rebuild and run on the RK3588S board:

```bash
cd <PROJECT_DIR>/VehicleTerminal
chmod +x run_hdmi.sh
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
./run_hdmi.sh
```

Expected behavior when `./MyGif` is missing or empty:

```text
setting gif directory has no usable gif files: ".../MyGif"
```

The app should continue startup and must not crash in:

```text
SettingWindow::ScanGif()
QMovie::setFileName(QString const&)
```

Observed RK3588S result after uploading and rebuilding:

```text
setting gif directory has no usable gif files: "/home/quark/car/VehicleTerminal/MyGif"
weather input disabled
dht11 input disabled
key input disabled; speech recognition trigger is not started
map image saved "map.png" 62452
```

This confirms the missing/empty `./MyGif` path no longer crashes startup.

If a new segment fault remains, collect the next backtrace:

```bash
cd <PROJECT_DIR>/VehicleTerminal
export DISPLAY=:0
export QT_XCB_GL_INTEGRATION=none
export QT_OPENGL=software
export XDG_RUNTIME_DIR=/tmp/runtime-$(id -u)
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
gdb --args ./VehicleTerminal
run
bt
thread apply all bt
quit
```

## Common Board Warnings

Warnings currently known and not necessarily fatal:

```text
QMetaObject::connectSlotsByName: No matching signal ...
Could not create pixmap from :/img/images/tmp.png
Unknown property rotation-point
Unknown property rotation
qt.qpa.xcb: QXcbConnection: XCB error: 148 ...
map network error or map reply data is empty
Netjson not an jsonObject!
```

Expected disabled-device logs when config disables devices:

```text
gps input disabled
key input disabled; speech recognition trigger is not started
dht11 input disabled
ap3216 input disabled
```

## Upload Common Files To Board

Example PowerShell upload commands:

```powershell
ssh quark@<BOARD_IP> "mkdir -p /home/quark/car/rk3588s_migration/device /home/quark/car/rk3588s_migration/common /home/quark/car/VehicleTerminal/config"
scp "rk3588s_migration/device/vehicledeviceconfig.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/vehicledeviceconfig.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/keyinputdevice.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/keyinputdevice.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "<LEGACY_PROJECT>/VehicleTerminal/config/vehicle_devices.conf" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/config/
```

Upload module-specific Qt files as needed.

## Upload DHT11 Device Files To Board

```powershell
ssh quark@<BOARD_IP> "mkdir -p /home/quark/car/rk3588s_migration/device /home/quark/car/VehicleTerminal/config"
scp "rk3588s_migration/device/dht11device.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/dht11device.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/vehicledeviceconfig.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/vehicledeviceconfig.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "<LEGACY_PROJECT>/VehicleTerminal/dht11.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/dht11.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/dht11worker.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/dht11worker.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/mainwindow.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/mainwindow.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/VehicleTerminal.pro" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/config/vehicle_devices.conf" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/config/
```

After uploading, rebuild and run:

```bash
cd <PROJECT_DIR>/VehicleTerminal
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
./run_hdmi.sh
```

Expected DHT11 behavior when config is disabled (default):

```text
dht11 input disabled
```

To enable DHT11 on a board with the device available, edit `vehicle_devices.conf`:

First verify the IIO sysfs node is present (DHT11 uses the in-tree IIO driver `drivers/iio/humidity/dht11.c`, NOT a `/dev/dht11` character device):

```bash
ls /sys/bus/iio/devices/                                      # 应见到 iio:deviceN
for d in /sys/bus/iio/devices/iio:device*; do \
    echo "$d: $(cat $d/name 2>/dev/null)"; \
done
# 期望某行: /sys/bus/iio/devices/iio:deviceN: dht11
dmesg | grep -i dht
zcat /proc/config.gz 2>/dev/null | grep CONFIG_DHT11          # 期望 =y 或 =m
```

Observed RK3588S result on 2026-06-24 (and unchanged through 2026-07-18):

```text
no DHT11 IIO sysfs node, kernel log, or loaded module was found
```

Do not enable DHT11 until the kernel exposes `CONFIG_DHT11` and the device tree declares a `compatible="dht11"` node with a free GPIO pin.

```bash
cd <PROJECT_DIR>/VehicleTerminal/config
vi vehicle_devices.conf
```

Set, replacing the path with the real IIO sysfs directory (or leave `dht11_sysfs_dir` empty to auto-scan):

```text
dht11_enabled=true
# 方式一：显式指定 IIO sysfs 目录
dht11_sysfs_dir=/sys/bus/iio/devices/iio:device0
# 方式二：留空让 Dht11Device 自动扫描 iio:device*/name=="dht11"
# dht11_sysfs_dir=
dht11_poll_interval_ms=2000
```

Then run the app again. A config-only change does not require rebuilding. When DHT11 is working, the GUI humidity and temperature labels will update periodically, and `availabilityChanged(true)` / `updateDht11Data(QString,QString)` signals arrive on the GUI thread.

Verify old hardcoded DHT11 device path is no longer in the binary:

```bash
cd <PROJECT_DIR>/VehicleTerminal
strings ./VehicleTerminal | grep "/dev/dht11" || echo "OK: binary has no hardcoded dht11 path"
strings ./VehicleTerminal | grep "/dev/key" || echo "OK: binary has no old key path"
strings ./VehicleTerminal | grep "/dev/ttymxc2" || echo "OK: binary has no old gps path"
```

Observed RK3588S result:

```text
OK: binary has no hardcoded dht11 path
OK: binary has no old key path
OK: binary has no old gps path
```

## Upload AP3216 Monitor Sensor Files To Board

```powershell
ssh quark@<BOARD_IP> "mkdir -p /home/quark/car/rk3588s_migration/device /home/quark/car/VehicleTerminal/Monitor /home/quark/car/VehicleTerminal/config"
scp "rk3588s_migration/device/ap3216device.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/ap3216device.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/vehicledeviceconfig.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "rk3588s_migration/device/vehicledeviceconfig.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/device/
scp "<LEGACY_PROJECT>/VehicleTerminal/Monitor/ap3216.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Monitor/
scp "<LEGACY_PROJECT>/VehicleTerminal/Monitor/ap3216.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Monitor/
scp "<LEGACY_PROJECT>/VehicleTerminal/Monitor/ap3216worker.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Monitor/
scp "<LEGACY_PROJECT>/VehicleTerminal/Monitor/ap3216worker.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Monitor/
scp "<LEGACY_PROJECT>/VehicleTerminal/Monitor/monitor.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Monitor/
scp "<LEGACY_PROJECT>/VehicleTerminal/Monitor/monitor.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Monitor/
scp "<LEGACY_PROJECT>/VehicleTerminal/mainwindow.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/VehicleTerminal.pro" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/config/vehicle_devices.conf" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/config/
```

After uploading, rebuild and run:

```bash
cd <PROJECT_DIR>/VehicleTerminal
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
./run_hdmi.sh
```

Expected AP3216 behavior when config is disabled (default):

```text
ap3216 input disabled
```

To enable AP3216 on a board with the sysfs files available, first discover the real directory and channel names.

Avoid using only broad patterns such as `*ps*` or `*ir*`; they match unrelated paths such as `oops`, `psci`, `irq`, and `firmware`.

First check I2C devices and driver names:

```bash
ls /sys/bus/i2c/devices
for d in /sys/bus/i2c/devices/*; do
    [ -f "$d/name" ] && printf "%s " "$d" && cat "$d/name"
done
```

Then check IIO-style sensor channels:

```bash
find /sys/bus/iio/devices -maxdepth 3 -type f \
  \( -name name -o -name "in_illuminance*" -o -name "in_proximity*" -o -name "in_intensity*" \) \
  -print -exec cat {} \; 2>/dev/null
```

Then check AP3216-specific names in the device tree and sysfs:

```bash
find /sys/bus/i2c /sys/class /sys/devices -maxdepth 6 \
  \( -iname "*ap3216*" -o -iname "*ap3216c*" -o -iname "*illuminance*" -o -iname "*proximity*" \) \
  2>/dev/null

dmesg | grep -iE "ap3216|ap3216c|illuminance|proximity|iio"
```

Then edit:

```bash
cd <PROJECT_DIR>/VehicleTerminal/config
vi vehicle_devices.conf
```

Set, replacing the placeholder directory and file names with the real board values:

```text
ap3216_enabled=true
ap3216_sysfs_dir=/sys/path/to/ap3216
ap3216_als_file=als
ap3216_ps_file=ps
ap3216_ir_file=ir
ap3216_poll_interval_ms=500
```

A config-only AP3216 enable/disable change does not require rebuilding. Restart the app after editing the config.

Observed RK3588S result:

```text
find /sys -maxdepth 5 \( -iname "*ap3216*" -o -iname "*als*" -o -iname "*ps*" -o -iname "*ir*" \)
```

This broad command returned many unrelated paths and no usable AP3216 sysfs node. The narrower discovery result below supersedes the earlier pending state.

Observed narrower RK3588S result:

```text
/sys/bus/i2c/devices/0-001a imx415
/sys/bus/i2c/devices/0-0042 rk8602
/sys/bus/i2c/devices/0-0043 rk8603
/sys/bus/i2c/devices/1-0011 es8388
/sys/bus/i2c/devices/1-001a imx415
/sys/bus/i2c/devices/2-001a imx415
/sys/bus/i2c/devices/2-0042 rk8602
/sys/bus/i2c/devices/2-004e husb311
/sys/bus/i2c/devices/i2c-0 rk3x-i2c
/sys/bus/i2c/devices/i2c-1 rk3x-i2c
/sys/bus/i2c/devices/i2c-10 ddc
/sys/bus/i2c/devices/i2c-2 rk3x-i2c
/sys/bus/i2c/devices/i2c-9 fde50000.dp
```

Observed IIO result:

```text
find /sys/bus/iio/devices -maxdepth 3 -type f \( -name name -o -name "in_illuminance*" -o -name "in_proximity*" -o -name "in_intensity*" \)
no output
```

Observed AP3216-specific sysfs and dmesg result:

```text
find /sys/bus/i2c /sys/class /sys/devices -maxdepth 6 \( -iname "*ap3216*" -o -iname "*ap3216c*" -o -iname "*illuminance*" -o -iname "*proximity*" \)
no output

dmesg | grep -iE "ap3216|ap3216c|illuminance|proximity|iio"
no output
```

Conclusion: the current board image does not expose AP3216/AP3216C. The `imx415` entries are camera sensors. Keep `ap3216_enabled=false` and move the next board-facing software migration to Camera/V4L2 unless AP3216 hardware/device-tree/driver exposure changes.

Verify old hardcoded AP3216 sysfs path is no longer in the binary:

```bash
cd <PROJECT_DIR>/VehicleTerminal
strings ./VehicleTerminal | grep "/sys/class/misc/ap3216c" || echo "OK: binary has no hardcoded ap3216 sysfs path"
strings ./VehicleTerminal | grep "/dev/dht11" || echo "OK: binary has no hardcoded dht11 path"
strings ./VehicleTerminal | grep "/dev/key" || echo "OK: binary has no old key path"
strings ./VehicleTerminal | grep "/dev/ttymxc2" || echo "OK: binary has no old gps path"
```

## Discover Camera/V4L2 Devices On Board

Run these commands before modifying camera code:

```bash
cd <PROJECT_DIR>/VehicleTerminal
ls -l /dev/video* /dev/media* 2>/dev/null
command -v v4l2-ctl || echo "v4l2-ctl not installed"
command -v media-ctl || echo "media-ctl not installed"
v4l2-ctl --list-devices
media-ctl -p
for n in /dev/video*; do
    echo "==== $n ===="
    v4l2-ctl -d "$n" --all
    v4l2-ctl -d "$n" --list-formats-ext
done
```

If `v4l2-ctl` or `media-ctl` is missing, first record the `/dev/video*` and `/dev/media*` output. Install `v4l-utils` only when package access and board policy allow it.

## Build Camera/V4L2 Module On Board

After uploading the rewritten `capture_thread.*`, `cameraconfig.*`, and `camera_settings.conf`:

```bash
cd <PROJECT_DIR>/VehicleTerminal
qmake && make -j$(nproc)
```

Expected new object files:

```
cameraconfig.o
capture_thread.o
moc_capture_thread.o
```

## Verify Camera Config Parsing On Board

```bash
cd <PROJECT_DIR>/VehicleTerminal
strings ./VehicleTerminal | grep "/dev/video33" && echo "OK: camera config device path found"
strings ./VehicleTerminal | grep "camera_settings.conf" && echo "OK: camera config file reference found"
strings ./VehicleTerminal | grep "/dev/video1" || echo "OK: old hardcoded /dev/video1 removed"
```

## Run VehicleTerminal With Camera Enabled

```bash
cd <PROJECT_DIR>/VehicleTerminal
export DISPLAY=:0
export QT_XCB_GL_INTEGRATION=none
export QT_OPENGL=software
./VehicleTerminal
```

Expected log output (camera enabled):

```
camera: negotiated format: NV12 800x600 (requested 800x600)
camera: 4 buffers mapped
camera: stream started
camera: capturing from /dev/video33 800x600 fmt=4e563132
```

Expected log output (camera disabled in config):

```
camera: capture disabled by config
```

Expected log output (camera device not available):

```
camera: failed to open /dev/video33
```

## Quick Camera Capture Test With v4l2-ctl

Before running the full application, verify the camera produces frames:

```bash
v4l2-ctl -d /dev/video33 --set-fmt-video=width=800,height=600,pixelformat=NV12 --stream-mmap --stream-count=1 --stream-to=/tmp/test.nv12
ls -la /tmp/test.nv12
# Expected size: 800 * 600 * 1.5 = 720000 bytes
```

## Switch Camera Device

To use a different rkisp instance, edit `config/camera_settings.conf`:

```bash
# rkisp0-vir0 (default)
camera_device=/dev/video33

# rkisp0-vir1 (second camera)
camera_device=/dev/video42

# rkisp1-vir1 (third camera)
camera_device=/dev/video51
```

Or override with environment variable:

```bash
VEHICLE_CAMERA_CONFIG=/path/to/other_camera.conf ./VehicleTerminal
```

## Upload Map Configuration Files To Board

```powershell
ssh quark@<BOARD_IP> "mkdir -p /home/quark/car/rk3588s_migration/map /home/quark/car/VehicleTerminal/Map /home/quark/car/VehicleTerminal/config"
scp "rk3588s_migration/map/staticmapconfig.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/map/
scp "rk3588s_migration/map/staticmapconfig.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/map/
scp "rk3588s_migration/map/staticmapresponse.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/map/
scp "rk3588s_migration/map/staticmapresponse.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/map/
scp "<LEGACY_PROJECT>/VehicleTerminal/Map/baidumap.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Map/
scp "<LEGACY_PROJECT>/VehicleTerminal/Map/baidumap.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Map/
scp "<LEGACY_PROJECT>/VehicleTerminal/VehicleTerminal.pro" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/config/map_settings.conf" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/config/
```

After rebuild, verify old hardcoded map strings are no longer in the binary:

```bash
cd <PROJECT_DIR>/VehicleTerminal
strings ./VehicleTerminal | grep "api.map.baidu.com" || echo "OK: binary has no hardcoded map URL"
strings ./VehicleTerminal | grep "61n6OaTvjjomVyN1AJui2mDlCKsU4WEE" || echo "OK: binary has no hardcoded map AK"
strings ./VehicleTerminal | grep "/dev/key" || echo "OK: binary has no old key path"
strings ./VehicleTerminal | grep "/dev/ttymxc2" || echo "OK: binary has no old gps path"
```

Expected map startup URL when GPS is disabled:

```text
center=123.43147,41.80570
```

## Diagnose Board Map DNS/Network And API State

If map runtime prints `QNetworkReply::HostNotFoundError`, check DNS and routing on the RK3588S board:

```bash
ip addr
ip route
cat /etc/resolv.conf
getent hosts api.map.baidu.com
ping -c 3 223.5.5.5
ping -c 3 api.map.baidu.com
curl -I "http://api.map.baidu.com/staticimage/v2"
```

Root-cause notes:

- If `ping 223.5.5.5` fails, fix board network or gateway first.
- If IP ping works but `getent hosts api.map.baidu.com` fails, fix DNS such as `/etc/resolv.conf`.
- If DNS works but `curl` fails, inspect firewall/proxy/API service connectivity.

If `curl` is unavailable, use Python to verify DNS, HTTP, and the Baidu response body:

```bash
python3 - <<'PY'
import urllib.request

url = "http://api.map.baidu.com/staticimage/v2?ak=61n6OaTvjjomVyN1AJui2mDlCKsU4WEE&width=1024&height=600&scale=1&copyright=1&center=123.43147,41.80570&markers=123.43147,41.80570&zoom=10"
r = urllib.request.urlopen(url, timeout=10)
print("status:", r.status)
print("content-type:", r.headers.get("Content-Type"))
print(r.read().decode("utf-8", errors="replace"))
PY
```

Observed board result:

```text
status: 200
content-type: text/html
{"message":"APP被用户自己禁用，请在控制台解禁","status":"201"}
```

This means board DNS, HTTP connectivity, and URL construction are usable. The old blocker was the Baidu Maps application/API key state. Enable the APP in the Baidu console or replace `map_api_key` in `config/map_settings.conf`, then rerun the Python check. A valid static map response should return an image content type such as `image/png` or `image/jpeg`.

Observed replacement-AK result:

```text
status: 200
content-type: image/png
first-bytes: b'\x89PNG\r\n\x1a\n...'
```

After uploading the QLabel map-rendering change and rebuilding, run:

```bash
cd <PROJECT_DIR>/VehicleTerminal
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
./run_hdmi.sh
```

Expected map log after a successful image response:

```text
map image saved "map.png" <bytes>
```

## Upload Weather Crash Hardening Files To Board

```powershell
ssh quark@<BOARD_IP> "mkdir -p /home/quark/car/rk3588s_migration/weather /home/quark/car/VehicleTerminal/Weather"
scp "rk3588s_migration/weather/weatherformat.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/weather/
scp "rk3588s_migration/weather/weatherformat.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/weather/
scp "<LEGACY_PROJECT>/VehicleTerminal/Weather/weather.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Weather/
scp "<LEGACY_PROJECT>/VehicleTerminal/Weather/weather.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Weather/
scp "<LEGACY_PROJECT>/VehicleTerminal/VehicleTerminal.pro" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
```

After uploading:

```bash
cd <PROJECT_DIR>/VehicleTerminal
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
./run_hdmi.sh
```

Expected weather behavior:

```text
map image saved "map.png" <bytes>
```

The app should not crash in:

```text
Weather::TransDataToMyData(QString)
```

If the provider returns malformed or short data, expected logs include one of:

```text
weather data array is missing or too short
weather day has invalid date
```

Observed board result after Weather crash hardening:

```text
weather data array is missing or too short 0 "{\"errcode\":100,\"errmsg\":\"免费权限已过期!\"}"
map image saved "map.png" 62247
```

This means the crash guard is effective. The remaining Weather blocker is provider permission/configuration, not a date parser crash.

If a segment fault still occurs, collect a board backtrace:

```bash
cd <PROJECT_DIR>/VehicleTerminal
gdb --args ./VehicleTerminal
run
bt
quit
```

## Upload Weather Configuration Files To Board

```powershell
ssh quark@<BOARD_IP> "mkdir -p /home/quark/car/rk3588s_migration/weather /home/quark/car/VehicleTerminal/Weather /home/quark/car/VehicleTerminal/config"
scp "rk3588s_migration/weather/weatherconfig.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/weather/
scp "rk3588s_migration/weather/weatherconfig.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/weather/
scp "rk3588s_migration/weather/weatherformat.h" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/weather/
scp "rk3588s_migration/weather/weatherformat.cpp" quark@<BOARD_IP>:/home/quark/car/rk3588s_migration/weather/
scp "<LEGACY_PROJECT>/VehicleTerminal/Weather/weather.h" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Weather/
scp "<LEGACY_PROJECT>/VehicleTerminal/Weather/weather.cpp" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/Weather/
scp "<LEGACY_PROJECT>/VehicleTerminal/VehicleTerminal.pro" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/
scp "<LEGACY_PROJECT>/VehicleTerminal/config/weather_settings.conf" quark@<BOARD_IP>:/home/quark/car/VehicleTerminal/config/
```

The checked-in default keeps live weather disabled:

```text
weather_enabled=false
```

After uploading, edit `weather_settings.conf` on the board to enable live weather and fill valid `weather_appid` and `weather_appsecret`:

```bash
cd <PROJECT_DIR>/VehicleTerminal/config
vi weather_settings.conf
```

Then rebuild and run:

```bash
cd <PROJECT_DIR>/VehicleTerminal
rm -f Makefile .qmake.stash
qmake VehicleTerminal.pro
make -j$(nproc)
./run_hdmi.sh
```

Expected weather behavior when config is disabled or credentials are empty:

```text
weather input disabled
```

If the weather refresh button is pressed while config is disabled or invalid, expected log:

```text
weather request skipped: config disabled or invalid
```

Expected weather behavior when config is enabled with valid credentials:

```text
Weather page loads with provider data
```

Observed RK3588S result after Weather config review fixes:

```text
weather input disabled
map image saved "map.png" 62247
```

This confirms the default-disabled Weather config path runs without a Weather crash after board rebuild.

Verify old hardcoded weather strings are no longer in the binary:

```bash
cd <PROJECT_DIR>/VehicleTerminal
strings ./VehicleTerminal | grep "82455931" || echo "OK: binary has no old weather appid"
strings ./VehicleTerminal | grep "X2Z4OvM5" || echo "OK: binary has no old weather appsecret"
```

## Verify Shared RKNN Postprocess

The production detector and Stage B CLI now compile the same
`media/rknn_postprocess.cpp`. The checked-in safety gate remains
`rknn_enabled=false`.

Fresh Windows host verification from the project root on 2026-07-15 used
unique outputs under `C:\tmp` so no pre-existing executable could satisfy a
run. The strict common test command was:

```powershell
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = "C:\tmp\rknn_common_task6_$stamp.exe"
<QT_DIR>\Tools\mingw730_64\bin\g++.exe -std=c++17 -Wall -Wextra -Werror `
  tests/rknn/test_rknn_image_common.cpp media/rknn_postprocess.cpp -o $out
if ($LASTEXITCODE -ne 0) { throw "common compile failed: $LASTEXITCODE" }
& $out
$commonRc = $LASTEXITCODE
Write-Output "COMMON_EXIT=$commonRc"
if ($commonRc -ne 0) { throw "common run failed: $commonRc" }
```

Observed result: `PASS=122 FAIL=0`, process exit 0.

The adapter build defined the project root as a quoted compiler macro rather
than relying on the current directory:

```powershell
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$root = 'E:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal'
$define = '-DRKNN_TEST_PROJECT_ROOT=\"' + $root + '\"'
$out = "C:\tmp\rknn_adapter_task6_$stamp.exe"
<QT_DIR>\Tools\mingw730_64\bin\g++.exe -std=c++17 -Wall -Wextra -Werror `
  $define tests/rknn/test_rknn_postprocess_adapter.cpp `
  media/rknn_postprocess.cpp -o $out
if ($LASTEXITCODE -ne 0) { throw "adapter compile failed: $LASTEXITCODE" }
& $out
$adapterRc = $LASTEXITCODE
Write-Output "ADAPTER_EXIT=$adapterRc"
if ($adapterRc -ne 0) { throw "adapter run failed: $adapterRc" }
```

Observed result: `PASS=79 FAIL=0`, process exit 0.

Fresh alarm and buzzer regressions were also rebuilt with unique `C:\tmp`
binaries:

```powershell
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$alarmOut = "C:\tmp\alarm_manager_task6_$stamp.exe"
<QT_DIR>\Tools\mingw730_64\bin\g++.exe -std=c++17 -O2 -Wall -Wextra -Werror `
  -Icommon tests/alarm/test_alarmmanager.cpp common/alarmmanager.cpp `
  common/alarmpolicy.cpp -o $alarmOut
if ($LASTEXITCODE -ne 0) { throw "alarm compile failed: $LASTEXITCODE" }
& $alarmOut
$alarmRc = $LASTEXITCODE
Write-Output "ALARM_EXIT=$alarmRc"
if ($alarmRc -ne 0) { throw "alarm run failed: $alarmRc" }

$buzzerOut = "C:\tmp\buzzer_task6_$stamp.exe"
<QT_DIR>\Tools\mingw730_64\bin\g++.exe -std=c++17 -O2 -Wall -Wextra -Werror `
  -Icommon tests/buzzer/test_buzzer.cpp device/vehicledeviceconfig.cpp `
  device/buzzerdevice.cpp common/logmanager.cpp -pthread `
  -o $buzzerOut
if ($LASTEXITCODE -ne 0) { throw "buzzer compile failed: $LASTEXITCODE" }
& $buzzerOut
$buzzerRc = $LASTEXITCODE
Write-Output "BUZZER_EXIT=$buzzerRc"
if ($buzzerRc -ne 0) { throw "buzzer run failed: $buzzerRc" }
```

Observed results were `alarm manager and policy tests passed` and
`buzzer tests passed`, both with exit 0.

## GPS Host Tests (NMEA / Stream / Config / Simulation)

GPS software stack tests are Qt-free and compile with plain g++ under
`-std=c++17 -Wall -Wextra -Werror`. Run all four suites from the project root:

```powershell
# NMEA parser: GPRMC/GNRMC/GPGGA/GNGGA, checksum, coordinate conversion,
# NaN/Inf rejection, range checks, overlong rejection, unknown-sentence skip.
g++ -std=c++17 -Wall -Wextra -Werror -Icommon -Idevice `
  tests/gps/test_nmeaparser.cpp common/nmeaparser.cpp -o test_nmeaparser.exe
./test_nmeaparser.exe

# Streaming reassembler: split reads, multi-sentence reads, noise resync,
# invalid-then-valid, half-sentence, buffer cap, reset, LF-only.
g++ -std=c++17 -Wall -Wextra -Werror -Icommon -Idevice `
  tests/gps/test_gpsstream.cpp common/nmeaparser.cpp -o test_gpsstream.exe
./test_gpsstream.exe

# Config: both-defaults-false, mutual-exclusion reject, empty-path reject,
# invalid baudrate/timeout/interval, duplicate/unknown keys, valid configs.
g++ -std=c++17 -Wall -Wextra -Werror -Icommon -Idevice `
  tests/gps/test_gpsconfig.cpp device/vehicledeviceconfig.cpp -o test_gpsconfig.exe
./test_gpsconfig.exe

# Simulation: continuous trajectory, simulated=true, missing/empty/all-invalid
# file failures, stop-no-push, repeated rewind, destructor cleanup, loop at EOF.
g++ -std=c++17 -Wall -Wextra -Werror -Icommon -Idevice `
  tests/gps/test_gpssimulation.cpp common/nmeaparser.cpp device/gpssimulator.cpp `
  -o test_gpssimulation.exe
./test_gpssimulation.exe
```

Observed (2026-07-16): all four suites pass with exit 0:

```text
nmeaparser tests passed
gpsstream tests passed
gpsconfig tests passed
gpssimulation tests passed
```

Device-layer and worker-layer source compiles cleanly on Windows (Linux APIs
guarded by `#ifdef __linux__`):

```powershell
g++ -std=c++17 -Wall -Wextra -Werror -Icommon -Idevice -c device/gpsdevice.cpp -o nul
g++ -std=c++17 -Wall -Wextra -Werror -Icommon -Idevice -c device/gpssimulator.cpp -o nul
```

The Qt worker (`device/gpsworker.cpp`) and GUI integration
(`gui/Map/baidumap.cpp`) require Qt headers + moc; verify with qmake on the
board. The `.pro` file parses with exit 0 on Windows qmake 5.12.11.

### GPS QuarkPi-CA2 Simulation Verification (2026-07-17)

Fresh board evidence from the final files:

- strict `-std=c++17 -O2 -Wall -Wextra -Werror` builds and runs passed for
  `test_nmeaparser`, `test_gpsstream`, `test_gpsconfig`, and
  `test_gpssimulation`; all four recorded return code 0;
- the full qmake application build returned 0 and the final link contained
  `nmeaparser.o`, `gpsdevice.o`, `gpssimulator.o`, `gpsworker.o`, and
  `moc_gpsworker.o`;
- with both GPS modes false, the application reached ready, handled SIGINT,
  exited with code 0, did not log `gps opened`, and left no process;
- with real GPS false and simulation true, the worker logged simulation start
  with 14 NMEA sentences, the GUI displayed `GPS模拟定位`, nonzero coordinates,
  speed, course, and satellite count, and coordinates changed over 5-10 s;
- clean SIGINT logged simulation-loop exit and joined/released the worker;
  application, worker-start, simulation-start, simulation-exit, and worker-stop
  checks all returned 0;
- simulation did not log `gps opened`, no `CarTerminal` process remained, and
  `gps_enabled=false` plus `gps_simulation_enabled=false` were restored with
  exactly one key each.

The repeated worker-stop log is produced by idempotent stop calls from owner
and worker destruction; no thread/process leak was observed. This completes
software and board-simulation verification only. Real UART/GNSS node discovery,
antenna acquisition, cold/warm start, real trajectory/speed accuracy, and
physical disconnect/reconnect remain hardware-pending.

Run both source gates directly; this avoids claiming that host CTest built the
RKNN-dependent board targets:

```powershell
<QT_DIR>\cmake\bin\cmake.exe `
  '-DPROJECT_ROOT=E:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal' `
  -P tests/rknn/test_rknn_detector_source.cmake
<QT_DIR>\cmake\bin\cmake.exe `
  '-DSTAGEB_SOURCE=E:/单片机/Linux_project/AI智能车载终端改进版本/RK3588S_CarTerminal/tests/rknn/rknn_image_test.cpp' `
  -P tests/rknn/test_rknn_image_stageb_source.cmake
```

Both returned exit 0; the detector gate printed
`rknn_detector production source regression passed`. Fresh static checks also
confirmed exactly one production `decodeOutputs` call; no local production
`sigmoid`, `computeIoU`, `nmsFilter`, `validateYoloOutput`, `decodeYoloLayer`,
or old resize helper definition/call; one postprocess `.cpp` in qmake SOURCES
and one `.h` in HEADERS; production postprocess reuse by common, adapter, and
Stage B CMake targets; no old `rknn_image_common.cpp/.h`; canonical metadata
present; `rknn_enabled=false`; and no `BuzzerDevice`/`setBuzzerAlarm`
dependency in `Monitor`.

The following copy/paste static contract check supports each conclusion in
the preceding paragraph. Run it from the project root after the two CMake
source gates:

```powershell
$ErrorActionPreference = 'Stop'
$detector = Get-Content media/rknn_detector.cpp -Raw
$detectorCode = [regex]::Replace($detector, '(?s)/\*.*?\*/', '')
$detectorCode = [regex]::Replace($detectorCode, '(?m)//.*$', '')

$decodeCount = [regex]::Matches($detectorCode, 'decodeOutputs\s*\(').Count
$oldDecoderCount = [regex]::Matches(
  $detectorCode,
  'static\s+inline\s+float\s+sigmoid\s*\(|computeIoU\s*\(|nmsFilter\s*\(|validateYoloOutput\s*\(|decodeYoloLayer\s*\('
).Count
$oldResizeCount = [regex]::Matches(
  $detectorCode,
  'static\s+[^;{}]*(resizeRgb|resizeRGB|letterboxResize|computeLetterbox|applyLetterbox)\s*\('
).Count
if ($decodeCount -ne 1 -or $oldDecoderCount -ne 0 -or $oldResizeCount -ne 0) {
  throw "detector contract failed: decode=$decodeCount oldDecoder=$oldDecoderCount oldResize=$oldResizeCount"
}

$scope = ''
$proCppCount = 0
$proHeaderCount = 0
foreach ($rawLine in Get-Content CarTerminal.pro) {
  $line = $rawLine -replace '#.*$', ''
  if ($line -match '^\s*SOURCES\s*\+=') { $scope = 'SOURCES' }
  elseif ($line -match '^\s*HEADERS\s*\+=') { $scope = 'HEADERS' }
  if ($line -match 'rknn_postprocess\.cpp') {
    if ($scope -ne 'SOURCES') { throw 'rknn_postprocess.cpp is outside SOURCES' }
    ++$proCppCount
  }
  if ($line -match 'rknn_postprocess\.h') {
    if ($scope -ne 'HEADERS') { throw 'rknn_postprocess.h is outside HEADERS' }
    ++$proHeaderCount
  }
  if ($line -notmatch '\\\s*$') { $scope = '' }
}
if ($proCppCount -ne 1 -or $proHeaderCount -ne 1) {
  throw "qmake contract failed: cpp=$proCppCount header=$proHeaderCount"
}

$testCMake = Get-Content tests/rknn/CMakeLists.txt -Raw
$cmakeProductionReuse = [regex]::Matches(
  $testCMake, '\.\./\.\./media/rknn_postprocess\.cpp'
).Count
if ($cmakeProductionReuse -ne 3) {
  throw "expected three production postprocess CMake uses, found $cmakeProductionReuse"
}

$oldDuplicates = @(
  'tests/rknn/rknn_image_common.cpp',
  'tests/rknn/rknn_image_common.h'
) | Where-Object { Test-Path $_ }
if ($oldDuplicates.Count -ne 0) { throw "old duplicates remain: $oldDuplicates" }
if (-not (Test-Path config/yolov5s_metadata.conf)) { throw 'canonical metadata missing' }
if (-not (Select-String -Path config/rknn_settings.conf `
                       -Pattern '^rknn_enabled=false\s*$' -Quiet)) {
  throw 'rknn_enabled=false safety gate missing'
}
if (Select-String -Path gui/Monitor/monitor.h,gui/Monitor/monitor.cpp `
                  -Pattern 'BuzzerDevice|setBuzzerAlarm' -Quiet) {
  throw 'Monitor has a direct buzzer dependency'
}

Write-Output "decodeOutputs=$decodeCount oldDecoder=$oldDecoderCount oldResize=$oldResizeCount"
Write-Output "CarTerminal.pro cpp=$proCppCount header=$proHeaderCount"
Write-Output "CMake production reuse=$cmakeProductionReuse oldDuplicates=$($oldDuplicates.Count)"
Write-Output 'metadata=present rknn_enabled=false MonitorBuzzerDependency=0'
Write-Output 'RKNN_STATIC_CONTRACT=PASS'
```

Expected final output includes `decodeOutputs=1 oldDecoder=0 oldResize=0`,
`CarTerminal.pro cpp=1 header=1`, `CMake production reuse=3
oldDuplicates=0`, and `RKNN_STATIC_CONTRACT=PASS`.

For the targeted Qt host check, map the source temporarily to an ASCII drive,
run Qt 5.12.11 qmake into `C:\tmp`, and compile only the two relevant release
objects. Windows needs the existing host-only `tests/stubs/dlfcn.h` include:

```powershell
subst R: '<LOCAL_SRC_DIR>\RK3588S_CarTerminal'
cd C:\tmp\rknn_qt_task6_stub_20260715
<QT_DIR>\5.12.11\mingw73_64\bin\qmake.exe CarTerminal.pro `
  -spec win32-g++ 'INCLUDEPATH+=R:/tests/stubs'
<QT_DIR>\Tools\mingw730_64\bin\mingw32-make.exe -f Makefile.Release `
  release/rknn_detector.o release/rknn_postprocess.o
subst R: /D
```

Observed result: both objects compiled, make exit 0, and `subst` cleanup exit
0. qmake explicitly reported that `rknn_api.h` was absent and therefore built
the disabled detector stub. This is not evidence that the board-only RKNN
branch or the full application compiles.

Prior QuarkPi-CA2 evidence, before the production unification, was:

- Stage A: `STAGE_A_VERDICT=APPROVE`, probe exit 0, stability 10/10, and zero
  new NPU dmesg errors;
- Stage B: five final detections including `bus`, annotated output written,
  `RKNN_IMAGE_RESULT=PASS`, exit 0;
- board common test: `PASS=94 FAIL=0`, exit 0.

Fresh Task 7 QuarkPi-CA2 evidence for the unified implementation on 2026-07-15:

- `build_rknn_shared` compiled the board RKNN targets against
  `/usr/local/include/rknn_api.h`; CTest discovered and passed all 4/4 tests:
  detector source, common, adapter, and Stage B source; exit 0.
- Canonical-metadata Stage B matched model SHA
  `85287823c5909c4f47396c4fdbc53db0abc2ed411c981f8fa872df649572dac9`,
  completed `INPUTS_SET`, `RUN`, `OUTPUTS_GET`, and `OUTPUTS_RELEASE` with 0,
  retained five detections including `bus`, wrote the 346 KiB
  `/tmp/bus_annotated_shared.jpg`, printed `RKNN_IMAGE_RESULT=PASS`, and
  returned `RKNN_SHARED_STAGE_B_RC=0`.
- A clean qmake build linked `rknn_detector.o` and `rknn_postprocess.o` into
  `CarTerminal` and returned `CARTERMINAL_SHARED_BUILD_RC=0`.
- With `rknn_enabled=false`, the normal `quark` user opened
  `/dev/gpiochip4` line 3, reached `application ready`, handled SIGINT through
  the application's signal timer, logged `application exiting (code=0)`,
  restored `buzzer setAlarm: active=0 level=1`, and returned
  `CARTERMINAL_DISABLED_APP_RC=0`.
- The first lifecycle attempt used `... | tee ...`; Ctrl+C terminated `tee`
  and caused SIGPIPE exit 141. Repeating the same lifecycle with direct log
  redirection (no pipe) proved the application shutdown path itself returns 0.

Task 7 is complete. Task 8 progressed through controlled board-only enablement
and detector initialization, but stopped before camera-frame inference because
no real camera is currently connected. The first enable attempt correctly
restored the disabled configuration after production rejected the board's
native FLOAT16 tensors as `qnt=AFFINE`; a probe then established that the input
and all outputs use the identity parameters `zp=0`, `scale=1` and that a full
inference still returns `RKNN_PROBE_RESULT=PASS`.

The TDD fix accepts `QNT_NONE` or identity AFFINE only and rejects DFP,
unknown quantization, nonzero zero point, non-unit scale, NaN, and infinity.
Fresh post-fix evidence is:

- host RKNN CTest 4/4, alarm regression, and buzzer regression passed;
- board RKNN CTest 4/4 returned `RKNN_AFFINE_CTEST_RC=0`;
- board canonical Stage B again retained five detections including `bus`,
  wrote a 346 KiB image, printed `RKNN_IMAGE_RESULT=PASS`, and returned 0;
- forced qmake recompilation used `/usr/local/include/rknn_api.h`, rebuilt
  `rknn_detector.o` and `rknn_postprocess.o`, relinked `CarTerminal`, and
  returned `RKNN_AFFINE_FORCED_BUILD_RC=0`;
- controlled live initialization matched model SHA
  `85287823c5909c4f47396c4fdbc53db0abc2ed411c981f8fa872df649572dac9`,
  accepted the NHWC FLOAT16 identity-AFFINE input, accepted three NCHW FLOAT16
  identity-AFFINE outputs, derived grids/strides 80/8, 40/16, and 20/32, logged
  `RKNN YOLOv5 detector initialized`, and reached `application ready`.

Still hardware-pending: `/dev/video33` real-frame capture, live RKNN frame
execution, three-frame qualifying-person trigger, ten-frame clear, repeating
200 ms on / 800 ms off buzzer behavior, and shutdown after that live sequence.
The intended configured camera is IMX415 over MIPI CSI through
`rkisp0-vir0` (`/dev/video33`, NV12 800x600), not a USB camera. If the final
hardware instead uses USB, its node, pixel format, and single/multi-planar
capture contract must be discovered before changing the config. DHT11 live
input remains a separate hardware-pending item. Keep the tracked default
`rknn_enabled=false` until the camera/live-alarm gate succeeds.

The explicit RKNN shutdown lifecycle gate was completed on 2026-07-16 without
requiring camera hardware. `MainWindow::~MainWindow()` now calls the existing
idempotent `Monitor::myStop()` after clearing alarm state and before destroying
the buzzer. The source-contract test isolates both functions, checks teardown
ordering, requires exactly one guarded `deinitDetector()` call, and rejects a
singleton-ownership refactor.

Fresh QuarkPi-CA2 evidence from the final files:

- RKNN source-contract test returned `RKNN_RELEASE_SOURCE_TEST_RC=0`;
- `build_rknn_shared` CTest passed 4/4 with zero failures and
  `RKNN_RELEASE_CTEST_RC=0`;
- qmake returned `RKNN_RELEASE_QMAKE_RC=0`; the build recompiled
  `mainwindow.cpp` and `monitor.cpp`, linked `CarTerminal`, and returned
  `RKNN_RELEASE_MAIN_BUILD_RC=0`;
- a controlled `rknn_enabled=true` run initialized the YOLOv5 detector without
  opening the Monitor page, reached `application ready`, handled SIGINT, logged
  `application exiting (code=0)`, then logged `RKNN detector released` and
  `buzzer setAlarm: active=0 level=1`;
- `RKNN_RELEASE_APP_RC`, `RKNN_INIT_LOG_RC`, `RKNN_RELEASE_LOG_RC`, and
  `RKNN_EXIT_LOG_RC` were all 0;
- no `CarTerminal` process remained, the config contained exactly one
  `rknn_enabled` key, and it was explicitly restored to `false`.

This closes only the software initialization/exit-release gate. Real camera
frames, danger-object trigger/clear behavior, the repeating buzzer pattern
during a live detection, and post-camera shutdown remain hardware-pending.

## Media Pipeline Host Tests (Frame Packet / Test Pattern / File Source / Broker / Pipeline)

The 2026-07-17 media-pipeline tests are Qt-free and compile with plain g++
under `-std=c++17 -Wall -Wextra -Werror`. All five suites must be run from
the project root because the file-source tests write and read sample NV12
files under the relative path `tests/media/build/`.

Compile and run all five suites plus the existing alarm and v4l2 utility
regressions (PowerShell, host g++ at `<QT_DIR>\Tools\mingw730_64\bin\g++.exe`):

```powershell
$gxx = '<QT_DIR>\Tools\mingw730_64\bin\g++.exe'
$flags = '-std=c++17','-O2','-Wall','-Wextra','-Werror','-pthread'

# 1. FramePacket shared-ownership and metadata.
& $gxx @flags -Imedia tests/media/test_frame_packet.cpp -o tests/media/build/test_frame_packet.exe
if ($LASTEXITCODE -ne 0) { throw "frame_packet compile failed: $LASTEXITCODE" }
& .\tests\media\build\test_frame_packet.exe
if ($LASTEXITCODE -ne 0) { throw "frame_packet run failed: $LASTEXITCODE" }

# 2. TestPatternSource: NV12 color-bar + moving block, monotonic seq/timestamp.
& $gxx @flags -Imedia tests/media/test_test_pattern_source.cpp media/test_pattern_source.cpp -o tests/media/build/test_test_pattern_source.exe
if ($LASTEXITCODE -ne 0) { throw "test_pattern_source compile failed: $LASTEXITCODE" }
& .\tests\media\build\test_test_pattern_source.exe
if ($LASTEXITCODE -ne 0) { throw "test_pattern_source run failed: $LASTEXITCODE" }

# 3. FileFrameSource: missing file, bad size, loop, no-loop EOF, stop, reopen.
& $gxx @flags -Imedia tests/media/test_file_frame_source.cpp media/file_frame_source.cpp -o tests/media/build/test_file_frame_source.exe
if ($LASTEXITCODE -ne 0) { throw "file_frame_source compile failed: $LASTEXITCODE" }
& .\tests\media\build\test_file_frame_source.exe
if ($LASTEXITCODE -ne 0) { throw "file_frame_source run failed: $LASTEXITCODE" }

# 4. FrameBroker: 3 bounded lanes, drop_oldest, stop-unblocks-take.
& $gxx @flags -Imedia tests/media/test_frame_broker.cpp media/frame_broker.cpp -o tests/media/build/test_frame_broker.exe
if ($LASTEXITCODE -ne 0) { throw "frame_broker compile failed: $LASTEXITCODE" }
& .\tests\media\build\test_frame_broker.exe
if ($LASTEXITCODE -ne 0) { throw "frame_broker run failed: $LASTEXITCODE" }

# 5. MediaPipeline: source+broker+3 consumer threads, 20x start/stop, destructor join.
& $gxx @flags -Imedia tests/media/test_media_pipeline.cpp media/media_pipeline.cpp media/frame_broker.cpp media/test_pattern_source.cpp media/file_frame_source.cpp -o tests/media/build/test_media_pipeline.exe
if ($LASTEXITCODE -ne 0) { throw "media_pipeline compile failed: $LASTEXITCODE" }
& .\tests\media\build\test_media_pipeline.exe
if ($LASTEXITCODE -ne 0) { throw "media_pipeline run failed: $LASTEXITCODE" }

# Existing regressions that gate the camera-path fix.
& $gxx @flags -Imedia tests/test_v4l2_capture_utils.cpp media/v4l2_capture_utils.cpp -o tests/media/build/test_v4l2_capture_utils.exe
if ($LASTEXITCODE -ne 0) { throw "v4l2_capture_utils compile failed: $LASTEXITCODE" }
& .\tests\media\build\test_v4l2_capture_utils.exe
if ($LASTEXITCODE -ne 0) { throw "v4l2_capture_utils run failed: $LASTEXITCODE" }

& $gxx @flags -Icommon tests/alarm/test_alarmmanager.cpp common/alarmmanager.cpp common/alarmpolicy.cpp -o tests/media/build/test_alarmmanager.exe
if ($LASTEXITCODE -ne 0) { throw "alarm compile failed: $LASTEXITCODE" }
& .\tests\media\build\test_alarmmanager.exe
if ($LASTEXITCODE -ne 0) { throw "alarm run failed: $LASTEXITCODE" }
```

Observed (2026-07-17, run from project root `RK3588S_CarTerminal`): all seven
suites pass with exit 0:

```text
=== test_frame_packet ===
frame_packet tests passed
exit=0
=== test_test_pattern_source ===
test_pattern_source tests passed
exit=0
=== test_file_frame_source ===
file_frame_source tests passed
exit=0
=== test_frame_broker ===
frame_broker tests passed
exit=0
=== test_media_pipeline ===
media_pipeline tests passed
exit=0
=== test_alarmmanager ===
alarm manager and policy tests passed
exit=0
=== test_v4l2_capture_utils ===
v4l2 capture utility tests passed
exit=0
```

Note: `test_file_frame_source` writes sample NV12 files to
`tests/media/build/` using a project-root-relative path; it must be launched
from the project root. Launching it from inside `tests/media/build/` will
cause path resolution to fail and may crash the test process.

While the application is running, filter `[MEDIA_STATS]` in its log. Every
five seconds it reports source FPS/count and, for GUI/RKNN/RTSP, processed
FPS/count, drops, current/capacity/maximum queue depth, and average/P95
callback time. A final snapshot is emitted at pipeline stop. Disabled lanes
should remain at zero.

### Static check: no QThread::terminate() in capture_thread

```powershell
Select-String -Path media\capture_thread.cpp,media\capture_thread.h -Pattern 'terminate\(\)'
# Expected: no matches
```

### Board validation matrix (PENDING_BOARD_VALIDATION)

The following combinations must be executed on QuarkPi-CA2 before the
2026-07-17 media-pipeline work can be promoted from
`SOURCE_IMPLEMENTED + HOST_TESTED` to `BOARD_TESTED_*`:

| Combination | source_type | GUI | RKNN | RTSP | Expected |
|---|---|---|---|---|---|
| A | test_pattern | on | off | off | GUI shows moving pattern; no crash over 30 min |
| B | file | on | on | off | GUI shows frames; RKNN logs detections; alarm cooldown works |
| C | file | on | on | on | GUI + RKNN + VLC playback of `rtsp://<ip>:8554/live` |
| D | file | off (page closed) | off | on | RTSP keeps streaming after Monitor page is closed |
| E | (missing file) | on | off | off | GUI + log show explicit error; app does not crash; can restart with valid file |

For each combination record: input resolution/fps, GUI fps, RKNN avg/P95
latency, encode fps, RTSP client count, per-lane dropped frames, max queue
depth, process RSS, CPU%, NPU% (if available), temperature, run duration,
and any crash/deadlock/leak. Minimum stability gate: 30 min continuous run,
then 2 hours; repeated Monitor page enter/exit; repeated pipeline
start/stop; RTSP client reconnect; RKNN dynamic disable/enable; file loop
at EOF; clean shutdown leaves no residual thread/process.

These board runs are **PENDING_BOARD_VALIDATION** and must not be reported
as PASS until executed on RK3588S with logs retained.

## Verify MPP Encoder Root-Cause Fix On RK3588S

Hardware baseline already observed on 2026-06-27:

```bash
mpi_enc_test -w 800 -h 600 -f 4 -t 7 -n 10 -o /tmp/mpp_test.h264
ls -lh /tmp/mpp_test.h264
```

Observed output size: 42 KiB. This proves the board VPU, kernel driver, and system
MPP library can encode H.264.

Compile the repaired direct-link regression test:

```bash
cd ~/RK3588S_CarTerminal
g++ -std=c++17 -O2 -Wall -Wextra \
    tests/test_mpp_direct.cpp \
    -lrockchip_mpp -lpthread \
    -o test_mpp_direct
./test_mpp_direct 2>&1 | tee test_mpp_direct.log
ls -lh test_mpp_direct.h264
od -An -tx1 -N16 test_mpp_direct.h264
```

Expected:

```text
MPP_FMT_YUV420P=4
Frame 0: ret=0, packet=<non-zero> bytes, Annex-B=yes
...
Encoded 10/10 frames, total=<non-zero> bytes
=== RESULT: PASS ===
```

Then compile the dlopen adapter test:

```bash
cd ~/RK3588S_CarTerminal
g++ -std=c++17 -O2 -Wall -Wextra \
    tests/test_mpp_encoder.cpp \
    media/mpp_encoder.cpp common/logmanager.cpp \
    -Imedia -Icommon -ldl -lpthread \
    -o test_mpp_encoder
./test_mpp_encoder 2>&1 | tee test_mpp_encoder.log
ls -lh test_output.h264
```

Finally rebuild the Qt application:

```bash
cd ~/RK3588S_CarTerminal
qmake CarTerminal.pro
make -j"$(nproc)"
```

Do not mark MPP Encoder Done until both standalone tests produce non-empty
Annex-B streams and the RTSP MPP path has been exercised.

## Verify Active Buzzer On QuarkPi-CA2

Verified hardware mapping:

```text
Board: QuarkPi-CA2 RK3588S
Signal: GPIO4_A3
GPIO character device: /dev/gpiochip4
Line offset: 3
Polarity: active-low (0 = beep, 1 = silent)
```

Build and run the host/unit test on the board:

```bash
cd ~/RK3588S_CarTerminal/tests/buzzer
g++ -std=c++17 -O2 -Wall -Wextra \
  -I../../common \
  test_buzzer.cpp \
  ../../device/vehicledeviceconfig.cpp \
  ../../device/buzzerdevice.cpp \
  ../../common/logmanager.cpp \
  -pthread \
  -o test_buzzer
./test_buzzer
```

Verified result:

```text
buzzer tests passed
```

Build and run the physical board test as the normal `quark` user:

```bash
g++ -std=c++17 -O2 -Wall -Wextra \
  -I../../common \
  test_buzzer_board.cpp \
  ../../device/vehicledeviceconfig.cpp \
  ../../device/buzzerdevice.cpp \
  ../../common/logmanager.cpp \
  -pthread \
  -o test_buzzer_board
./test_buzzer_board
```

Verified result:

```text
silent for 1 second
beep for 200 ms
silent for 1 second
buzzer board test passed
```

Persistent GPIO permission setup is stored in:

```text
scripts/install_buzzer_gpio_permissions.sh
```

After a fresh board image, install it once and then log out or reboot:

```bash
cd ~/RK3588S_CarTerminal
chmod +x scripts/install_buzzer_gpio_permissions.sh
bash -n scripts/install_buzzer_gpio_permissions.sh
sudo ./scripts/install_buzzer_gpio_permissions.sh
```

Verify the normal user permission after logging in again:

```bash
id -nG | grep gpio
ls -l /dev/gpiochip4
```

Verified permission state:

```text
quark is a member of gpio
/dev/gpiochip4 is owned by root:gpio with mode crw-rw----
```

Build and run the full application:

```bash
cd ~/RK3588S_CarTerminal
qmake CarTerminal.pro
make -j"$(nproc)"
./scripts/run_hdmi.sh
```

Verified application behavior:

```text
Full application link completed successfully.
Buzzer GPIO opened without sudo.
Application startup remained silent.
Application shutdown remained silent.
```

The driver, GPIO mapping, permissions, physical output, full build, and safe
open/close lifecycle are verified.

## Verify Multi-Source Alarm Integration

The alarm integration uses a central logical OR so one source cannot silence
another active source. The current policies are:

- temperature: trigger at 40 C, clear at 38 C;
- danger classes: person, bicycle, car, motorcycle, bus, and truck;
- minimum normalized detection area: 0.12;
- danger debounce: 3 dangerous frames to trigger and 10 safe frames to clear;
- buzzer pattern: 200 ms on and 800 ms off.

Build and run the Qt-free policy test:

```bash
cd ~/RK3588S_CarTerminal
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -Icommon \
  tests/alarm/test_alarmmanager.cpp \
  common/alarmmanager.cpp common/alarmpolicy.cpp \
  -o tests/alarm/test_alarmmanager
./tests/alarm/test_alarmmanager
```

Expected result:

```text
alarm manager and policy tests passed
```

Build and run the simulated-input diagnostic against the real GPIO as the
normal `quark` user:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  -Icommon -Idevice \
  tests/alarm/test_alarm_board.cpp \
  common/alarmmanager.cpp common/alarmpolicy.cpp common/logmanager.cpp \
  device/vehicledeviceconfig.cpp device/buzzerdevice.cpp \
  -pthread -o tests/alarm/test_alarm_board
./tests/alarm/test_alarm_board
```

Then rebuild and run the application:

```bash
qmake CarTerminal.pro
make -j"$(nproc)"
./scripts/run_hdmi.sh
```

Verification boundary as of 2026-07-14:

- Host alarm-policy tests passed.
- The changed `MainWindow`, `Monitor`, and Qt MOC targets compile successfully.
- The normal-user board diagnostic produced the expected short physical pulse,
  printed `alarm board diagnostic passed`, and returned exit code 0.
- The updated full RK3588S application compiled and linked successfully.
- The application opened `/dev/gpiochip4` line 3 as the normal `quark` user,
  reached `application ready`, exited with code 0 after SIGINT, and wrote
  active-low silent level 1 during shutdown.
- Real DHT11 triggering remains pending because no DHT11 device node exists.
- Live RKNN danger triggering remains pending until RKNN is enabled and exercised
  with real camera inference.
