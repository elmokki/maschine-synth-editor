/*
 * Step 1: just detect the Maschine Mk2 over USB host.
 *
 * Installs ESP-IDF's native USB Host Library (usb/usb_host.h - this is the
 * low-level host stack ESP-IDF ships for the chip's native USB OTG
 * peripheral; not the same API surface as TinyUSB's host driver, but it's
 * the simplest path for "enumerate what's plugged in"). On any new device
 * connecting, it opens the device, reads the device descriptor, and prints
 * VID/PID - flagging it clearly if it matches the Maschine Mk2
 * (VID 0x17CC, PID 0x1140, see docs/PROTOCOL.md).
 *
 * This deliberately does NOT touch the HID interface/reports yet - the
 * goal here is only to confirm the board's USB OTG + VBUS power situation
 * actually works and the device enumerates at all, before building any of
 * the protocol logic on top.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb/usb_host.h"

static const char *TAG = "maschine_detect";

#define MASCHINE_VID 0x17CC
#define MASCHINE_PID 0x1140

static usb_host_client_handle_t client_hdl;

static void handle_new_device(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl;
    esp_err_t err = usb_host_device_open(client_hdl, dev_addr, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *desc;
    err = usb_host_get_device_descriptor(dev_hdl, &desc);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New device at addr %d: VID=0x%04X PID=0x%04X",
                 dev_addr, desc->idVendor, desc->idProduct);
        if (desc->idVendor == MASCHINE_VID && desc->idProduct == MASCHINE_PID) {
            ESP_LOGI(TAG, ">>> This is the Maschine Mk2. Enumeration + VBUS power work. <<<");
        } else {
            ESP_LOGW(TAG, "(not the Maschine Mk2 - got VID/PID above instead)");
        }
    } else {
        ESP_LOGE(TAG, "usb_host_get_device_descriptor failed: %s", esp_err_to_name(err));
    }

    usb_host_device_close(client_hdl, dev_hdl);
}

static void client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        handle_new_device(event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "Device disconnected");
        break;
    default:
        break;
    }
}

/* Dedicated task that pumps the USB Host Library's own event queue
 * (USB_HOST_LIB_EVENT_FLAGS_* - "no clients", "all free", etc). Separate
 * from the client event task below per the standard ESP-IDF USB Host
 * Library usage pattern (see IDF's usb/host/usb_host_lib example). */
static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Installing USB Host Library...");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL);

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_callback,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client_hdl));

    ESP_LOGI(TAG, "Waiting for USB devices (plug in the Maschine Mk2 now if it isn't already)...");

    while (1) {
        usb_host_client_handle_events(client_hdl, portMAX_DELAY);
    }
}
