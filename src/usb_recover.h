/*
 * Getting an endpoint going again after a transfer error.
 *
 * Seen on hardware: writing to flash (LittleFS) stalls the USB host
 * interrupt long enough that every pipe ends up with transfer errors, after
 * which a plain resubmit fails with ESP_ERR_INVALID_STATE. Halting, flushing
 * and clearing the endpoint puts it back into a usable state.
 */
#pragma once
#include "esp_log.h"
#include "usb/usb_host.h"

static inline void usb_recover_endpoint(usb_device_handle_t dev, uint8_t ep, const char *tag)
{
    esp_err_t err = usb_host_endpoint_halt(dev, ep);
    if (err == ESP_OK) {
        err = usb_host_endpoint_flush(dev, ep);
    }
    if (err == ESP_OK) {
        err = usb_host_endpoint_clear(dev, ep);
    }
    if (err != ESP_OK) {
        ESP_LOGE(tag, "recovering EP 0x%02X failed: %s", ep, esp_err_to_name(err));
    } else {
        ESP_LOGW(tag, "EP 0x%02X recovered after a transfer error", ep);
    }
}
