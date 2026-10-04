/*
 * Step 3: HID round-trip with the Maschine Mk2 over USB host, plus displays.
 *
 * On connect: finds the HID interface (IF2) in the config descriptor,
 * claims it, keeps an interrupt IN transfer running for pads (0x20), and
 * polls buttons/encoders (0x01) with GET_REPORT on EP0 (see poll_report01
 * for why). Logs button presses/releases, main encoder steps and pad
 * presses. Output: all pads are lit dim blue and a pressed pad turns white
 * (0x80), and the two displays (0xE0/0xE1, see display.c) show a test
 * synth-editor UI - one parameter per endless encoder, in a column right
 * under it, changed by turning that encoder.
 *
 * Report layouts are the ones confirmed on PC in docs/PROTOCOL.md; offsets
 * below are into the raw transfer buffer, where byte 0 is the report ID.
 *
 * Uses ESP-IDF's native USB Host Library (usb/usb_host.h). All USB
 * callbacks run in app_main's client-event loop, so no locking is needed
 * between them.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "display.h"
#include "fonts.h"

static const char *TAG = "maschine";

#define MASCHINE_VID 0x17CC
#define MASCHINE_PID 0x1140
#define MASCHINE_HID_IF 2

/* IN transfers are one packet each (see in_cb); reports longer than one
 * packet (0x20 is 65 bytes) are reassembled in report_buf. */
#define REPORT_BUF_SIZE 128
#define PAD_LED_REPORT_SIZE 49  /* 0x80 + 16 pads x RGB */

/* Pad thresholds on the raw 0-4095 value. Cross-talk from a firmly pressed
 * neighbour reaches ~750 (see PROTOCOL.md), so press well above that, with
 * hysteresis so a pad near the threshold doesn't chatter. */
#define PAD_PRESS_RAW 1000
#define PAD_RELEASE_RAW 300

static const char *const button_names[48] = {
    "Display 1", "Display 2", "Display 3", "Display 4",
    "Display 5", "Display 6", "Display 7", "Display 8",
    "Control", "Step", "Browse", "Sampling",
    "Arrow-Left", "Arrow-Right", "All", "Auto",
    "Volume", "Swing", "Tempo", "Arrow-Left #2",
    "Arrow-Right #2", "Enter", "Note Repeat", NULL,
    "Group A", "Group B", "Group C", "Group D",
    "Group E", "Group F", "Group G", "Group H",
    "Restart", "Step-Left", "Step-Right", "Grid",
    "Play", "Rec", "Erase", "Shift",
    "Scene", "Pattern", "Pad Mode", "Navigate",
    "Duplicate", "Select", "Solo", "Mute",
};

static usb_host_client_handle_t client_hdl;
static usb_device_handle_t dev_hdl;
static uint8_t ep_in, ep_out;
static int in_mps;
static usb_transfer_t *xfer_in, *xfer_out, *xfer_ctrl;
static bool ctrl_busy;
static int64_t ctrl_last_us;
static uint32_t ctrl_fail;
static bool connected;
static bool out_busy, pad_leds_dirty;

/* Input state, for change detection */
static bool have_report01;
static uint8_t prev_buttons[6];
static uint8_t prev_main_enc;
static uint16_t prev_enc[8];
static bool pad_down[16];

static uint8_t pad_leds[PAD_LED_REPORT_SIZE];

static uint8_t report_buf[REPORT_BUF_SIZE];
static int report_len;

static void set_pad_led(int pad, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = &pad_leds[1 + pad * 3];
    p[0] = r & 0x7F;
    p[1] = g & 0x7F;
    p[2] = b & 0x7F;
}

/* One interrupt OUT transfer in flight at a time, shared by pad LEDs and
 * display chunks. Pad LEDs go first so they stay responsive while a
 * display redraw is still streaming out. */
static void out_kick(void)
{
    if (!connected || out_busy) {
        return;
    }
    uint8_t *buf = xfer_out->data_buffer;
    if (pad_leds_dirty) {
        memcpy(buf, pad_leds, PAD_LED_REPORT_SIZE);
        xfer_out->num_bytes = PAD_LED_REPORT_SIZE;
        pad_leds_dirty = false;
    } else if (display_next_packet(buf)) {
        xfer_out->num_bytes = DISPLAY_PACKET_SIZE;
    } else {
        return;
    }
    xfer_out->device_handle = dev_hdl;
    xfer_out->bEndpointAddress = ep_out;
    out_busy = true;
    esp_err_t err = usb_host_transfer_submit(xfer_out);
    if (err != ESP_OK) {
        out_busy = false;
        ESP_LOGE(TAG, "OUT transfer submit failed: %s", esp_err_to_name(err));
    }
}

static void send_pad_leds(void)
{
    pad_leds_dirty = true;
    out_kick();
}

static void out_cb(usb_transfer_t *xfer)
{
    out_busy = false;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "OUT transfer status %d (report 0x%02X)",
                 xfer->status, xfer->data_buffer[0]);
    }
    out_kick();
}

/* Test UI: 8 parameters, one per endless encoder, shown in four 64 px
 * columns per display so each sits right under its encoder. */
#define PARAM_COUNT 8
#define ENCODER_UNITS_PER_STEP 10  /* ~1300 units per turn -> ~130 steps */
#define COLUMN_WIDTH 64

static const char *const param_names[PARAM_COUNT] = {
    "CUTOFF", "RESO", "ENV AMT", "DRIVE",
    "ATTACK", "DECAY", "SUSTAIN", "RELEASE",
};
static int param_values[PARAM_COUNT] = {64, 20, 80, 0, 5, 40, 100, 30};
static int encoder_acc[PARAM_COUNT];

static void draw_param(int i)
{
    int disp = i / 4;
    int x = (i % 4) * COLUMN_WIDTH;
    /* The last column on each display loses x=255 to the panel crop, so
     * keep every column's content inside a 63 px wide box. */
    int w = COLUMN_WIDTH - 1;
    char val[8];
    snprintf(val, sizeof(val), "%d", param_values[i]);

    display_fill_rect(disp, x, 0, COLUMN_WIDTH, DISPLAY_HEIGHT, false);
    display_text(disp, x + (w - display_text_width(&font_small, param_names[i])) / 2, 2,
                 &font_small, param_names[i], true);
    display_text(disp, x + (w - display_text_width(&font_large, val)) / 2, 20,
                 &font_large, val, true);
    display_rect(disp, x + 4, 50, w - 8, 10, true);
    int fill = (w - 12) * param_values[i] / 127;
    display_fill_rect(disp, x + 6, 52, fill, 6, true);
}

static void draw_all(void)
{
    for (int d = 0; d < DISPLAY_COUNT; d++) {
        display_clear(d);
    }
    for (int i = 0; i < PARAM_COUNT; i++) {
        draw_param(i);
    }
}

static void param_turn(int i, int delta)
{
    encoder_acc[i] += delta;
    int steps = encoder_acc[i] / ENCODER_UNITS_PER_STEP;
    if (steps == 0) {
        return;
    }
    encoder_acc[i] -= steps * ENCODER_UNITS_PER_STEP;
    int v = param_values[i] + steps;
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    if (v != param_values[i]) {
        param_values[i] = v;
        draw_param(i);
        out_kick();
    }
}

/* 8 endless encoders: free-running 0-999 counter, wraps both ways */
static int encoder_delta(uint16_t cur, uint16_t prev)
{
    int d = (int)cur - (int)prev;
    if (d > 500) {
        d -= 1000;
    } else if (d < -500) {
        d += 1000;
    }
    return d;
}

static void handle_report01(const uint8_t *buf, int len)
{
    if (len < 25) {
        return;
    }
    const uint8_t *buttons = &buf[1];
    uint8_t main_enc = buf[8] & 0x0F;
    uint16_t enc[8];
    for (int i = 0; i < 8; i++) {
        enc[i] = buf[9 + i * 2] | (buf[10 + i * 2] << 8);
    }

    if (have_report01) {
        for (int bit = 0; bit < 48; bit++) {
            bool now = buttons[bit / 8] & (1 << (bit % 8));
            bool was = prev_buttons[bit / 8] & (1 << (bit % 8));
            if (now != was) {
                const char *name = button_names[bit] ? button_names[bit] : "?";
                ESP_LOGI(TAG, "button %-14s %s (bit %d)", name, now ? "down" : "up", bit);
            }
        }
        if (main_enc != prev_main_enc) {
            /* 4-bit wrapping counter, one step per detent */
            int d = ((main_enc - prev_main_enc + 8) & 0x0F) - 8;
            ESP_LOGI(TAG, "main encoder %+d (raw %d)", d, main_enc);
        }
        for (int i = 0; i < 8; i++) {
            if (enc[i] != prev_enc[i]) {
                param_turn(i, encoder_delta(enc[i], prev_enc[i]));
            }
        }
    }

    memcpy(prev_buttons, buttons, sizeof(prev_buttons));
    prev_main_enc = main_enc;
    memcpy(prev_enc, enc, sizeof(prev_enc));
    have_report01 = true;
}

static void handle_report20(const uint8_t *buf, int len)
{
    /* Pad identity is in the data (high nibble), each pad shows up in two
     * slots; take the max of the two. */
    int raw[16] = {0};
    for (int i = 1; i + 1 < len; i += 2) {
        uint8_t l = buf[i], h = buf[i + 1];
        int pad = (h & 0xF0) >> 4;
        int v = ((h & 0x0F) << 8) | l;
        if (v > raw[pad]) {
            raw[pad] = v;
        }
    }

    bool leds_changed = false;
    for (int pad = 0; pad < 16; pad++) {
        if (!pad_down[pad] && raw[pad] >= PAD_PRESS_RAW) {
            pad_down[pad] = true;
            set_pad_led(pad, 127, 127, 127);
            leds_changed = true;
            ESP_LOGI(TAG, "pad %2d down (raw %d)", pad, raw[pad]);
        } else if (pad_down[pad] && raw[pad] <= PAD_RELEASE_RAW) {
            pad_down[pad] = false;
            set_pad_led(pad, 0, 0, 20);
            leds_changed = true;
            ESP_LOGI(TAG, "pad %2d up", pad);
        }
    }
    if (leds_changed) {
        send_pad_leds();
    }
}

/* Buttons/encoders (0x01) are fetched with HID GET_REPORT on EP0, not
 * read from the interrupt pipe: the ESP32-S3 is a full-speed host, so the
 * interrupt IN endpoint is polled once per 1 ms frame, and the Maschine's
 * 500 Hz pad stream (2 packets per 0x20 report) fills every one of those
 * polls - no 0x01 report ever arrives there. The control pipe doesn't
 * compete with it. Polled from in_cb, which the pad stream keeps running
 * at ~1 kHz, so this ends up at ~170 Hz. */
#define CTRL_POLL_US 5000
#define REPORT01_LEN 25

static void ctrl_cb(usb_transfer_t *xfer)
{
    ctrl_busy = false;
    int n = xfer->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED || n <= 0) {
        if (ctrl_fail++ < 5) {
            ESP_LOGW(TAG, "GET_REPORT 0x01 failed: status %d, %d bytes", xfer->status, n);
        }
        return;
    }
    const uint8_t *d = xfer->data_buffer + sizeof(usb_setup_packet_t);
    if (d[0] == 0x01) {
        handle_report01(d, n);
    }
}

static void poll_report01(void)
{
    int64_t now = esp_timer_get_time();
    if (!connected || ctrl_busy || now - ctrl_last_us < CTRL_POLL_US) {
        return;
    }
    ctrl_last_us = now;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer_ctrl->data_buffer;
    setup->bmRequestType = 0xA1;  /* device-to-host, class, interface */
    setup->bRequest = 0x01;  /* GET_REPORT */
    setup->wValue = (0x01 << 8) | 0x01;  /* report type Input, report ID 0x01 */
    setup->wIndex = MASCHINE_HID_IF;
    setup->wLength = REPORT01_LEN;
    xfer_ctrl->num_bytes = sizeof(usb_setup_packet_t) + REPORT01_LEN;
    xfer_ctrl->device_handle = dev_hdl;
    xfer_ctrl->bEndpointAddress = 0;
    ctrl_busy = true;
    esp_err_t err = usb_host_transfer_submit_control(client_hdl, xfer_ctrl);
    if (err != ESP_OK) {
        ctrl_busy = false;
        if (ctrl_fail++ < 5) {
            ESP_LOGE(TAG, "GET_REPORT submit failed: %s", esp_err_to_name(err));
        }
    }
}

static int expected_report_len(uint8_t report_id)
{
    switch (report_id) {
    case 0x01: return 25;
    case 0x20: return 65;
    default: return 0;
    }
}

static void dispatch_report(const uint8_t *buf, int len)
{
    switch (buf[0]) {
    case 0x01:
        handle_report01(buf, len);
        break;
    case 0x20:
        handle_report20(buf, len);
        break;
    default:
        ESP_LOGD(TAG, "report 0x%02X, %d bytes", buf[0], len);
        break;
    }
}

/* Each IN transfer is exactly one packet: ESP-IDF's HCD asserts
 * (hcd_dwc.c _buffer_parse_intr) when a multi-packet interrupt IN transfer
 * is cut short by a short packet, which is how every 0x20 report ends.
 * A report is complete on a short packet, or once it reaches its known
 * length. */
static void in_cb(usb_transfer_t *xfer)
{
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
            ESP_LOGW(TAG, "IN transfer status %d", xfer->status);
        }
        report_len = 0;
    } else if (xfer->actual_num_bytes > 0) {
        int n = xfer->actual_num_bytes;
        if (report_len + n > REPORT_BUF_SIZE) {
            ESP_LOGW(TAG, "report overflow, dropping (id 0x%02X)", report_buf[0]);
            report_len = 0;
        } else {
            memcpy(&report_buf[report_len], xfer->data_buffer, n);
            report_len += n;
            int expected = expected_report_len(report_buf[0]);
            if (n < in_mps || (expected && report_len >= expected)) {
                dispatch_report(report_buf, report_len);
                report_len = 0;
            }
        }
    }

    poll_report01();

    if (connected) {
        esp_err_t err = usb_host_transfer_submit(xfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IN resubmit failed: %s", esp_err_to_name(err));
        }
    }
}

static bool find_hid_endpoints(const usb_config_desc_t *cfg)
{
    int offset = 0;
    const usb_intf_desc_t *intf =
        usb_parse_interface_descriptor(cfg, MASCHINE_HID_IF, 0, &offset);
    if (!intf) {
        ESP_LOGE(TAG, "HID interface %d not found", MASCHINE_HID_IF);
        return false;
    }

    ep_in = ep_out = 0;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        int ep_offset = offset;
        const usb_ep_desc_t *ep =
            usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_offset);
        if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_INTR) {
            continue;
        }
        if (USB_EP_DESC_GET_EP_DIR(ep)) {
            ep_in = ep->bEndpointAddress;
            in_mps = USB_EP_DESC_GET_MPS(ep);
        } else {
            ep_out = ep->bEndpointAddress;
        }
        ESP_LOGI(TAG, "IF%d EP 0x%02X interrupt, MPS %d, interval %d",
                 MASCHINE_HID_IF, ep->bEndpointAddress, USB_EP_DESC_GET_MPS(ep), ep->bInterval);
    }
    return ep_in && ep_out;
}

static void handle_new_device(uint8_t dev_addr)
{
    if (connected) {
        return;  /* one Maschine at a time */
    }

    usb_device_handle_t hdl;
    esp_err_t err = usb_host_device_open(client_hdl, dev_addr, &hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }

    const usb_device_desc_t *desc;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(hdl, &desc));
    ESP_LOGI(TAG, "New device at addr %d: VID=0x%04X PID=0x%04X",
             dev_addr, desc->idVendor, desc->idProduct);
    if (desc->idVendor != MASCHINE_VID || desc->idProduct != MASCHINE_PID) {
        ESP_LOGW(TAG, "not the Maschine Mk2, ignoring");
        usb_host_device_close(client_hdl, hdl);
        return;
    }

    const usb_config_desc_t *cfg;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(hdl, &cfg));
    if (!find_hid_endpoints(cfg)) {
        usb_host_device_close(client_hdl, hdl);
        return;
    }

    err = usb_host_interface_claim(client_hdl, hdl, MASCHINE_HID_IF, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interface claim failed: %s", esp_err_to_name(err));
        usb_host_device_close(client_hdl, hdl);
        return;
    }

    dev_hdl = hdl;
    connected = true;
    have_report01 = false;
    memset(pad_down, 0, sizeof(pad_down));
    out_busy = false;
    report_len = 0;
    ctrl_busy = false;

    xfer_in->num_bytes = in_mps;
    xfer_in->device_handle = dev_hdl;
    xfer_in->bEndpointAddress = ep_in;
    ESP_ERROR_CHECK(usb_host_transfer_submit(xfer_in));

    for (int pad = 0; pad < 16; pad++) {
        set_pad_led(pad, 0, 0, 20);
    }
    display_invalidate_all();  /* redraw everything on (re)connect */
    send_pad_leds();

    ESP_LOGI(TAG, ">>> Maschine Mk2 HID interface claimed, listening. <<<");
}

static void handle_device_gone(usb_device_handle_t gone_hdl)
{
    if (!connected || gone_hdl != dev_hdl) {
        return;
    }
    ESP_LOGI(TAG, "Maschine disconnected");
    connected = false;
    usb_host_endpoint_halt(dev_hdl, ep_in);
    usb_host_endpoint_flush(dev_hdl, ep_in);
    usb_host_endpoint_halt(dev_hdl, ep_out);
    usb_host_endpoint_flush(dev_hdl, ep_out);
    usb_host_interface_release(client_hdl, dev_hdl, MASCHINE_HID_IF);
    usb_host_device_close(client_hdl, dev_hdl);
    dev_hdl = NULL;
}

static void client_event_callback(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        handle_new_device(event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        handle_device_gone(event_msg->dev_gone.dev_hdl);
        break;
    default:
        break;
    }
}

/* Dedicated task that pumps the USB Host Library's own event queue,
 * separate from the client event loop (standard ESP-IDF pattern, see IDF's
 * usb/host/usb_host_lib example). */
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

    /* Allocated once and reused across reconnects */
    ESP_ERROR_CHECK(usb_host_transfer_alloc(REPORT_BUF_SIZE, 0, &xfer_in));
    xfer_in->callback = in_cb;
    xfer_in->timeout_ms = 0;
    ESP_ERROR_CHECK(usb_host_transfer_alloc(DISPLAY_PACKET_SIZE, 0, &xfer_out));
    xfer_out->callback = out_cb;
    xfer_out->timeout_ms = 1000;
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + REPORT01_LEN, 0, &xfer_ctrl));
    xfer_ctrl->callback = ctrl_cb;
    xfer_ctrl->timeout_ms = 1000;
    pad_leds[0] = 0x80;
    draw_all();

    ESP_LOGI(TAG, "Waiting for the Maschine Mk2...");

    while (1) {
        usb_host_client_handle_events(client_hdl, portMAX_DELAY);
    }
}
