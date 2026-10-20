# MODULE_STATUS

<!-- WEEK8_9_IMX335_BEGIN -->
## Week8–9 IMX335 Camera Porting — PASS

- Target: ATK-MCIMX335 on CAMERA_1
- I2C: i2c2 / 0x1a
- PHY: csi2_dcphy0
- Lanes: 4
- Raw capture: RG10 2592x1944
- First real frame: PASS
- 300-frame stability capture: PASS, approximately 30 fps
- V4L2 sensor/subdev/media link: PASS
- Runtime PM active while used: PASS
- Runtime PM idle suspend after camera consumer release: PASS
- Camera2/Camera3 IMX415 configuration preserved
- Vendor imx335.c functional modification: not required
- Board DTS adaptation: rk3588s-atk-camera.dtsi
- Porting document: docs/CAMERA_SENSOR_PORTING.md

Known boundary: the current DTS does not provide explicit IMX335 dvdd/dovdd/avdd supply phandles, so the Runtime PM validation proves the software PM lifecycle and driver power callbacks, not physical removal of all module power rails.
<!-- WEEK8_9_IMX335_END -->
