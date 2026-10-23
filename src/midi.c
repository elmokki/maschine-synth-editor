#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_helpers.h"
#include "midi.h"
#include "usb_recover.h"

static const char *TAG = "midi";

#define MIDI_IF 1
#define MIDI_CABLE 0         /* the Maschine's only external MIDI port */
#define MIDI_BUF_SIZE 64     /* one full-speed bulk packet = 16 events */
#define QUEUE_LEN 512        /* events, must be a power of two; the SR's patch dump is ~290 */
#define FS_BULK_MPS 64

static usb_host_client_handle_t client_hdl;
static usb_device_handle_t dev_hdl;
static uint8_t ep_in, ep_out;
static int in_mps;
/* Several IN transfers queued, so one is always waiting while another's
 * callback runs. (Single-byte messages sent back to back - no gaps at all -
 * still lose some before they reach us, in the Maschine or the sender;
 * realistic MIDI clock at 120 BPM passes complete.) */
#define IN_XFERS 4
static usb_transfer_t *xfer_in[IN_XFERS], *xfer_out;
static bool attached, out_busy;

/* The sequencer sends from its own task, so the OUT queue is guarded (recursive: out_kick is called with it held).
 * Created with the first attach; until then nothing is sent (attached is false). */
static SemaphoreHandle_t q_lock;
#define Q_LOCK() do { if (q_lock) xSemaphoreTakeRecursive(q_lock, portMAX_DELAY); } while (0)
#define Q_UNLOCK() do { if (q_lock) xSemaphoreGiveRecursive(q_lock); } while (0)

/* Pending USB-MIDI event packets (4 bytes each) waiting for xfer_out */
static uint8_t queue[QUEUE_LEN][4];
static unsigned q_head, q_tail;

/* USB-MIDI 1.0 spec table 4-1: payload bytes per Code Index Number */
static const uint8_t cin_len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

static void out_kick(void)
{
    Q_LOCK();
    if (!attached || out_busy || q_head == q_tail) {
        Q_UNLOCK();
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
    Q_UNLOCK();
}

static void out_cb(usb_transfer_t *xfer)
{
    Q_LOCK();
    out_busy = false;
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "OUT transfer status %d", xfer->status);
        if (attached && xfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
            xfer->status != USB_TRANSFER_STATUS_CANCELED) {
            usb_recover_endpoint(dev_hdl, ep_out, TAG);
        }
    }
    out_kick();
    Q_UNLOCK();
}

void midi_send(uint8_t status, uint8_t data1, uint8_t data2)
{
    if (!attached || status < 0x80 || status >= 0xF0) {
        return;
    }
    Q_LOCK();
    if (q_head - q_tail >= QUEUE_LEN) {
        ESP_LOGW(TAG, "OUT queue full, dropping 0x%02X", status);
        Q_UNLOCK();
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
    Q_UNLOCK();
}

void midi_send_realtime(uint8_t status)
{
    if (!attached || status < 0xF8) {
        return;
    }
    Q_LOCK();
    if (q_head - q_tail >= QUEUE_LEN) {
        Q_UNLOCK();
        return;
    }
    uint8_t *ev = queue[q_head % QUEUE_LEN];
    ev[0] = (MIDI_CABLE << 4) | 0xF;  /* CIN F = a single byte */
    ev[1] = status;
    ev[2] = 0;
    ev[3] = 0;
    q_head++;
    out_kick();
    Q_UNLOCK();
}

/* System Exclusive, F0 ... F7 inclusive, as USB-MIDI event packets:
 * CIN 4 = 3 bytes that start/continue it, CIN 5/6/7 = it ends with 1/2/3
 * bytes. The whole message is queued or dropped, never cut. */
bool midi_send_sysex(const uint8_t *data, size_t len)
{
    if (!attached || len < 2 || data[0] != 0xF0 || data[len - 1] != 0xF7) {
        return false;
    }
    size_t packets = (len + 2) / 3;
    Q_LOCK();
    if (QUEUE_LEN - (q_head - q_tail) < packets) {
        ESP_LOGW(TAG, "OUT queue full, dropping %u byte SysEx", (unsigned)len);
        Q_UNLOCK();
        return false;
    }
    for (size_t i = 0; i < len; i += 3) {
        size_t n = len - i < 3 ? len - i : 3;
        bool last = i + n == len;
        uint8_t *ev = queue[q_head % QUEUE_LEN];
        ev[0] = (MIDI_CABLE << 4) | (last ? 4 + n : 4);
        ev[1] = data[i];
        ev[2] = n > 1 ? data[i + 1] : 0;
        ev[3] = n > 2 ? data[i + 2] : 0;
        q_head++;
    }
    out_kick();
    Q_UNLOCK();
    return true;
}

/* A long System Exclusive message sent in parts (a bank dump is 30 KB: it can't be queued whole, and the DIN
 * cable takes ~3 KB/s, so the caller paces the parts). The first part starts with F0, the last ends with F7; the
 * other parts are multiples of 3 bytes. False if the queue has no room now (nothing is sent: try again). */
bool midi_send_sysex_part(const uint8_t *data, size_t len, bool last)
{
    if (!attached || len == 0 || (!last && len % 3)) {
        return false;
    }
    size_t packets = (len + 2) / 3;
    Q_LOCK();
    if (QUEUE_LEN - (q_head - q_tail) < packets) {
        Q_UNLOCK();
        return false;
    }
    for (size_t i = 0; i < len; i += 3) {
        size_t n = len - i < 3 ? len - i : 3;
        bool end = last && i + n == len;
        uint8_t *ev = queue[q_head % QUEUE_LEN];
        ev[0] = (MIDI_CABLE << 4) | (end ? 4 + n : 4);
        ev[1] = data[i];
        ev[2] = n > 1 ? data[i + 1] : 0;
        ev[3] = n > 2 ? data[i + 2] : 0;
        q_head++;
    }
    out_kick();
    Q_UNLOCK();
    return true;
}

/* ---- MIDI thru ---- */

#define THRU_SYSEX_MAX 1024      /* what goes back out (thru) */
#define SYSEX_IN_MAX 32768       /* what is collected for the handler: a bank dump is 29828 bytes */
static midi_thru_t thru_mode;
static int thru_channel;
static uint8_t thru_sysex_small[THRU_SYSEX_MAX];
static uint8_t *thru_sysex = thru_sysex_small;  /* in PSRAM when there is some */
static size_t thru_cap = THRU_SYSEX_MAX;
static size_t thru_sysex_len;
static bool thru_sysex_overflow;

void midi_set_thru(midi_thru_t mode, int channel)
{
    thru_mode = mode;
    thru_channel = channel & 0x0F;
}

/* One USB-MIDI event packet to the OUT queue as is */
static void queue_packet(const uint8_t *ev)
{
    Q_LOCK();
    if (q_head - q_tail < QUEUE_LEN) {
        memcpy(queue[q_head % QUEUE_LEN], ev, 4);
        queue[q_head % QUEUE_LEN][0] = (MIDI_CABLE << 4) | (ev[0] & 0x0F);
        q_head++;
    }
    Q_UNLOCK();
}

static midi_sysex_handler_t sysex_handler;
static midi_note_handler_t note_handler;

void midi_set_note_handler(midi_note_handler_t fn)
{
    note_handler = fn;
}

void midi_set_sysex_handler(midi_sysex_handler_t fn)
{
    sysex_handler = fn;
}

/* Incoming SysEx is collected whole: for the handler (e.g. a synth's dump
 * reply) and for thru */
static void sysex_in(const uint8_t *ev)
{
    static bool grown;
    if (!grown) {
        grown = true;  /* once: PSRAM if there is any */
        uint8_t *big = heap_caps_malloc(SYSEX_IN_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (big) {
            thru_sysex = big;
            thru_cap = SYSEX_IN_MAX;
        }
    }
    int cin = ev[0] & 0x0F;
    {
        int n = cin == 0x4 ? 3 : cin - 4;
        if (ev[1] == 0xF0) {
            thru_sysex_len = 0;
            thru_sysex_overflow = false;
        }
        for (int k = 0; k < n; k++) {
            if (thru_sysex_len < thru_cap) {
                thru_sysex[thru_sysex_len++] = ev[1 + k];
            } else {
                thru_sysex_overflow = true;
            }
        }
        if (cin != 0x4) {
            if (thru_sysex_overflow) {
                ESP_LOGW(TAG, "SysEx over %d bytes dropped", (int)thru_cap);
            } else {
                if (sysex_handler) {
                    sysex_handler(thru_sysex, thru_sysex_len);
                }
                if (thru_mode != MIDI_THRU_OFF && thru_sysex_len <= THRU_SYSEX_MAX) {
                    midi_send_sysex(thru_sysex, thru_sysex_len);
                }
            }
            thru_sysex_len = 0;
        }
    }
}

static void thru(const uint8_t *ev)
{
    int cin = ev[0] & 0x0F;
    uint8_t out[4] = {ev[0], ev[1], ev[2], ev[3]};
    if (cin >= 0x8 && cin <= 0xE && thru_mode == MIDI_THRU_TO_CHANNEL) {
        out[1] = (out[1] & 0xF0) | thru_channel;
    }
    queue_packet(out);
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
            bool taken = false;
            if ((cin == 0x8 || cin == 0x9) && note_handler) {
                taken = note_handler(d[i + 1] & 0x0F, d[i + 2] & 0x7F, cin == 0x8 ? 0 : d[i + 3] & 0x7F);
            }
            if (cin >= 0x4 && cin <= 0x7) {
                sysex_in(&d[i]);
            } else if (thru_mode != MIDI_THRU_OFF && !taken) {
                thru(&d[i]);
            }
            char hex[3 * 3 + 1] = "";
            for (int k = 0; k < len; k++) {
                sprintf(&hex[k * 3], "%02X ", d[i + 1 + k]);
            }
            ESP_LOGD(TAG, "DIN IN (cable %d): %s", d[i] >> 4, hex);
        }
        out_kick();
    } else if (xfer->status != USB_TRANSFER_STATUS_NO_DEVICE && xfer->status != USB_TRANSFER_STATUS_CANCELED) {
        /* (CANCELED is what a recovery's own flush does to the other queued
         * transfers: recovering again would cancel them again, for ever) */
        ESP_LOGW(TAG, "IN transfer status %d", xfer->status);
        if (attached) {
            usb_recover_endpoint(dev_hdl, ep_in, TAG);
        }
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
    if (!q_lock) {
        q_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (!xfer_out) {
        /* Allocated once and reused across reconnects */
        for (int k = 0; k < IN_XFERS; k++) {
            ESP_ERROR_CHECK(usb_host_transfer_alloc(MIDI_BUF_SIZE, 0, &xfer_in[k]));
            xfer_in[k]->callback = in_cb;
            xfer_in[k]->timeout_ms = 0;
        }
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
    Q_LOCK();
    attached = true;
    out_busy = false;
    q_head = q_tail = 0;
    Q_UNLOCK();

    for (int k = 0; k < IN_XFERS; k++) {
        xfer_in[k]->num_bytes = usb_round_up_to_mps(MIDI_BUF_SIZE, in_mps);
        xfer_in[k]->device_handle = dev_hdl;
        xfer_in[k]->bEndpointAddress = ep_in;
        ESP_ERROR_CHECK(usb_host_transfer_submit(xfer_in[k]));
    }

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
