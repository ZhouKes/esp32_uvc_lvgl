/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_lvgl.h"
#include "usbd_core.h"
#include "usbd_video.h"

#define UVC_BUS_ID              0
#define UVC_VIDEO_IN_EP         0x81
#define UVC_VIDEO_INT_EP        0x83
#define UVC_WIDTH               LVGL_UVC_WIDTH
#define UVC_HEIGHT              LVGL_UVC_HEIGHT
#define UVC_FPS                 5U
#define UVC_FRAME_INTERVAL      (10000000UL / UVC_FPS)
#define UVC_FRAME_SIZE          ((uint32_t)UVC_WIDTH * UVC_HEIGHT * 2U)
#define UVC_BIT_RATE            (UVC_FRAME_SIZE * 8U * UVC_FPS)

/* Keep one 512-byte transaction per 125 us microframe. This matches the
 * default P4 EP1 TX FIFO and avoids host/DWC2 interoperability problems seen
 * with high-bandwidth multi-transaction isochronous endpoints. */
#define UVC_HS_TRANSACTION_SIZE 512U
#define UVC_HS_TRANSACTION_MULT 1U
#define UVC_MAX_PAYLOAD_SIZE    \
    (UVC_HS_TRANSACTION_SIZE * UVC_HS_TRANSACTION_MULT)
#define UVC_VIDEO_PACKET_SIZE   \
    (UVC_HS_TRANSACTION_SIZE | ((UVC_HS_TRANSACTION_MULT - 1U) << 11))

#define UVC_VS_HEADER_SIZE \
    (VIDEO_SIZEOF_VS_INPUT_HEADER_DESC(1, 1) + \
     VIDEO_SIZEOF_VS_FORMAT_UNCOMPRESSED_DESC + \
     VIDEO_SIZEOF_VS_FRAME_UNCOMPRESSED_DESC(1))

#define UVC_CONFIG_DESC_SIZE \
    (9 + VIDEO_VC_NOEP_DESCRIPTOR_LEN + 9 + UVC_VS_HEADER_SIZE + 6 + 9 + 7)

#define USBD_VID               0xFFFF
#define USBD_PID               0xFFFF
#define USBD_MAX_POWER         100

static const char *TAG = "uvc_lvgl";

static volatile bool s_streaming;
static volatile bool s_transfer_busy;
static uint8_t *s_yuyv_frame;
static TaskHandle_t s_stream_task;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    s_packet_buffer[UVC_MAX_PAYLOAD_SIZE];

static const uint8_t s_device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01,
                               USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t s_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(UVC_CONFIG_DESC_SIZE, 0x02, 0x01,
                               USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    VIDEO_VC_NOEP_DESCRIPTOR_INIT(0x00, UVC_VIDEO_INT_EP, 0x0100,
                                  VIDEO_VC_TERMINAL_LEN, 48000000, 0x02),
    VIDEO_VS_DESCRIPTOR_INIT(0x01, 0x00, 0x00),
    VIDEO_VS_INPUT_HEADER_DESCRIPTOR_INIT(0x01, UVC_VS_HEADER_SIZE,
                                          UVC_VIDEO_IN_EP, 0x00),
    VIDEO_VS_FORMAT_UNCOMPRESSED_DESCRIPTOR_INIT(0x01, 0x01, VIDEO_GUID_YUY2),
    VIDEO_VS_FRAME_UNCOMPRESSED_DESCRIPTOR_INIT(
        0x01, UVC_WIDTH, UVC_HEIGHT, UVC_BIT_RATE, UVC_BIT_RATE,
        UVC_FRAME_SIZE, DBVAL(UVC_FRAME_INTERVAL), 0x01,
        DBVAL(UVC_FRAME_INTERVAL)),
    VIDEO_VS_COLOR_MATCHING_DESCRIPTOR_INIT(),
    VIDEO_VS_DESCRIPTOR_INIT(0x01, 0x01, 0x01),
    USB_ENDPOINT_DESCRIPTOR_INIT(UVC_VIDEO_IN_EP, 0x05,
                                 UVC_VIDEO_PACKET_SIZE, 0x01),
};

static const uint8_t s_device_qualifier_descriptor[] = {
    0x0A,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02,
    0x00, 0x00, 0x00,
    0x40,
    0x00, 0x00,
};

static const char *s_string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },
    "CherryUSB",
    "LVGL USB Camera 640x480",
    "2026092201",
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_config_descriptor;
}

static const uint8_t *device_qualifier_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return s_device_qualifier_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void)speed;
    if (index >= sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0])) {
        return NULL;
    }
    return s_string_descriptors[index];
}

static const struct usb_descriptor s_uvc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_qualifier_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

static void uvc_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        s_streaming = false;
        s_transfer_busy = false;
        break;
    case USBD_EVENT_CONFIGURED:
        s_transfer_busy = false;
        break;
    default:
        break;
    }
}

void usbd_video_open(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
    s_transfer_busy = false;
    s_streaming = true;
    ESP_EARLY_LOGI(TAG, "UVC stream opened");
}

void usbd_video_close(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
    s_streaming = false;
    s_transfer_busy = false;
    ESP_EARLY_LOGI(TAG, "UVC stream closed");
}

static void uvc_iso_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    static bool first_frame_sent;

    if (nbytes == 0) {
        return;
    }
    if (usbd_video_stream_split_transfer(busid, ep)) {
        s_transfer_busy = false;
        if (!first_frame_sent) {
            ESP_EARLY_LOGI(TAG, "First UVC frame sent");
            first_frame_sent = true;
        }
        if (s_stream_task != NULL) {
            BaseType_t task_woken = pdFALSE;
            vTaskNotifyGiveFromISR(s_stream_task, &task_woken);
            if (task_woken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

static struct usbd_endpoint s_video_in_ep = {
    .ep_addr = UVC_VIDEO_IN_EP,
    .ep_cb = uvc_iso_in_callback,
};

static struct usbd_interface s_video_control_intf;
static struct usbd_interface s_video_stream_intf;

static void uvc_init(void)
{
    usbd_desc_register(UVC_BUS_ID, &s_uvc_descriptor);
    usbd_add_interface(UVC_BUS_ID,
        usbd_video_init_intf(UVC_BUS_ID, &s_video_control_intf,
                             UVC_FRAME_INTERVAL, UVC_FRAME_SIZE,
                             UVC_MAX_PAYLOAD_SIZE));
    usbd_add_interface(UVC_BUS_ID,
        usbd_video_init_intf(UVC_BUS_ID, &s_video_stream_intf,
                             UVC_FRAME_INTERVAL, UVC_FRAME_SIZE,
                             UVC_MAX_PAYLOAD_SIZE));
    usbd_add_endpoint(UVC_BUS_ID, &s_video_in_ep);
    usbd_initialize(UVC_BUS_ID, ESP_USB_HS0_BASE, uvc_event_handler);
}

static void uvc_stream_task(void *arg)
{
    (void)arg;
    const int64_t frame_period_us = 1000000LL / UVC_FPS;

    s_stream_task = xTaskGetCurrentTaskHandle();
    memset(s_packet_buffer, 0, sizeof(s_packet_buffer));
    uvc_init();
    ESP_LOGI(TAG, "UVC ready: %ux%u YUY2 at %u FPS",
             UVC_WIDTH, UVC_HEIGHT, UVC_FPS);

    while (true) {
        if (!s_streaming) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const int64_t frame_start = esp_timer_get_time();
        if (!lvgl_display_copy_yuyv(s_yuyv_frame, UVC_FRAME_SIZE)) {
            ESP_LOGE(TAG, "Failed to capture the LVGL framebuffer");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Discard a notification left by a stream that was just closed. */
        (void)ulTaskNotifyTake(pdTRUE, 0);
        s_transfer_busy = true;
        if (usbd_video_stream_start_write(UVC_BUS_ID, UVC_VIDEO_IN_EP,
                                          s_packet_buffer, s_yuyv_frame,
                                          UVC_FRAME_SIZE, true) < 0) {
            s_transfer_busy = false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        while (s_transfer_busy && s_streaming) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }

        const int64_t remaining_us = frame_period_us -
                                     (esp_timer_get_time() - frame_start);
        if (remaining_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS((remaining_us + 999) / 1000));
        }
    }
}

void app_main(void)
{
    if (!lvgl_display_init()) {
        ESP_LOGE(TAG, "LVGL framebuffer initialization failed");
        return;
    }

    s_yuyv_frame = heap_caps_malloc(UVC_FRAME_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_yuyv_frame == NULL) {
        ESP_LOGE(TAG, "Unable to allocate %lu-byte UVC frame in PSRAM",
                 (unsigned long)UVC_FRAME_SIZE);
        return;
    }

    if (xTaskCreatePinnedToCore(uvc_stream_task, "uvc_stream", 4096, NULL,
                                10, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create UVC streaming task");
        heap_caps_free(s_yuyv_frame);
        s_yuyv_frame = NULL;
    }
}
