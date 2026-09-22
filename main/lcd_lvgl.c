/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lcd_lvgl.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "demos/lv_demos.h"

#define LVGL_DRAW_BUFFER_LINES 40U
#define LVGL_DRAW_BUFFER_SIZE \
    (LVGL_UVC_WIDTH * LVGL_DRAW_BUFFER_LINES * sizeof(uint16_t))

static const char *TAG = "lvgl_uvc_display";

static lv_display_t *s_display;
static uint16_t *s_framebuffer;
static void *s_draw_buffer;
static SemaphoreHandle_t s_framebuffer_mutex;
static SemaphoreHandle_t s_lvgl_ready;

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void lvgl_flush(lv_display_t *display, const lv_area_t *area,
                       uint8_t *pixels)
{
    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t x2 = area->x2 >= (int32_t)LVGL_UVC_WIDTH ?
                 (int32_t)LVGL_UVC_WIDTH - 1 : area->x2;
    int32_t y2 = area->y2 >= (int32_t)LVGL_UVC_HEIGHT ?
                 (int32_t)LVGL_UVC_HEIGHT - 1 : area->y2;

    if (x1 <= x2 && y1 <= y2 &&
        xSemaphoreTake(s_framebuffer_mutex, portMAX_DELAY) == pdTRUE) {
        const size_t source_stride =
            (size_t)(area->x2 - area->x1 + 1) * sizeof(uint16_t);
        const size_t copy_width = (size_t)(x2 - x1 + 1) * sizeof(uint16_t);
        const uint8_t *source = pixels +
            ((size_t)(y1 - area->y1) * source_stride) +
            ((size_t)(x1 - area->x1) * sizeof(uint16_t));

        for (int32_t y = y1; y <= y2; ++y) {
            memcpy(&s_framebuffer[(size_t)y * LVGL_UVC_WIDTH + x1],
                   source, copy_width);
            source += source_stride;
        }
        xSemaphoreGive(s_framebuffer_mutex);
    }

    lv_display_flush_ready(display);
}

static void lvgl_task(void *arg)
{
    (void)arg;

    /* Widgets Demo creation and its first full refresh are deliberately kept
     * on this larger stack. Rendering it from app_main overflows main's stack. */
    lv_demo_widgets();
    lv_demo_widgets_start_slideshow();
    lv_refr_now(s_display);
    xSemaphoreGive(s_lvgl_ready);

    while (true) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < 1U) {
            delay_ms = 1U;
        } else if (delay_ms > 20U) {
            delay_ms = 20U;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

bool lvgl_display_init(void)
{
    const size_t framebuffer_size =
        LVGL_UVC_WIDTH * LVGL_UVC_HEIGHT * sizeof(uint16_t);

    s_framebuffer_mutex = xSemaphoreCreateMutex();
    s_lvgl_ready = xSemaphoreCreateBinary();
    s_framebuffer = heap_caps_malloc(framebuffer_size,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_draw_buffer = heap_caps_malloc(LVGL_DRAW_BUFFER_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_framebuffer_mutex == NULL || s_lvgl_ready == NULL ||
        s_framebuffer == NULL || s_draw_buffer == NULL) {
        ESP_LOGE(TAG, "Unable to allocate LVGL buffers in PSRAM");
        return false;
    }
    memset(s_framebuffer, 0, framebuffer_size);

    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);

    s_display = lv_display_create(LVGL_UVC_WIDTH, LVGL_UVC_HEIGHT);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "Unable to create LVGL display");
        return false;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL,
                           LVGL_DRAW_BUFFER_SIZE,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(s_display);

    if (xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 5, NULL, 1) !=
        pdPASS) {
        ESP_LOGE(TAG, "Unable to create LVGL task");
        return false;
    }

    if (xSemaphoreTake(s_lvgl_ready, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for the first Widgets Demo frame");
        return false;
    }

    ESP_LOGI(TAG, "Virtual display ready: %ux%u RGB565",
             LVGL_UVC_WIDTH, LVGL_UVC_HEIGHT);
    return true;
}

static inline uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static inline void rgb565_to_rgb888(uint16_t pixel, int *r, int *g, int *b)
{
    *r = ((pixel >> 11) & 0x1F) * 255 / 31;
    *g = ((pixel >> 5) & 0x3F) * 255 / 63;
    *b = (pixel & 0x1F) * 255 / 31;
}

bool lvgl_display_copy_yuyv(uint8_t *destination, size_t destination_size)
{
    static bool first_frame_logged;

    if (destination == NULL || destination_size < LVGL_UVC_YUYV_SIZE ||
        s_framebuffer == NULL || s_framebuffer_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_framebuffer_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    size_t output = 0;
    uint8_t min_luma = 255;
    uint8_t max_luma = 0;
    const size_t pixel_count = LVGL_UVC_WIDTH * LVGL_UVC_HEIGHT;
    for (size_t pixel = 0; pixel < pixel_count; pixel += 2) {
        int r0, g0, b0, r1, g1, b1;
        rgb565_to_rgb888(s_framebuffer[pixel], &r0, &g0, &b0);
        rgb565_to_rgb888(s_framebuffer[pixel + 1], &r1, &g1, &b1);

        const int y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16;
        const int y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
        const int r = (r0 + r1) / 2;
        const int g = (g0 + g1) / 2;
        const int b = (b0 + b1) / 2;
        const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

        const uint8_t y0_clamped = clamp_u8(y0);
        const uint8_t y1_clamped = clamp_u8(y1);
        if (y0_clamped < min_luma) {
            min_luma = y0_clamped;
        }
        if (y1_clamped < min_luma) {
            min_luma = y1_clamped;
        }
        if (y0_clamped > max_luma) {
            max_luma = y0_clamped;
        }
        if (y1_clamped > max_luma) {
            max_luma = y1_clamped;
        }

        destination[output++] = y0_clamped;
        destination[output++] = clamp_u8(u);
        destination[output++] = y1_clamped;
        destination[output++] = clamp_u8(v);
    }

    xSemaphoreGive(s_framebuffer_mutex);

    if (!first_frame_logged) {
        ESP_LOGI(TAG, "First UVC frame luma range: %u..%u",
                 min_luma, max_luma);
        first_frame_logged = true;
    }
    return true;
}
