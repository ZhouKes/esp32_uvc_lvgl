# CherryUSB + LVGL UVC USB 摄像头

[English](README.md) | 简体中文

本工程使用 CherryUSB 的 UVC Device 类模拟 USB 摄像头，并将 LVGL 官方
Widgets Demo 的渲染结果作为摄像头画面输出。工程同时支持 ESP32-P4 和
ESP32-S3，选择 ESP-IDF target 后会自动适配 USB 控制器、总线速度、端点配置
和帧率，无需修改源代码。

## 功能

- UVC 分辨率：640 × 480
- UVC 像素格式：未压缩 YUY2（`Y0 U Y1 V`）
- LVGL 内部像素格式：RGB565
- 运行 LVGL 官方 Widgets Demo
- 自动启动 Widgets Demo slideshow，无需触摸或鼠标输入
- ESP32-P4、ESP32-S3 编译时自动适配
- LVGL 帧缓冲和完整 YUY2 帧缓冲放置在 PSRAM

## 芯片差异

| 芯片 | USB 控制器 | USB 速度 | UVC 传输负载 | 输出帧率 |
| --- | --- | --- | --- | --- |
| ESP32-P4 | USB OTG 2.0 | High-Speed | 每个 125 µs microframe 传输 512 字节 | 5 FPS |
| ESP32-S3 | USB OTG 1.1 | Full-Speed | 每个 1 ms USB frame 传输 512 字节 | 约 0.67 FPS |

一帧 640 × 480 YUY2 图像需要 614400 字节。ESP32-S3 的原生 USB 外设仅支持
Full-Speed，无法以常规摄像头帧率传输该尺寸的未压缩图像，因此 S3 的帧间隔
设置为 1.5 秒。这是 USB 总线带宽限制，不是 LVGL 刷新速度限制。

ESP32-S3 的 CherryUSB 默认配置只为每个 IN 端点分配 64 字节 TX FIFO。本工程
启用了 `CONFIG_USB_DWC2_CUSTOM_FIFO`，回收未使用端点的 FIFO，将视频 EP1 的
TX FIFO 扩展为 512 字节，避免启动视频流时出现以下断言：

```text
ASSERT FAIL [(fifo_size * 4) >= USB_GET_MAXPACKETSIZE(...)]
Ep addr 81 fifo overflow
```

## 图像数据路径

```text
LVGL Widgets Demo
        ↓
640 × 480 RGB565 虚拟显示缓冲区（PSRAM）
        ↓
RGB565 转 YUY2
        ↓
640 × 480 YUY2 完整帧（PSRAM）
        ↓
CherryUSB UVC payload 分包
        ↓
USB Isochronous IN EP1
        ↓
电脑摄像头软件
```

LVGL 使用 40 行的局部绘制缓冲区，将刷新区域复制到完整虚拟帧缓冲区。UVC
任务取得互斥锁后生成一致的 YUY2 快照，再由 CherryUSB 分包发送。Widgets Demo
的创建、首次刷新和后续定时器处理均运行在独立的 16 KB LVGL 任务栈中。

## 主要文件

- `main/device_uvc_main.c`：UVC 描述符、视频流任务、芯片适配和 DWC2 FIFO 配置
- `main/lcd_lvgl.c`：虚拟显示、Widgets Demo、RGB565 到 YUY2 转换
- `main/lcd_lvgl.h`：分辨率、帧大小和显示接口
- `sdkconfig.defaults`：两个芯片共用的 CherryUSB、LVGL 和 PSRAM 配置
- `sdkconfig.defaults.esp32p4`：ESP32-P4 High-Speed USB 配置
- `sdkconfig.defaults.esp32s3`：ESP32-S3 Full-Speed USB 配置
- `CMakeLists.txt`：为 CherryUSB 全局启用自定义 DWC2 FIFO

`dwc2_get_user_fifo_config()` 与 `app_main()` 保持在同一个源文件中，确保 ESP-IDF
静态库链接时不会丢弃该回调。若将它移到独立、且没有其他强引用的源文件中，
可能出现 `undefined reference to dwc2_get_user_fifo_config`。

## 环境要求

- ESP-IDF 6.1.0（已测试）
- ESP32-P4 或 ESP32-S3
- 可用的 PSRAM，缓冲区合计约占用 1.3 MB
- 正确连接芯片原生 USB OTG 的 D+、D- 引脚
- 电脑端支持 UVC 的摄像头应用

当前项目已在 ESP-IDF 6.1.0 下测试通过，其他 ESP-IDF 版本尚未测试，可能需要
调整配置或处理 API 兼容性问题。

依赖由 `main/idf_component.yml` 管理：

- `cherry-embedded/cherryusb`：`1.5.2~2`
- `espressif/esp_lvgl_port`：`^2.6.0`

## 选择芯片并构建

ESP32-P4：

```sh
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

ESP32-S3：

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

ESP-IDF 会根据 target 自动加载对应的 `sdkconfig.defaults.<target>` 文件。切换芯片
后如果工程保留了旧的配置或 CMake 缓存，建议重新生成：

```sh
idf.py fullclean
idf.py set-target esp32s3   # 或 esp32p4
idf.py build
```

## 正常运行日志

启动后应能看到类似日志：

```text
I (...) lvgl_uvc_display: Virtual display ready: 640x480 RGB565
I (...) uvc_lvgl: UVC ready (Full-Speed): 640x480 YUY2 at 0.67 FPS
I (...) uvc_lvgl: UVC stream opened
I (...) lvgl_uvc_display: First UVC frame luma range: ...
I (...) uvc_lvgl: First UVC frame sent
```

P4 的第二行会显示 `High-Speed` 和 `5 FPS`。电脑打开摄像头后才会出现
`UVC stream opened`。

## 常见问题

### 电脑能识别摄像头，但画面全黑

检查串口日志：

1. 是否出现 `Virtual display ready`，确认 Widgets Demo 已完成首次刷新。
2. `First UVC frame luma range` 的最小值和最大值是否不同；若相同，LVGL 帧缓冲
   可能没有实际内容。
3. 是否出现 `UVC stream opened` 和 `First UVC frame sent`；若没有，说明主机没有
   打开视频流，或 USB 传输没有完成。
4. 确认摄像头应用选择的是 `LVGL USB Camera 640x480`。

### ESP32-S3 报 EP1 FIFO overflow

确认工程顶层 `CMakeLists.txt` 仍包含：

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_USB_DWC2_CUSTOM_FIFO" APPEND)
```

随后执行 `idf.py fullclean` 再构建，避免继续使用旧对象文件。

### 链接时报找不到 dwc2_get_user_fifo_config

当前实现已将回调放入 `main/device_uvc_main.c`，和 `app_main()` 一起参与链接。如果
仍然看到旧错误，通常是构建缓存尚未更新，可执行：

```sh
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

### ESP32-S3 画面更新很慢

这是预期行为。640 × 480 未压缩 YUY2 的单帧大小为 614400 字节，而 S3 仅支持
USB Full-Speed。若需要更高帧率，需要降低分辨率，或改用 MJPEG 等压缩格式。
