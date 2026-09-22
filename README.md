# CherryUSB LVGL UVC Device

This ESP32-P4 example exposes a USB Video Class (UVC) camera whose image is
rendered by LVGL.

## Video format

- Resolution: 640 x 480
- Pixel format: uncompressed YUY2
- Frame rate: 5 FPS
- USB endpoint: high-speed isochronous IN, one 512-byte transaction per
  125 us microframe

One frame is 614400 bytes. The conservative single-transaction endpoint avoids
host and DWC2 interoperability problems with high-bandwidth isochronous mode,
while remaining compatible with CherryUSB's default 512-byte EP1 TX FIFO.

## Data path

LVGL runs its official Widgets Demo and renders RGB565 into a 640 x 480 virtual
display stored in PSRAM. Before
each UVC frame, the renderer is locked long enough to take a coherent snapshot
and convert it to packed YUY2 (`Y0 U Y1 V`). CherryUSB then divides that frame
into UVC payloads and submits them through the isochronous IN endpoint.

`lv_demo_widgets_start_slideshow()` automatically scrolls and switches the demo
because the virtual UVC display has no pointer input device. The USB capture
path does not need any demo-specific handling.

## Requirements

- ESP32-P4 with its USB 2.0 High-Speed/UTMI port correctly routed on the board
- PSRAM enabled (about 1.3 MB is used by the RGB565 framebuffer, LVGL draw
  buffer, and YUY2 output frame)
- CherryUSB video device class enabled

The implementation is in:

- `main/device_uvc_main.c`: descriptors, UVC interface, endpoint and stream task
- `main/lcd_lvgl.c`: virtual LVGL display, Widgets Demo and RGB565-to-YUY2 capture
