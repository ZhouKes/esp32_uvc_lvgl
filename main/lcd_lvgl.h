#ifndef LCD_LVGL_H
#define LCD_LVGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef LVGL_UVC_WIDTH
#define LVGL_UVC_WIDTH       640
#endif

#ifndef LVGL_UVC_HEIGHT
#define LVGL_UVC_HEIGHT      480
#endif

#if (LVGL_UVC_WIDTH < 2) || (LVGL_UVC_WIDTH > 65535)
#error "LVGL_UVC_WIDTH must be in the range 2..65535"
#endif

#if (LVGL_UVC_WIDTH % 2) != 0
#error "LVGL_UVC_WIDTH must be even for packed YUY2"
#endif

#if (LVGL_UVC_HEIGHT < 1) || (LVGL_UVC_HEIGHT > 65535)
#error "LVGL_UVC_HEIGHT must be in the range 1..65535"
#endif

#if (LVGL_UVC_WIDTH * LVGL_UVC_HEIGHT * 2ULL) > 0xFFFFFFFFULL
#error "The YUY2 frame size must fit in 32 bits"
#endif

#define LVGL_UVC_YUYV_SIZE   (LVGL_UVC_WIDTH * LVGL_UVC_HEIGHT * 2U)

/* Starts an RGB565 LVGL display backed by PSRAM. */
bool lvgl_display_init(void);

/* Takes a coherent snapshot and converts RGB565 to packed YUY2 (Y0 U Y1 V). */
bool lvgl_display_copy_yuyv(uint8_t *destination, size_t destination_size);

#endif
