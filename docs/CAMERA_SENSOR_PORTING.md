# CAMERA_SENSOR_PORTING

## Week8–9 IMX335 Camera Porting Closure

Status: **PASS**

Date: 2026-08-08

Target platform: RK3588S QuarkPi CA2  
Target sensor module: ATK-MCIMX335  
Target physical port: CAMERA_1  
Target I2C controller: i2c2 / feaa0000.i2c  
Sensor address used by BSP: 0x1a  
Target CSI PHY: csi2_dcphy0  
MIPI data lanes: 4  
Raw format: Bayer RG10 / SRGGB10_1X10

## 1. Porting boundary

The Week8–9 implementation uses the existing Rockchip vendor IMX335 V4L2 sensor driver.

`drivers/media/i2c/imx335.c` was not functionally modified.

The required board adaptation is confined to:

`kernel/arch/arm64/boot/dts/rockchip/rk3588s-atk-camera.dtsi`

CAMERA_1 was changed from IMX415 to IMX335 while CAMERA_2 and CAMERA_3 remain IMX415.

The CAMERA_1 adaptation includes:

- compatible changed to `sony,imx335`;
- I2C address remains `0x1a`;
- MCLK remains Camera1 M1 clock;
- reset remains GPIO1_PB0 active-low;
- the previous IMX415 `power-gpios` property was removed because the vendor IMX335 driver does not consume a power/PWDN GPIO;
- module name changed to `ATK-MCIMX335`;
- the sensor endpoint label and reciprocal DPHY endpoint were changed to IMX335;
- four MIPI data lanes remain enabled.

The existing lens-name string is retained because the exact optical lens model of the physical ATK-MCIMX335 module has not been independently verified.

## 2. Existing vendor driver capability

The existing vendor `imx335.c` already contains the required Week9 implementation components:

- V4L2 I2C sub-device initialization;
- media entity and source pad;
- asynchronous sensor sub-device registration;
- media-bus format enumeration;
- frame-size and frame-interval enumeration;
- get/set format operations;
- `s_stream`;
- V4L2 exposure, gain, blanking, link-frequency and pixel-rate controls;
- XVCLK setup and enable/disable;
- reset GPIO control;
- regulator bulk handling;
- Runtime PM get/put;
- Runtime PM suspend/resume callbacks;
- probe/remove Runtime PM lifecycle.

No additional functional modification to `imx335.c` is required by the verified board results.

## 3. Board probe result

The board successfully bound IMX335 as:

`2-001a -> imx335`

The active device-tree node is:

`/i2c@feaa0000/imx335@1a`

The BSP sensor check reported:

`Detected imx335 id 000003`

This value is recorded only as the Rockchip BSP driver's existing sensor-identification criterion. It is not described here as an independently verified official Sony silicon identification register value.

The media graph exposed the sensor as a V4L2 sensor sub-device and linked it through:

IMX335 -> csi2-dcphy0 -> mipi0-csi2 -> RKCIF -> ISP

The sensor-side media-bus format was verified as:

`SRGGB10_1X10 / 2616x1964`

with crop bounds:

`2592x1944`

## 4. Raw capture result

The verified raw RKCIF capture node is:

`/dev/video0`

The active memory format was:

- width: 2592
- height: 1944
- pixel format: RG10
- bytes per line: 3328
- image size: 6469632 bytes

A single real raw frame was successfully captured.

A 300-frame stability test subsequently completed with:

- requested frames: 300
- stream return code: 0
- duration: approximately 10 seconds
- observed rate: approximately 30.00 fps
- stream start: PASS
- stream stop: PASS

Therefore the Week8–9 real-image data path from IMX335 through the Rockchip CSI/RKCIF path is verified operational.

## 5. Runtime PM board validation

The source audit confirmed that sensor stream-on executes Runtime PM acquisition and stream-off releases that Runtime PM reference.

During normal system operation, CarTerminal held:

`/dev/video42`

which is the `rkisp_mainpath` node of `rkisp0-vir1`.

While that camera consumer remained active, the IMX335 Runtime PM state remained:

`active`

This is expected while the sensor is in use.

After CarTerminal released its camera file descriptor, the board log showed the complete stop sequence:

- RKCIF stream stop;
- MIPI CSI2 stream OFF;
- CSI2 DPHY stream stop;
- `imx335 2-001a: s_stream: 0`.

After that release, the IMX335 sysfs Runtime PM state became:

`suspended`

and `runtime_suspended_time` continued increasing while idle.

Therefore:

- Runtime PM active while streaming: PASS
- Runtime PM suspend after the camera consumer releases the pipeline: PASS
- CarTerminal camera ownership causality: PASS
- additional IMX335 Runtime PM driver modification required: NO

## 6. Known warnings and limits

The BSP currently prints:

`Get hdr mode failed! no hdr default`

The driver explicitly falls back to non-HDR mode when the device tree does not provide an HDR mode property. Verified capture is non-HDR and this warning is not treated as a functional failure.

The BSP also prints:

`could not get sleep pinstate`

The existing board pinctrl configuration does not provide a separately verified IMX335 sleep pinctrl state.

The driver reports dummy regulators for:

- dvdd
- dovdd
- avdd

because the current QuarkPi camera node does not provide explicit supply phandles for those rails.

Accordingly, the Runtime PM board test proves the Linux PM lifecycle, stream-reference handling and driver power callbacks. It does **not** prove that the physical module AVDD/DOVDD/DVDD rails are electrically removed during Runtime PM suspend.

CAMERA_2 and CAMERA_3 remain configured as IMX415. Missing-sensor messages from unused camera ports are not treated as IMX335 failures.

## 7. Week8–9 closure

The verified Week8–9 IMX335 closure state is:

- device-tree adaptation: PASS
- kernel/DTB build: PASS
- boot-only image deployment: PASS
- IMX335 I2C probe: PASS
- V4L2 sub-device registration: PASS
- DPHY/CSI2/RKCIF media link: PASS
- first raw frame: PASS
- 300-frame real capture: PASS
- approximately 30 fps capture stability: PASS
- Runtime PM active-under-use: PASS
- Runtime PM idle suspend: PASS
- existing vendor `imx335.c` functional modification required: NO

Week8–9 IMX335 camera porting is therefore closed as **PASS**.
