# CherryUSB + LVGL UVC USB Camera

English | [简体中文](README_zh-CN.md)

This project implements a USB Video Class device with CherryUSB and streams
the LVGL official Widgets Demo as the camera image. It supports both ESP32-P4
and ESP32-S3. Selecting the ESP-IDF target automatically configures the USB
controller, bus speed, endpoint parameters, and frame rate without requiring
source changes.

## Features

- Default UVC resolution: 640 × 480 (compile-time configurable)
- UVC pixel format: uncompressed YUY2 (`Y0 U Y1 V`)
- LVGL internal pixel format: RGB565
- Runs the official LVGL Widgets Demo
- Automatically starts the Widgets Demo slideshow, with no pointer input needed
- Compile-time adaptation for ESP32-P4 and ESP32-S3
- LVGL framebuffer and complete YUY2 frame buffer stored in PSRAM

## Target differences

| Target | USB controller | USB speed | UVC transfer payload | Output frame rate |
| --- | --- | --- | --- | --- |
| ESP32-P4 | USB OTG 2.0 | High-Speed | 512 bytes per 125 µs microframe | 5 FPS |
| ESP32-S3 | USB OTG 1.1 | Full-Speed | 512 bytes per 1 ms USB frame | Approximately 0.67 FPS |

A single 640 × 480 YUY2 frame occupies 614400 bytes. The native ESP32-S3 USB
peripheral supports Full-Speed only and cannot transfer uncompressed video at
this resolution at a conventional camera frame rate. The S3 frame interval is
therefore set to 1.5 seconds. This is a USB bus bandwidth limitation, not an
LVGL rendering limitation.

## Configuring the resolution

The default resolution is defined by two overridable macros in
`main/lcd_lvgl.h`:

```c
#ifndef LVGL_UVC_WIDTH
#define LVGL_UVC_WIDTH  640
#endif

#ifndef LVGL_UVC_HEIGHT
#define LVGL_UVC_HEIGHT 480
#endif
```

You can change the defaults directly or set the CMake cache values from the
command line:

```sh
idf.py -DLVGL_UVC_WIDTH=320 -DLVGL_UVC_HEIGHT=240 reconfigure
idf.py build
```

The top-level `CMakeLists.txt` maps these values to C compiler definitions. The
selected dimensions are then used automatically by the LVGL virtual display,
RGB565 framebuffer, YUY2 conversion, UVC descriptors, frame allocation,
runtime log, and USB product string.

The width must be an even integer from 2 through 65535 because packed YUY2
encodes pixels in pairs. The height must be from 1 through 65535. Frame memory
usage and required USB bandwidth increase with the selected pixel count; the
target-specific frame intervals are not automatically increased when a larger
resolution is selected.

The default CherryUSB configuration for ESP32-S3 allocates only 64 bytes of TX
FIFO to each IN endpoint. This project enables `CONFIG_USB_DWC2_CUSTOM_FIFO`,
reclaims FIFO space from unused endpoints, and assigns a 512-byte TX FIFO to
video EP1. This prevents the following assertion when the video stream starts:

```text
ASSERT FAIL [(fifo_size * 4) >= USB_GET_MAXPACKETSIZE(...)]
Ep addr 81 fifo overflow
```

## Image data path

```text
LVGL Widgets Demo
        ↓
640 × 480 RGB565 virtual display framebuffer (PSRAM)
        ↓
RGB565 to YUY2 conversion
        ↓
Complete 640 × 480 YUY2 frame (PSRAM)
        ↓
CherryUSB UVC payload splitting
        ↓
USB Isochronous IN EP1
        ↓
Host camera application
```

LVGL uses a 40-line partial draw buffer and copies updated regions into a full
virtual framebuffer. The UVC task takes a mutex while creating a coherent YUY2
snapshot, which CherryUSB then splits into USB payloads. Widgets Demo creation,
the first refresh, and subsequent timer handling run in a dedicated LVGL task
with a 16 KB stack.

## Main files

- `main/device_uvc_main.c`: UVC descriptors, streaming task, target adaptation,
  and DWC2 FIFO configuration
- `main/lcd_lvgl.c`: virtual display, Widgets Demo, and RGB565-to-YUY2 conversion
- `main/lcd_lvgl.h`: resolution, frame size, and display interface
- `sdkconfig.defaults`: common CherryUSB, LVGL, and PSRAM configuration
- `sdkconfig.defaults.esp32p4`: ESP32-P4 High-Speed USB configuration
- `sdkconfig.defaults.esp32s3`: ESP32-S3 Full-Speed USB configuration
- `CMakeLists.txt`: maps optional resolution values to compiler definitions and
  globally enables the custom CherryUSB DWC2 FIFO layout

`dwc2_get_user_fifo_config()` is intentionally kept in the same translation
unit as `app_main()`. This ensures that the ESP-IDF static-library linker retains
the callback. Moving it into a separate source file with no other strong
references can cause `undefined reference to dwc2_get_user_fifo_config`.

## Requirements

- ESP-IDF 6.1.0 (tested)
- ESP32-P4 or ESP32-S3
- PSRAM with approximately 1.3 MB available at the default 640 × 480 resolution
- The native USB OTG D+ and D- pins correctly routed
- A host camera application with UVC support

This project has been tested with ESP-IDF 6.1.0. Other ESP-IDF versions have
not been tested and may require configuration or API compatibility changes.

Dependencies are managed by `main/idf_component.yml`:

- `cherry-embedded/cherryusb`: `1.5.2~2`
- `espressif/esp_lvgl_port`: `^2.6.0`

## Selecting a target and building

ESP32-P4:

```sh
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

ESP32-S3:

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

ESP-IDF automatically loads the matching `sdkconfig.defaults.<target>` file.
If configuration or CMake cache files from the previous target remain after
switching chips, regenerate the build directory:

```sh
idf.py fullclean
idf.py set-target esp32s3   # or esp32p4
idf.py build
```

## Expected runtime log

After startup, the log should contain messages similar to these:

```text
I (...) lvgl_uvc_display: Virtual display ready: 640x480 RGB565
I (...) uvc_lvgl: UVC ready (Full-Speed): 640x480 YUY2 at 0.67 FPS
I (...) uvc_lvgl: UVC stream opened
I (...) lvgl_uvc_display: First UVC frame luma range: ...
I (...) uvc_lvgl: First UVC frame sent
```

On ESP32-P4, the second line reports `High-Speed` and `5 FPS`. The
`UVC stream opened` message appears only after a host application opens the
camera stream.

## Troubleshooting

### The camera is detected, but the image is black

Check the serial log:

1. Verify that `Virtual display ready` appears, which confirms that the Widgets
   Demo completed its first refresh.
2. Check whether the minimum and maximum values in
   `First UVC frame luma range` differ. Identical values may indicate that the
   LVGL framebuffer contains no rendered content.
3. Verify that both `UVC stream opened` and `First UVC frame sent` appear. If
   they do not, the host has not opened the stream or USB transfers are not
   completing.
4. Make sure the camera application selected
   `LVGL USB Camera <width>x<height>` (for example, the default device is
   `LVGL USB Camera 640x480`).

### ESP32-S3 reports EP1 FIFO overflow

Verify that the top-level `CMakeLists.txt` still contains:

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_USB_DWC2_CUSTOM_FIFO" APPEND)
```

Then run `idf.py fullclean` before rebuilding to prevent stale object files from
being reused.

### The linker cannot find dwc2_get_user_fifo_config

The current implementation keeps the callback in `main/device_uvc_main.c`
together with `app_main()`. If an old linker error remains, clear the build
cache and rebuild:

```sh
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

### Video updates slowly on ESP32-S3

This is expected. An uncompressed 640 × 480 YUY2 frame occupies 614400 bytes,
while ESP32-S3 supports USB Full-Speed only. A higher frame rate requires a
lower resolution or a compressed format such as MJPEG.
