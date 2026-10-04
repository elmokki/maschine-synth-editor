#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "usb/usb_helpers.h"
#include "midi.h"

static const char *TAG = "midi";

#define MIDI_IF 1
#define MIDI_CABLE 0         /* the Maschine's only external MIDI port */
#define MIDI_BUF_SIZE 64     /* one full-speed bulk packet = 16 events */
#define QUEUE_LEN 64         /* events, must be a power of two */
#define FS_BULK_MPS 64

static usb_host_client_handle_t client_hdl;
static usb_device_handle_t dev_hdl;
static uint8_t ep_in, ep_out;
static int in_mps;
static usb_transfer_t *xfer_in, *xfer_out;
static bool attached, out_busy;

/* Pending USB-MIDI event packets (4 bytes each) waiting for xfer_out */
static uint8_t queue[QUEUE_LEN][4];
static unsigned q_head, q_tail;

/* USB-MIDI 1.0 spec table 4-1: payload bytes per Code Index Number */
static const uint8_t cin_len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

static void out_kick(void)
{
    if (!attached || out_busy || q_head == q_tail) {
        return;
    }
    int n = 0;
    while (q_tail != q_head && n < MIDI_BUF_SIZE) {
        memcpy(&xfer_out->data_buffer[n], queue[q_tail % QUEUE_LEN], 4);
        q_tail++;
        n += 4;
    }
    xfer_out->num_bytes = n;
    xfer_out->device_handle = dev_hdl;
    xfer_out->bEndpointAddress = ep_out;
    out_busy = true;
    esp_err_t err = usb_host_transfer_submit(xfer_out);
    if (err != ESP_OK) {
        out_busy = false;
        ESP_LOGE(TAG, "OUT submit failed: %s", esp_err_to_name(err));
    }
}

static void out_cb(usb_transfer_t *xfer)
{
    out_busy = false;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "OUT transfer status %d", xfer->status);
    }
    out_kick();
}

void midi_send(uint8_t status, uint8_t data1, uint8_t data2)
{
    if (!attached || status < 0x80 || status >= 0xF0) {
        return;
    }
    if (q_head - q_tail >= QUEUE_LEN) {
        ESP_LOGW(TAG, "OUT queue full, dropping 0x%02X", status);
        return;
    }
    uint8_t *ev = queue[q_head % QUEUE_LEN];
    ev[0] = (MIDI_CABLE << 4) | (status >> 4);  /* CIN = status high nibble */
    ev[1] = status;
    ev[2] = data1 & 0x7F;
    ev[3] = data2 & 0x7F;
    if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
        ev[3] = 0;  /* 2-byte messages */
    }
    q_head++;
    out_kick();
}

static void in_cb(usb_transfer_t *xfer)
{
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        const uint8_t *d = xfer->data_buffer;
        for (int i = 0; i + 4 <= xfer->actual_num_bytes; i += 4) {
            int cin = d[i] & 0x0F;
            int len = cin_len[cin];
            if (len == 0) {
                continue;  /* padding / reserved */
            }
            char hex[3 * 3 + 1] = "";
            for (int k = 0; k < len; k++) {
                sprintf(&hex[k * 3], "%02X ", d[i + 1 + k]);
            }
            ESP_LOGI(TAG, "DIN IN (cable %d): %s", d[i] >> 4, hex);
        }
    } else if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "IN transfer status %d", xfer->status);
    }

    if (attached) {
        esp_err_t err = usb_host_transfer_submit(xfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IN resubmit failed: %s", esp_err_to_name(err));
        }
    }
}

/* The Maschine reports its bulk endpoints with wMaxPacketSize 512 even when
 * enumerated at full speed, where bulk endpoints can only be 64 (it serves
 * its high-speed descriptors at both speeds). ESP-IDF refuses to allocate
 * that (limit 408), so the interface claim fails. The ESP32-S3 is full-speed
 * only, so patch the cached descriptor down to 64 before claiming. Safe in
 * practice: USB-MIDI events are 4 bytes, and a full-speed device can't send
 * bulk packets larger than 64 anyway. */
static void fix_fs_bulk_mps(const usb_ep_desc_t *ep)
{
    if (USB_EP_DESC_GET_MPS(ep) > FS_BULK_MPS) {
        ESP_LOGW(TAG, "EP 0x%02X reports bulk MPS %d at full speed, using %d",
                 ep->bEndpointAddress, USB_EP_DESC_GET_MPS(ep), FS_BULK_MPS);
        ((usb_ep_desc_t *)ep)->wMaxPacketSize = FS_BULK_MPS;
    }
}

static bool find_endpoints(const usb_config_desc_t *cfg, bool full_speed)
{
    int offset = 0;
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cfg, MIDI_IF, 0, &offset);
    if (!intf) {
        ESP_LOGE(TAG, "MIDI interface %d not found", MIDI_IF);
        return false;
    }
    ep_in = ep_out = 0;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
        int ep_offset = offset;
        const usb_ep_desc_t *ep =
            usb_parse_endpoint_descriptor_by_index(intf, i, cfg->wTotalLength, &ep_offset);
        if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) {
            continue;
        }
        if (full_speed) {
            fix_fs_bulk_mps(ep);
        }
        if (USB_EP_DESC_GET_EP_DIR(ep)) {
            ep_in = ep->bEndpointAddress;
            in_mps = USB_EP_DESC_GET_MPS(ep);
        } else {
            ep_out = ep->bEndpointAddress;
        }
        ESP_LOGI(TAG, "IF%d EP 0x%02X bulk, MPS %d",
                 MIDI_IF, ep->bEndpointAddress, USB_EP_DESC_GET_MPS(ep));
    }
    return ep_in && ep_out;
}

bool midi_attach(usb_host_client_handle_t client, usb_device_handle_t dev,
                 const usb_config_desc_t *cfg)
{
    if (!xfer_in) {
        /* Allocated once and reused across reconnects */
        ESP_ERROR_CHECK(usb_host_transfer_alloc(MIDI_BUF_SIZE, 0, &xfer_in));
        xfer_in->callback = in_cb;
        xfer_in->timeout_ms = 0;
        ESP_ERROR_CHECK(usb_host_transfer_alloc(MIDI_BUF_SIZE, 0, &xfer_out));
        xfer_out->callback = out_cb;
        xfer_out->timeout_ms = 1000;
    }

    usb_device_info_t info;
    ESP_ERROR_CHECK(usb_host_device_info(dev, &info));
    if (!find_endpoints(cfg, info.speed == USB_SPEED_FULL)) {
        return false;
    }
    esp_err_t err = usb_host_interface_claim(client, dev, MIDI_IF, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interface claim failed: %s", esp_err_to_name(err));
        return false;
    }

    client_hdl = client;
    dev_hdl = dev;
    attached = true;
    out_busy = false;
    q_head = q_tail = 0;

    xfer_in->num_bytes = usb_round_up_to_mps(MIDI_BUF_SIZE, in_mps);
    xfer_in->device_handle = dev_hdl;
    xfer_in->bEndpointAddress = ep_in;
    ESP_ERROR_CHECK(usb_host_transfer_submit(xfer_in));

    ESP_LOGI(TAG, "USB-MIDI interface claimed (DIN IN/OUT)");
    return true;
}

void midi_detach(void)
{
    if (!attached) {
        return;
    }
    attached = false;
    usb_host_endpoint_halt(dev_hdl, ep_in);
    usb_host_endpoint_flush(dev_hdl, ep_in);
    usb_host_endpoint_halt(dev_hdl, ep_out);
    usb_host_endpoint_flush(dev_hdl, ep_out);
    usb_host_interface_release(client_hdl, dev_hdl, MIDI_IF);
    dev_hdl = NULL;
}
