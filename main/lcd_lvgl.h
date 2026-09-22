#ifndef LCD_LVGL_H
#define LCD_LVGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LVGL_UVC_WIDTH       640U
#define LVGL_UVC_HEIGHT      480U
#define LVGL_UVC_YUYV_SIZE   (LVGL_UVC_WIDTH * LVGL_UVC_HEIGHT * 2U)

/* Starts a 640x480 RGB565 LVGL display backed by PSRAM. */
bool lvgl_display_init(void);

/* Takes a coherent snapshot and converts RGB565 to packed YUY2 (Y0 U Y1 V). */
bool lvgl_display_copy_yuyv(uint8_t *destination, size_t destination_size);

#endif
