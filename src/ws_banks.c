#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"
#include "ws_banks.h"
#include "ws_seq.h"

static const char *TAG = "wsbanks";

const uint8_t ws_to_ws_bank[12] = {0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 2, 3};
const uint8_t ws_from_ws_bank[12] = {0, 1, 10, 11, 2, 3, 4, 5, 6, 7, 8, 9};
const uint8_t ws_to_ws_exp_bank[12] = {0, 1, 6, 4, 5, 7, 8, 9, 10, 11, 2, 3};
const uint8_t ws_from_ws_exp_bank[12] = {0, 1, 10, 11, 3, 4, 2, 5, 6, 7, 8, 9};
const char *const ws_bank_names[12] = {"RAM 1", "RAM 2", "RAM 3", "ROM 4", "ROM 5", "ROM 6",
                                       "ROM 7", "ROM 8", "ROM 9", "ROM 10", "ROM 11", "CARD"};
const char *const ws_bank_tags[12] = {"R1", "R2", "R3", "ROM4", "ROM5", "ROM6", "ROM7", "ROM8", "ROM9", "ROM10",
                                      "ROM11", "CD"};

const ws_kind_t ws_kind_patch = {
    .item = "patch",
    .dir = STORAGE_ROOT "/wsp",
    .fallback = "Patch",
    .backup = "bank",
    .request = 0x1C,
    .reply = 0x4C,
    .count = 35,
    .item_len = 426,
};
const ws_kind_t ws_kind_seq = {
    .item = "wave sequence",
    .dir = STORAGE_ROOT "/wsseq",
    .fallback = "Sequence",
    .backup = "seq",
    .request = 0x0C,
    .reply = 0x54,
    .count = 32,
    .item_len = 0,
    .dump_len = 6 + 2 * 8784 + 2,
    .extract = ws_seq_extract,
    .put = ws_seq_put,
};
const ws_kind_t ws_kind_perf = {
    .item = "performance",
    .dir = STORAGE_ROOT "/wsperf",
    .fallback = "Performance",
    .backup = "perf",
    .request = 0x1D,
    .reply = 0x4D,
    .count = 50,
    .item_len = 181,
};

#define BANK_HEADER 6
#define BANK_MAX_LEN (BANK_HEADER + 35 * 2 * 426 + 2)  /* the biggest: the patches */
#define ITEM_MAX 2600  /* a wave sequence of 255 steps, see ws_seq.c */
#define NAME_CHARS 16
#define BANK_TIMEOUT_US 30000000
#define STREAM_CHUNK 96      /* bytes a part, a multiple of 3 */
#define STREAM_BYTE_US 340   /* the DIN cable's 320 us a byte, a little slower */
#define LIB_MAX (12 * 50)

typedef enum { JOB_NONE, JOB_ASK, JOB_WAIT, JOB_SAVE, JOB_SEND, JOB_ACK } job_stage_t;

static const ws_kind_t *kind;  /* of the job running or last run */
static job_stage_t job_stage;
static bool job_is_store;
static int job_bank, job_tries, job_saved, job_channel, store_num, save_n, import_total;
static uint16_t import_mask;  /* banks still to read */
static int64_t job_deadline, stream_next;
static size_t stream_pos;
static uint8_t *bank_buf;  /* in PSRAM */
static bool bank_in;       /* a good bank reply is in bank_buf */
static int bank_in_idx;
static uint8_t store_raw[ITEM_MAX];
static bool last_ok;       /* the last job ended with the unit's OK */
static uint8_t sr_code;    /* the SR's last status message: 21 write complete, 22 error, 23 data load completed, 24 error */

static size_t bank_len(const ws_kind_t *k) { return k->dump_len ? (size_t)k->dump_len : BANK_HEADER + k->count * k->item_len * 2 + 2; }

static bool bank_alloc(void)
{
    if (!bank_buf) {
        bank_buf = heap_caps_malloc(BANK_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bank_buf) {
            bank_buf = malloc(BANK_MAX_LEN);
        }
    }
    return bank_buf != NULL;
}

/* ---- the library's names of imported items (for the slot shown when storing) ---- */

static char lib_names[LIB_MAX][NAME_CHARS + 1];
static const ws_kind_t *lib_kind;
static bool lib_scanned;

const char *ws_banks_lib_name(const ws_kind_t *k, int bank, int num)
{
    if (!lib_scanned || lib_kind != k) {
        lib_scanned = true;
        lib_kind = k;
        memset(lib_names, 0, sizeof(lib_names));
        DIR *dir = opendir(k->dir);
        struct dirent *e;
        while (dir && (e = readdir(dir))) {
            int n = atoi(e->d_name);
            const char *sp = strchr(e->d_name, ' ');
            if (n >= 1 && n <= 12 * k->count && sp) {
                snprintf(lib_names[n - 1], sizeof(lib_names[0]), "%s", sp + 1);
            }
        }
        if (dir) {
            closedir(dir);
        }
    }
    int n = bank * k->count + num;
    return n >= 0 && n < LIB_MAX ? lib_names[n] : "";
}

/* ---- the job ---- */

static void job_finish(const char *text)
{
    job_stage = JOB_NONE;
    lib_scanned = false;
    synth_patches_changed();
    synth_notice(text);
    ESP_LOGI(TAG, "bank job: %s", text);
}

static void request_bank(int bank)
{
    uint8_t msg[7] = {0xF0, 0x42, 0x30 + job_channel, 0x28, kind->request, ws_to_ws_bank[bank], 0xF7};
    midi_send_sysex(msg, sizeof(msg));
}

/* The earlier import of a bank out of the library (buffers are static: the main task's stack is small) */
static void delete_bank_files(int bank)
{
    static char path[128];
    mkdir(kind->dir, 0777);
    DIR *dir = opendir(kind->dir);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        if (n > bank * kind->count && n <= (bank + 1) * kind->count) {
            snprintf(path, sizeof(path), "%s/%.100s", kind->dir, e->d_name);
            remove(path);
        }
    }
    if (dir) {
        closedir(dir);
    }
}

/* Item n of the bank in bank_buf into the library, one a tick so the editor stays alive; 1 if saved */
static int save_item_to_library(int bank, int n)
{
    static uint8_t raw[ITEM_MAX];
    static char name[NAME_CHARS + 1], path[128];
    int raw_len = kind->item_len;
    if (kind->extract) {
        raw_len = kind->extract(bank_buf + BANK_HEADER, n, raw);
        if (raw_len <= 0) {
            return 0;  /* an empty slot */
        }
    } else {
        const uint8_t *nib = bank_buf + BANK_HEADER + n * 2 * kind->item_len;
        for (int i = 0; i < raw_len; i++) {
            raw[i] = nib[2 * i] | nib[2 * i + 1] << 4;
        }
    }
    memcpy(name, raw, NAME_CHARS);
    name[NAME_CHARS] = 0;
    for (int i = 0; i < NAME_CHARS; i++) {
        if (name[i] < 32 || name[i] > 126 || name[i] == '/') {
            name[i] = name[i] == '/' ? '_' : ' ';
        }
    }
    for (int i = NAME_CHARS - 1; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    snprintf(path, sizeof(path), "%s/%03d %s", kind->dir, bank * kind->count + n + 1, name[0] ? name : kind->fallback);
    FILE *f = fopen(path, "wb");
    int ok = 0;
    if (f) {
        ok = fwrite(raw, 1, raw_len, f) == (size_t)raw_len;
        fclose(f);
    }
    return ok;
}

/* The item into the bank in bank_buf, the bank's checksum redone; the bank as it was kept first */
static const char *prepare_store(void)
{
    mkdir(STORAGE_ROOT "/ws", 0777);
    mkdir(STORAGE_ROOT "/ws/backup", 0777);
    char path[64];
    snprintf(path, sizeof(path), STORAGE_ROOT "/ws/backup/%s%02d", kind->backup, job_bank);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(bank_buf, 1, bank_len(kind), f);
        fclose(f);
    }
    if (kind->put) {
        const char *why = kind->put(bank_buf + BANK_HEADER, store_num, store_raw);
        if (why) {
            return why;
        }
    } else {
        uint8_t *dst = bank_buf + BANK_HEADER + store_num * 2 * kind->item_len;
        for (int i = 0; i < kind->item_len; i++) {
            dst[2 * i] = store_raw[i] & 0x0F;
            dst[2 * i + 1] = store_raw[i] >> 4;
        }
    }
    size_t len = bank_len(kind);
    int sum = 0;
    for (size_t i = BANK_HEADER; i < len - 2; i++) {
        sum += bank_buf[i];
    }
    bank_buf[2] = 0x30 + job_channel;
    bank_buf[len - 2] = sum & 0x7F;
    bank_buf[len - 1] = 0xF7;
    return NULL;
}

bool ws_banks_busy(void) { return job_stage != JOB_NONE; }
bool ws_banks_last_ok(void) { return last_ok; }

void ws_banks_import(const ws_kind_t *k, uint16_t mask, int channel)
{
    if (job_stage != JOB_NONE || !mask) {
        return;
    }
    kind = k;
    job_channel = channel & 0x0F;
    import_mask = mask & 0x0FFF;
    job_is_store = false;
    job_bank = __builtin_ctz(import_mask);
    import_total = __builtin_popcount(import_mask);
    job_tries = job_saved = 0;
    job_stage = JOB_ASK;
}

void ws_banks_store(const ws_kind_t *k, int bank, int num, const uint8_t *raw, int channel)
{
    if (job_stage != JOB_NONE || !(k->item_len || k->put)) {
        return;
    }
    kind = k;
    job_channel = channel & 0x0F;
    memcpy(store_raw, raw, k->item_len ? k->item_len : WSSEQ_MAX_LEN);
    job_bank = bank;
    store_num = num;
    job_is_store = true;
    last_ok = false;
    job_tries = 0;
    job_stage = JOB_ASK;
}

void ws_banks_tick(int64_t now)
{
    char buf[48];
    switch (job_stage) {
    case JOB_ASK:
        bank_in = false;
        ESP_LOGI(TAG, "asking for bank %d", job_bank);
        request_bank(job_bank);
        job_stage = JOB_WAIT;
        job_deadline = now + BANK_TIMEOUT_US;
        snprintf(buf, sizeof(buf), "%s %s...", job_is_store ? "READING" : "IMPORT", ws_bank_names[job_bank]);
        synth_notice(buf);
        break;
    case JOB_WAIT:
        if (bank_in && bank_in_idx == job_bank) {
            bank_in = false;
            if (job_is_store) {
                const char *why = prepare_store();
                if (why) {
                    job_finish(why);  /* nothing was sent: the unit is as it was */
                } else {
                    job_stage = JOB_SEND;
                    stream_pos = 0;
                    stream_next = 0;
                    synth_notice("WRITING THE BANK TO THE SR...");
                }
            } else {
                ESP_LOGI(TAG, "bank %d is in: saving", job_bank);
                job_stage = JOB_SAVE;
                save_n = -1;
            }
        } else if (now > job_deadline) {
            if (++job_tries < 2) {
                job_stage = JOB_ASK;  /* the first request after boot may get lost */
            } else if (!job_is_store && (import_mask & ~(1 << job_bank))) {
                import_mask &= ~(1 << job_bank);  /* no answer: next bank */
                job_bank = __builtin_ctz(import_mask);
                job_tries = 0;
                job_stage = JOB_ASK;
            } else {
                job_finish(job_is_store ? "STORE STOPPED: no reply from the SR" : "IMPORT STOPPED: no reply from the SR");
            }
        }
        break;
    case JOB_SAVE:
        if (save_n < 0) {
            delete_bank_files(job_bank);  /* the earlier import of this bank */
            save_n = 0;
        } else if (save_n < kind->count) {
            job_saved += save_item_to_library(job_bank, save_n++);
        } else {
            import_mask &= ~(1 << job_bank);
            if (import_mask) {
                job_bank = __builtin_ctz(import_mask);
                job_tries = 0;
                job_stage = JOB_ASK;
            } else {
                snprintf(buf, sizeof(buf), "IMPORTED %d %s", job_saved, kind == NULL ? "" : kind->item);
                job_finish(buf);
            }
        }
        break;
    case JOB_SEND: {
        if (now < stream_next) {
            break;
        }
        size_t len = bank_len(kind), rem = len - stream_pos, n = rem > STREAM_CHUNK ? STREAM_CHUNK : rem;
        bool last = n == rem;
        if (midi_send_sysex_part(bank_buf + stream_pos, n, last)) {
            stream_pos += n;
            stream_next = now + (int64_t)n * STREAM_BYTE_US;
            if (last) {
                sr_code = 0;
                job_deadline = now + 8000000;
                job_stage = JOB_ACK;  /* the SR answers when it has taken the bank */
            }
        }
        break;
    }
    case JOB_ACK:
        if (sr_code == 0x23 || sr_code == 0x21) {
            last_ok = true;
            job_finish("SR: DATA LOADED");
        } else if (sr_code == 0x24 || sr_code == 0x22) {
            job_finish("SR: ERROR - memory protect OFF? (GLOBAL)");
        } else if (now > job_deadline) {
            job_finish("SENT, NO ANSWER FROM THE SR");
        }
        break;
    default:
        break;
    }
}

bool ws_banks_sysex(const uint8_t *d, size_t len)
{
    if (len == 6 && d[1] == 0x42 && d[3] == 0x28) {
        sr_code = d[4];  /* 21 write complete, 22 write error, 23 data load completed, 24 data load error */
        ESP_LOGI(TAG, "SR status %02X", d[4]);
        return true;
    }
    if (kind && job_stage == JOB_WAIT && len == bank_len(kind) && d[1] == 0x42 && d[3] == 0x28 && d[4] == kind->reply) {
        int sum = 0;
        for (size_t i = BANK_HEADER; i < len - 2; i++) {
            sum += d[i];
        }
        if ((sum & 0x7F) != d[len - 2]) {
            ESP_LOGW(TAG, "bank dump: bad checksum");
        } else if (bank_alloc()) {
            memcpy(bank_buf, d, len);
            bank_in_idx = ws_from_ws_bank[d[5] % 12];
            bank_in = true;
            ESP_LOGI(TAG, "bank %d received", bank_in_idx);
        }
        return true;
    }
    return false;
}

/* ---- the job's screen (synth_t.busy): over everything, no input; it can't be cancelled (a bank half read or
 * sent leaves the SR and the editor out of step) ---- */

void ws_banks_draw(int disp)
{
    char buf[64];
    int64_t now = esp_timer_get_time();
    int banks_done = import_total - __builtin_popcount(import_mask & 0x0FFF);
    float frac;  /* of the bank */
    const char *status;
    int elapsed_s = job_stage == JOB_WAIT ? (int)((now - (job_deadline - BANK_TIMEOUT_US)) / 1000000) : 0;
    switch (job_stage) {
    case JOB_ASK:
    case JOB_WAIT:
        status = job_is_store ? "reading the bank" : "waiting for the dump";
        frac = elapsed_s / 15.0f > 0.95f ? 0.95f : elapsed_s / 15.0f;
        break;
    case JOB_SAVE:
        status = "saving to the library";
        frac = save_n < 0 ? 0.0f : (float)save_n / kind->count;
        break;
    case JOB_SEND:
        status = "writing the bank";
        frac = (float)stream_pos / bank_len(kind);
        break;
    default:
        status = "waiting for the SR's answer";
        frac = 1.0f;
        break;
    }
    float total = job_is_store ? (job_stage <= JOB_WAIT ? 0.4f * frac
                                  : job_stage == JOB_SEND ? 0.4f + 0.6f * frac : 1.0f)
                                : (banks_done + (job_stage == JOB_SAVE ? 0.5f + 0.5f * frac : 0.5f * frac)) /
                                      (import_total ? import_total : 1);
    if (disp == 0) {
        display_text(0, 1, 0, &font_small, job_is_store ? "STORING TO THE SR" : "IMPORTING FROM THE SR", true);
        display_fill_rect(0, 0, 11, 255, 1, true);
        if (job_is_store) {
            snprintf(buf, sizeof(buf), "%s  %s %d", ws_bank_names[job_bank], kind->item, store_num);
        } else {
            snprintf(buf, sizeof(buf), "%s  (%d / %d)", ws_bank_names[job_bank], banks_done + 1, import_total);
        }
        display_text(0, 4, 18, &font_medium, buf, true);
        if (job_stage == JOB_WAIT) {
            snprintf(buf, sizeof(buf), "%s  %d s", status, elapsed_s);
        } else {
            snprintf(buf, sizeof(buf), "%s", status);
        }
        display_text(0, 4, 42, &font_small, buf, true);
    } else {
        display_text(1, 1, 0, &font_small, "do not touch the SR or the knobs", true);
        display_fill_rect(1, 0, 11, 255, 1, true);
        display_rect(1, 6, 18, 242, 18, true);
        display_fill_rect(1, 8, 20, (int)(238 * (total > 1.0f ? 1.0f : total)), 14, true);
        snprintf(buf, sizeof(buf), "%d %%", (int)(total * 100));
        display_text(1, 6, 42, &font_medium, buf, true);
    }
}

/* ---- wave names (kept in memory: reading a file while a knob is turned stalls the editor) ---- */

#define ROM_WAVES 486
static char rom_wave_names[ROM_WAVES][WS_WAVE_NAME_LEN];
static char seq_names[12][32][WS_WAVE_NAME_LEN];
static bool names_loaded;

static void read_lines(const char *path, char *out, int count, int stride)
{
    FILE *f = fopen(path, "r");
    char line[64];
    for (int i = 0; f && i < count && fgets(line, sizeof(line), f); i++) {
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out + i * stride, stride, "%s", line);
    }
    if (f) {
        fclose(f);
    }
}

static void load_names(void)
{
    if (names_loaded) {
        return;
    }
    names_loaded = true;
    read_lines(STORAGE_ROOT "/ws/waves", &rom_wave_names[0][0], ROM_WAVES, WS_WAVE_NAME_LEN);
    for (int b = 0; b < 12; b++) {
        char path[40];
        snprintf(path, sizeof(path), STORAGE_ROOT "/ws/wavesq/%02d", b);
        read_lines(path, &seq_names[b][0][0], 32, WS_WAVE_NAME_LEN);
    }
}

/* Wave 0-31 are wave sequences (named per bank), the rest ROM waves; a card has its own */
void ws_wave_name(int bank, int num, char *buf, size_t len)
{
    load_names();
    if (num < 32) {
        snprintf(buf, len, "%s", seq_names[bank][num]);
    } else if (bank != 11 && num - 32 < ROM_WAVES) {
        snprintf(buf, len, "%s", rom_wave_names[num - 32]);
    } else {
        buf[0] = 0;
    }
}

