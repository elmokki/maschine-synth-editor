/*
 * Roland JV-880, Performance mode: a performance = a common part (name,
 * reverb, chorus, voice reserves) + eight parts, each playing a patch (I, C,
 * A or B 01-64; part 8 is the rhythm part: INT, CRD, PRA or PRB) on its own
 * MIDI channel. Group A PART (everything about the part edited, on one
 * page: patch, level, pan, output, tuning, voice reserve, receive channel and
 * the effect and receive switches), B COMMON (reverb, chorus); the part is
 * picked with Shift + Group A-H (the part's number) or the transport Step
 * arrows.
 *
 * Every change goes out at once as a DT1 to the Temporary Performance
 * (manual p. 223, 224-225). Reading it back (Auto) is optional and needs the
 * JV-880's MIDI OUT on the Maschine's MIDI IN: values are "--" until a
 * performance is loaded, read or the parameter is changed. The same read
 * brings in the names of the internal patches, shown with a part's patch
 * (the presets' names come from the library).
 *
 * A performance file is the performance: 311 bytes (31 common + 8 x 35 part).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "buttons.h"
#include "display.h"
#include "esp_log.h"
#include "jv880_common.h"
#include "midi.h"
#include "storage.h"
#include "synth.h"

static const char *TAG = "jv880p";

#define COMMON_LEN 0x1F
#define PART_LEN 0x23
#define PARTS 8
#define PERF_LEN (COMMON_LEN + PARTS * PART_LEN)
#define BLOCKS (PARTS + 1)  /* block 0 = common, 1-8 = parts */
#define TEMP_PERF JV880_ADDR(0x00, 0x00, 0x10, 0x00)
#define PRESET_FIRST 65     /* library number of preset A01 */
#define PRESETS 128

/* ---- parameters ---- */

typedef enum {
    K_NUM,
    K_SIGNED,   /* value - center */
    K_PLUS1,    /* stored 0.., shown 1.. */
    K_LIST,     /* labels[value - min] */
    K_SWITCH,
    K_PAN,      /* 0-127 = L64..63R */
    K_PATCH,    /* patch number: I/C/A/B 01-64, or the rhythm part's source */
} kind_t;

typedef struct {
    const char *name;  /* fits a 64 px display column */
    uint8_t off;       /* in the common part, or in each part */
    kind_t kind;
    int16_t min, max, center;
    const char *const *labels;
    bool part;         /* in each part, not the common part */
    bool wide;         /* two nibble bytes (off = high, off + 1 = low) */
} jv_param_t;

static const char *const reverb_labels[] = {"ROOM1", "ROOM2", "STAGE1", "STAGE2", "HALL1", "HALL2", "DELAY", "PAN-DLY"};
static const char *const chorus_labels[] = {"CHORUS1", "CHORUS2", "CHORUS3"};
static const char *const output_labels[] = {"MIX", "SUB", "PATCH"};

enum {
    /* common */
    P_REV_TYPE, P_REV_LEVEL, P_REV_TIME, P_REV_FB, P_CHO_TYPE, P_CHO_LEVEL, P_CHO_DEPTH, P_CHO_RATE, P_CHO_FB,
    P_CHO_OUT,
    P_RESERVE,  /* the voice reserve of the part edited: sits in the common part, one byte a part */
    COMMON_PARAMS,
    /* part */
    P_RX_SW = COMMON_PARAMS, P_RX_CH, P_PATCH, P_LEVEL, P_PAN, P_COARSE, P_FINE, P_REV_SW, P_CHO_SW, P_RX_PC,
    P_RX_VOL, P_RX_HOLD, P_OUT,
    PARAM_COUNT
};

#define CP(name, off, kind, min, max, center, labels) {name, off, kind, min, max, center, labels, false, false}
#define PP(name, off, kind, min, max, center, labels) {name, off, kind, min, max, center, labels, true, false}

static const jv_param_t params[PARAM_COUNT] = {
    [P_REV_TYPE] = CP("REV TYPE", 0x0D, K_LIST, 0, 7, 0, reverb_labels),
    [P_REV_LEVEL] = CP("REV LEVEL", 0x0E, K_NUM, 0, 127, 0, 0),
    [P_REV_TIME] = CP("REV TIME", 0x0F, K_NUM, 0, 127, 0, 0),
    [P_REV_FB] = CP("DELAY FB", 0x10, K_NUM, 0, 127, 0, 0),
    [P_CHO_TYPE] = CP("CHO TYPE", 0x11, K_LIST, 0, 2, 0, chorus_labels),
    [P_CHO_LEVEL] = CP("CHO LEVEL", 0x12, K_NUM, 0, 127, 0, 0),
    [P_CHO_DEPTH] = CP("CHO DEPTH", 0x13, K_NUM, 0, 127, 0, 0),
    [P_CHO_RATE] = CP("CHO RATE", 0x14, K_NUM, 0, 127, 0, 0),
    [P_CHO_FB] = CP("CHO FDBK", 0x15, K_NUM, 0, 127, 0, 0),
    [P_CHO_OUT] = CP("CHO > REV", 0x16, K_SWITCH, 0, 1, 0, 0),  /* chorus output: MIX / REV */
    [P_RESERVE] = CP("RESERVE", 0x17, K_NUM, 0, 28, 0, 0),

    [P_RX_SW] = PP("RX SWITCH", 0x15, K_SWITCH, 0, 1, 0, 0),
    [P_RX_CH] = PP("RX CHANNEL", 0x16, K_PLUS1, 0, 15, 0, 0),
    [P_PATCH] = {"PATCH", 0x17, K_PATCH, 0, 255, 0, 0, true, true},
    [P_LEVEL] = PP("LEVEL", 0x19, K_NUM, 0, 127, 0, 0),
    [P_PAN] = PP("PAN", 0x1A, K_PAN, 0, 127, 0, 0),
    [P_COARSE] = PP("COARSE", 0x1B, K_SIGNED, 16, 112, 64, 0),
    [P_FINE] = PP("FINE", 0x1C, K_SIGNED, 14, 114, 64, 0),
    [P_REV_SW] = PP("REVERB", 0x1D, K_SWITCH, 0, 1, 0, 0),
    [P_CHO_SW] = PP("CHORUS", 0x1E, K_SWITCH, 0, 1, 0, 0),
    [P_RX_PC] = PP("RX PC", 0x1F, K_SWITCH, 0, 1, 0, 0),
    [P_RX_VOL] = PP("RX VOL", 0x20, K_SWITCH, 0, 1, 0, 0),
    [P_RX_HOLD] = PP("RX HOLD-1", 0x21, K_SWITCH, 0, 1, 0, 0),
    [P_OUT] = PP("OUTPUT", 0x22, K_LIST, 0, 2, 0, output_labels),
};

/* ---- pages ---- */

#define _ SYNTH_NONE
enum { PG_PART, PG_REVERB, PG_CHORUS2 };
#define NO_SW {_, _, _, _, _, _, _, _}
static const synth_page_t pages[] = {
    /* A PART: all of the part's parameters on one page. The effect sends sit
     * over pan and output, the receive switches over what they belong to */
    [PG_PART] = {0, "PART", {P_PATCH, P_LEVEL, P_PAN, P_OUT, P_COARSE, P_FINE, P_RESERVE, P_RX_CH},
                 {P_RX_PC, P_RX_VOL, P_REV_SW, P_CHO_SW, _, _, P_RX_HOLD, P_RX_SW}},
    /* B COMMON: reverb and chorus (the chorus feedback and output on the second page) */
    [PG_REVERB] = {1, "REVERB / CHORUS", {P_REV_TYPE, P_REV_LEVEL, P_REV_TIME, P_REV_FB, P_CHO_TYPE, P_CHO_LEVEL,
                                         P_CHO_DEPTH, P_CHO_RATE}, NO_SW},
    [PG_CHORUS2] = {1, "CHORUS 2", {P_CHO_FB, _, _, _, _, _, _, _}, {P_CHO_OUT, _, _, _, _, _, _, _}},
};
#undef _

/* ---- settings ---- */

#define SETTING_UNIT 1
#define SETTING_VOLUME_JV 2
static const synth_setting_t settings[] = {
    /* The pads play the part on its receive channel once that is known */
    [SETTING_CHANNEL] = {"PART CH", SET_CHANNEL, 0, 15, 0},
    [SETTING_UNIT] = {"UNIT #", SET_NUMBER, 17, 32, 17},
    [SETTING_VOLUME_JV] = {"VOLUME", SET_NUMBER, 0, 127, 100},
};

/* ---- state ---- */

static uint8_t perf[PERF_LEN], init_perf[PERF_LEN];
static uint8_t as_read[PERF_LEN];  /* the performance as the unit last gave it, for Rec + Erase */
static bool as_read_ok;
static bool known[PERF_LEN];
static bool init_read;
static int part;  /* 0-7: the part edited */

/* Names of the internal patches (read from the unit) and of the presets A and
 * B (from the library: 065-192 in /lfs/jv880) */
#define NAME_LEN 13
static char int_names[64][NAME_LEN], preset_names[PRESETS][NAME_LEN];
static bool int_known[64], presets_scanned;
/* ...and of the internal performances I01-I16, shown when picking where to store */
#define UNIT_PERFS 16
static char perf_names[UNIT_PERFS][NAME_LEN];
static bool perf_known[UNIT_PERFS];

static int device_id(void) { return synth_setting(SETTING_UNIT) - 1; }

static int byte_of(int p, int pt)
{
    if (p == P_RESERVE) {
        return params[p].off + pt;
    }
    return params[p].part ? COMMON_LEN + pt * PART_LEN + params[p].off : params[p].off;
}

static void read_init(void)
{
    if (init_read) {
        return;
    }
    FILE *f = fopen(STORAGE_ROOT "/jv880p/000 Init", "rb");
    if (f) {
        init_read = fread(init_perf, 1, PERF_LEN, f) == PERF_LEN;
        fclose(f);
    }
}

static int get(const uint8_t *buf, int p, int pt)
{
    int at = byte_of(p, pt);
    return params[p].wide ? buf[at] << 4 | buf[at + 1] : buf[at];
}

static bool value_known(int p) { return known[byte_of(p, part)]; }

static int value_or_init(int p, int pt)
{
    if (known[byte_of(p, pt)]) {
        return get(perf, p, pt);
    }
    read_init();
    return get(init_perf, p, pt);
}

static bool is_rhythm(int pt) { return pt == PARTS - 1; }

/* ---- sending ---- */

/* Address of performance buffer index `at` when the performance starts at base */
static uint32_t address_of(uint32_t base, int at)
{
    if (at < COMMON_LEN) {
        return base + at;
    }
    int pt = (at - COMMON_LEN) / PART_LEN, w = (at - COMMON_LEN) % PART_LEN;
    return base + JV880_ADDR(0, 0, 8 + pt, 0) + w;  /* part n at +08 00 + n * 01 00 */
}

static void send_bytes_to(uint32_t base, int from, int len)
{
    uint8_t msg[13 + PART_LEN];
    int n = jv880_dt1(device_id(), address_of(base, from), perf + from, len, msg);
    midi_send_sysex(msg, n);
}

static void send_bytes(int from, int len) { send_bytes_to(TEMP_PERF, from, len); }

/* Whole performances go out a block at a time from tick(), so the MIDI queue
 * never overflows and the unit gets time for each */
static uint16_t pending;  /* bit n = block n still to send */
static uint32_t pending_base = TEMP_PERF;
static int64_t next_send_us;
#define SEND_GAP_US 12000
#define BYTE_US 330  /* 31250 baud DIN */

static void block_range(int b, int *from, int *len)
{
    *from = b ? COMMON_LEN + (b - 1) * PART_LEN : 0;
    *len = b ? PART_LEN : COMMON_LEN;
}

static void send_perf(uint32_t base)
{
    pending = (1 << BLOCKS) - 1;
    pending_base = base;
    next_send_us = 0;
}

/* ---- reading back (optional: needs the JV-880's MIDI OUT on the
 * Maschine's MIDI IN) ---- */

/* A read asks for the blocks one by one, then the internal patches' and
 * performances' names;
 * ones that didn't arrive (the first request after boot often gets lost) are
 * asked for again, twice at most */
#define READ_ITEMS (BLOCKS + 64 + UNIT_PERFS)
#define READ_GAP_US 60000
#define READ_CHECK_US 400000
#define READ_TRIES 3
static bool read_asked;  /* Auto pressed: the reply replaces the performance */
static int read_item = READ_ITEMS;  /* next to ask for, READ_ITEMS = idle */
static bool read_have[READ_ITEMS];
static int read_tries;
static int64_t read_check_at;  /* when to see what's missing, 0 = not waiting */

/* A 12-character name from the unit, without its trailing spaces */
static void copy_name(char *out, const uint8_t *data)
{
    memcpy(out, data, 12);
    out[12] = 0;
    for (int i = 11; i >= 0 && out[i] == ' '; i--) {
        out[i] = 0;
    }
}

/* ---- import: the unit's internal performances into the library ----
 * Reading I01-I16 (plain RQ1s, manual p. 223) saves them as 001-016, replacing
 * the earlier import; they're protected (own performances start at
 * FIRST_USER_PATCH). Needs the JV-880's MIDI OUT connected. */
#define FIRST_USER_PATCH 100
#define IMPORT_TIMEOUT_US 1500000
typedef enum { IMP_IDLE, IMP_ASK, IMP_REPLY } imp_stage_t;
static imp_stage_t imp_stage;
static int imp_slot, imp_block, imp_saved;
static uint8_t imp_buf[PERF_LEN];
static int64_t imp_deadline;

static uint32_t imp_base(void) { return JV880_ADDR(1, imp_slot, 0x10, 0); }

static void import_finish(const char *why)
{
    imp_stage = IMP_IDLE;
    synth_patches_changed();
    char buf[48];
    if (why) {
        snprintf(buf, sizeof(buf), "%s", why);
    } else {
        snprintf(buf, sizeof(buf), "IMPORTED %d PERFORMANCES", imp_saved);
    }
    synth_notice(buf);
    ESP_LOGI(TAG, "import: %s (%d saved)", why ? why : "done", imp_saved);
}

static void import_save(void)
{
    char name[NAME_LEN];
    copy_name(name, imp_buf);
    for (char *c = name; *c; c++) {
        if (*c == '/' || *c < 32 || *c > 126) {
            *c = '_';
        }
    }
    char path[96];
    snprintf(path, sizeof(path), STORAGE_ROOT "/jv880p/%03d %s", imp_slot + 1, name[0] ? name : "Performance");
    FILE *f = fopen(path, "wb");
    if (f) {
        imp_saved += fwrite(imp_buf, 1, PERF_LEN, f) == PERF_LEN;
        fclose(f);
    }
}

static void import_start(void)
{
    if (imp_stage != IMP_IDLE) {
        return;
    }
    mkdir(STORAGE_ROOT "/jv880p", 0777);
    DIR *dir = opendir(STORAGE_ROOT "/jv880p");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        if (n >= 1 && n <= UNIT_PERFS) {
            char path[300];
            snprintf(path, sizeof(path), STORAGE_ROOT "/jv880p/%s", e->d_name);
            remove(path);  /* the earlier import */
        }
    }
    if (dir) {
        closedir(dir);
    }
    pending = 0;
    read_item = READ_ITEMS;
    imp_slot = imp_block = imp_saved = 0;
    imp_stage = IMP_ASK;
    synth_notice("IMPORT: asking the JV-880...");
}

/* Buffer index of address addr in the performance at base, or -1 */
static int index_in(uint32_t addr, uint32_t base)
{
    int off = (int)(addr - base), k = off >> 7, w = off & 0x7F;
    int b = k == 0 ? 0 : k >= 8 && k < 8 + PARTS ? k - 7 : -1;
    if (off < 0 || b < 0) {
        return -1;
    }
    int from, len;
    block_range(b, &from, &len);
    return w < len ? from + w : -1;
}

static void request_item(int item)
{
    uint8_t msg[17];
    int n;
    if (item < BLOCKS) {
        int from, len;
        block_range(item, &from, &len);
        n = jv880_rq1(device_id(), address_of(TEMP_PERF, from), len, msg);
    } else if (item < BLOCKS + 64) {
        n = jv880_rq1(device_id(), JV880_ADDR(1, 0x40 + item - BLOCKS, 0x20, 0), 12, msg);  /* name of patch I01.. */
    } else {
        n = jv880_rq1(device_id(), JV880_ADDR(1, item - BLOCKS - 64, 0x10, 0), 12, msg);  /* name of performance I01.. */
    }
    midi_send_sysex(msg, n);
}

static bool all_in(void)
{
    for (int i = 0; i < READ_ITEMS; i++) {
        if (!read_have[i]) {
            return false;
        }
    }
    return true;
}

static void tick(int64_t now)
{
    if (imp_stage == IMP_ASK) {
        int from, len;
        block_range(imp_block, &from, &len);
        uint8_t msg[17];
        int n = jv880_rq1(device_id(), address_of(imp_base(), from), len, msg);
        midi_send_sysex(msg, n);
        imp_stage = IMP_REPLY;
        imp_deadline = now + IMPORT_TIMEOUT_US;
        return;
    }
    if (imp_stage == IMP_REPLY) {
        if (now > imp_deadline) {
            import_finish("IMPORT stopped: no reply - JV-880 MIDI OUT connected?");
        }
        return;
    }
    if (pending && now >= next_send_us) {
        int b = __builtin_ctz(pending);
        int from, len;
        block_range(b, &from, &len);
        pending &= ~(1 << b);
        send_bytes_to(pending_base, from, len);
        next_send_us = now + SEND_GAP_US + (int64_t)len * BYTE_US;
    } else if (!pending && read_item < READ_ITEMS && now >= next_send_us) {
        while (read_item < READ_ITEMS && read_have[read_item]) {
            read_item++;  /* this one is in already */
        }
        if (read_item < READ_ITEMS) {
            request_item(read_item++);
            next_send_us = now + READ_GAP_US;
        }
        if (read_item >= READ_ITEMS) {
            read_check_at = now + READ_CHECK_US;
        }
    } else if (read_check_at && read_item >= READ_ITEMS && now >= read_check_at) {
        read_check_at = 0;
        if (!all_in() && read_tries < READ_TRIES) {
            read_tries++;
            read_item = 0;  /* the missing ones again */
        }
    }
}

static void read_perf(void)
{
    if (imp_stage != IMP_IDLE) {
        import_finish("IMPORT stopped");  /* Auto during an import stops it */
        return;
    }
    read_asked = true;
    as_read_ok = false;  /* the new read is what Rec + Erase goes back to */
    read_item = 0;
    memset(read_have, 0, sizeof(read_have));
    read_tries = 1;
    read_check_at = 0;
    pending = 0;
    next_send_us = 0;
}

static void sysex_in(const uint8_t *d, size_t len)
{
    uint32_t addr;
    const uint8_t *data;
    int n = jv880_parse_dt1(d, len, device_id(), &addr, &data);
    if (n < 1) {
        ESP_LOGW(TAG, "SysEx ignored: %d bytes", (int)len);
        return;
    }
    if (imp_stage == IMP_REPLY) {
        int from, blen;
        block_range(imp_block, &from, &blen);
        for (int i = 0; i < n; i++) {
            int at = index_in(addr + i, imp_base());
            if (at >= from && at < from + blen) {
                imp_buf[at] = data[i];
            }
        }
        if (index_in(addr, imp_base()) == from && n == blen) {
            if (++imp_block < BLOCKS) {
                imp_stage = IMP_ASK;
            } else {
                import_save();
                char buf[32];
                snprintf(buf, sizeof(buf), "IMPORT %d / %d", imp_slot + 1, UNIT_PERFS);
                synth_notice(buf);
                imp_block = 0;
                if (++imp_slot >= UNIT_PERFS) {
                    import_finish(NULL);
                } else {
                    imp_stage = IMP_ASK;
                }
            }
        }
        return;
    }
    /* An internal performance's name (I01 = 01 00 10 00, +01 00 00 each) */
    if (n == 12 && addr >> 21 == 1 && (addr & 0x3FFF) == JV880_ADDR(0, 0, 0x10, 0) && (addr >> 14 & 0x7F) < UNIT_PERFS) {
        int slot = addr >> 14 & 0x7F;
        copy_name(perf_names[slot], data);
        perf_known[slot] = true;
        read_have[BLOCKS + 64 + slot] = true;
        return;
    }
    /* An internal patch's name (I01 = 01 40 20 00, +01 00 00 each) */
    if (n == 12 && addr >> 21 == 1 && (addr & 0x3FFF) == JV880_ADDR(0, 0, 0x20, 0) && (addr >> 14 & 0x7F) >= 0x40) {
        int slot = (addr >> 14 & 0x7F) - 0x40;
        copy_name(int_names[slot], data);
        int_known[slot] = true;
        read_have[BLOCKS + slot] = true;
        if (!is_rhythm(part) && known[byte_of(P_PATCH, part)] && (get(perf, P_PATCH, part) >> 6) == 0 &&
            (get(perf, P_PATCH, part) & 63) == slot) {
            synth_redraw();
        }
        return;
    }
    int off = (int)(addr - TEMP_PERF), k = off >> 7, w = off & 0x7F;
    int b = k == 0 ? 0 : k >= 8 && k < 8 + PARTS ? k - 7 : -1;
    if (off < 0 || b < 0) {
        return;
    }
    int from, blen;
    block_range(b, &from, &blen);
    if (w + n > blen) {
        return;
    }
    for (int i = 0; i < n; i++) {
        perf[from + w + i] = data[i];
        known[from + w + i] = true;
    }
    if (w == 0 && n == blen) {
        read_have[b] = true;
    }
    if (b == 0 && w == 0) {
        synth_state_received();  /* the performance's name is in: no fallback */
        if (read_asked) {
            /* Auto (read) on purpose: the unit's performance replaces whatever
             * library performance was loaded */
            read_asked = false;
            synth_sound_replaced();
        }
    }
    bool blocks_in = true;
    for (int i = 0; i < BLOCKS; i++) {
        blocks_in = blocks_in && read_have[i];
    }
    if (blocks_in && !as_read_ok) {
        memcpy(as_read, perf, PERF_LEN);
        as_read_ok = true;
    }
    if (b == 0 || b == part + 1) {
        synth_redraw();
    }
}

/* Rec + Erase on the unit's own sound: back to what it was when read */
static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(perf, as_read, PERF_LEN);
    memset(known, 1, PERF_LEN);
    send_perf(TEMP_PERF);
    return true;
}

/* ---- editing ---- */

/* Stores v for part pt (or the common part) and sends it */
static void set_one(int p, int pt, int v)
{
    int at = byte_of(p, pt), len = 1;
    if (params[p].wide) {
        perf[at] = v >> 4 & 0x0F;
        perf[at + 1] = v & 0x0F;
        known[at + 1] = true;
        len = 2;
    } else {
        perf[at] = v;
    }
    known[at] = true;
    send_bytes(at, len);
}

/* The voice reserves of the other parts: the eight must not add up to over 28 */
static int others_reserve(void)
{
    int sum = 0;
    for (int i = 0; i < PARTS; i++) {
        if (i != part) {
            sum += value_or_init(P_RESERVE, i);
        }
    }
    return sum;
}

static bool param_step(int p, int steps)
{
    int v = value_or_init(p, part), nv;
    int max = params[p].max;
    if (p == P_PATCH && is_rhythm(part)) {
        /* the rhythm part's source: INT, CRD, PRA or PRB, kept as the group's
         * first number */
        nv = (v >> 6) + (steps > 0 ? 1 : -1);
        nv = (nv < 0 ? 0 : nv > 3 ? 3 : nv) << 6;
    } else {
        if (p == P_RESERVE) {
            max = 28 - others_reserve();
            max = max < 0 ? 0 : max;
        }
        nv = v + steps;
        nv = nv < params[p].min ? params[p].min : nv > max ? max : nv;
    }
    if (nv == v && known[byte_of(p, part)]) {
        return false;
    }
    set_one(p, part, nv);
    return true;
}

static void param_toggle(int p) { set_one(p, part, known[byte_of(p, part)] ? !get(perf, p, part) : 0); }

static bool param_reset(int p)
{
    read_init();
    int v = get(init_perf, p, part);
    if (value_known(p) && get(perf, p, part) == v) {
        return false;
    }
    set_one(p, part, v);
    return true;
}

/* ---- formatting ---- */

static const char *param_name(int p) { return params[p].name; }

/* I01-I64, C01-C64, A01-A64, B01-B64 - or, for the rhythm part, where its
 * kit comes from */
static void patch_code(int v, bool rhythm, char *buf, size_t len)
{
    static const char *const rhythm_source[] = {"INT", "CRD", "PRA", "PRB"};
    if (rhythm) {
        snprintf(buf, len, "%s", rhythm_source[v >> 6 & 3]);
    } else {
        snprintf(buf, len, "%c%02d", "ICAB"[v >> 6 & 3], (v & 63) + 1);
    }
}

static void param_format(int p, char *buf, size_t len)
{
    const jv_param_t *pp = &params[p];
    if (!value_known(p)) {
        snprintf(buf, len, "--");
        return;
    }
    int v = get(perf, p, part);
    switch (pp->kind) {
    case K_SIGNED:
        snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        break;
    case K_PLUS1:
        snprintf(buf, len, "%d", v + 1);
        break;
    case K_LIST:
        snprintf(buf, len, "%s", pp->labels[v - pp->min <= pp->max - pp->min ? v - pp->min : 0]);
        break;
    case K_SWITCH:
        snprintf(buf, len, v ? "ON" : "OFF");
        break;
    case K_PAN:
        if (v < 64) {
            snprintf(buf, len, "L%d", 64 - v);
        } else {
            snprintf(buf, len, v == 64 ? "C" : "%dR", v - 64);
        }
        break;
    case K_PATCH:
        patch_code(v, is_rhythm(part), buf, len);
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    const jv_param_t *pp = &params[p];
    if (!value_known(p) || pp->kind == K_LIST || pp->kind == K_SWITCH || pp->kind == K_PATCH) {
        return -1;
    }
    return (get(perf, p, part) - pp->min) * 1000 / (pp->max - pp->min);
}

static bool param_on(int p) { return value_known(p) && get(perf, p, part); }

static int param_units(int p)
{
    if (p == P_PATCH) {
        return is_rhythm(part) ? 80 : 13;  /* a patch among 256: slow; fast turns accelerate */
    }
    int range = params[p].max - params[p].min;
    return range <= 1 ? 80 : range <= 4 ? 70 : range <= 16 ? 40 : range <= 30 ? 25 : 10;
}

/* The presets' names: the library's 065-192, "NNN Name" */
static void scan_presets(void)
{
    if (presets_scanned) {
        return;
    }
    presets_scanned = true;
    DIR *dir = opendir(STORAGE_ROOT "/jv880");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        const char *sp = strchr(e->d_name, ' ');
        if (n >= PRESET_FIRST && n < PRESET_FIRST + PRESETS && sp) {
            snprintf(preset_names[n - PRESET_FIRST], NAME_LEN, "%s", sp + 1);
        }
    }
    if (dir) {
        closedir(dir);
    }
}

/* The patch's name when one is known: an internal patch's (read from the
 * unit), a preset's (the library) */
static const char *patch_name(int v)
{
    int group = v >> 6 & 3, n = v & 63;
    if (group == 0) {
        return int_known[n] ? int_names[n] : "";
    }
    return group >= 2 ? preset_names[(group - 2) * 64 + n] : "";
}

/* The patch's code and name in the column */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    (void)h;
    if (p != P_PATCH || !value_known(p)) {
        return SYNTH_DRAW_NONE;
    }
    char code[8];
    int v = get(perf, P_PATCH, part);
    patch_code(v, is_rhythm(part), code, sizeof(code));
    jv880_draw_code_name(disp, x, y, w, code, is_rhythm(part) ? "rhythm kit" : patch_name(v));
    return SYNTH_DRAW_FULL;
}

/* Patch list (Note Repeat) for the part: INT, CARD, PRESET A and B; a rhythm part picks its source with the knob */
static int patch_list_current(void) { return get(perf, P_PATCH, part); }

static void patch_list_name(int i, char *buf, size_t len)
{
    char code[8];
    scan_presets();
    patch_code(i, false, code, sizeof(code));
    snprintf(buf, len, "%s %s", code, patch_name(i));
}

static void patch_list_pick(int i)
{
    param_step(P_PATCH, i - patch_list_current());
}

static const synth_list_t patch_list = {"PATCHES", 256, patch_list_current, patch_list_name, patch_list_pick, P_PATCH};

static const synth_list_t *page_list(int page)
{
    return page == PG_PART && value_known(P_PATCH) && !is_rhythm(part) ? &patch_list : NULL;
}

/* ---- patch overview (Grid): the eight parts as a table (no header: eight rows fill the display) ---- */

static void overview_draw(int page, int disp)
{
    (void)page;
    char buf[64], code[8], pan[8];
    for (int pt = 0; pt < PARTS; pt++) {
        int y = pt * 8;
        char mark = pt == part ? '>' : ' ';
        bool k = known[byte_of(P_PATCH, pt)];
        if (disp == 0) {
            if (!k) {
                snprintf(buf, sizeof(buf), "%c%d --", mark, pt + 1);
            } else {
                int v = get(perf, P_PATCH, pt);
                patch_code(v, is_rhythm(pt), code, sizeof(code));
                snprintf(buf, sizeof(buf), "%c%d %s %-12.12s CH%2d LV%3d%s", mark, pt + 1, code,
                         is_rhythm(pt) ? "rhythm kit" : patch_name(v), get(perf, P_RX_CH, pt) + 1,
                         get(perf, P_LEVEL, pt), get(perf, P_RX_SW, pt) ? "" : " off");
            }
        } else if (k) {
            int pn = get(perf, P_PAN, pt);
            if (pn < 64) {
                snprintf(pan, sizeof(pan), "L%d", 64 - pn);
            } else {
                snprintf(pan, sizeof(pan), pn == 64 ? "C" : "%dR", pn - 64);
            }
            snprintf(buf, sizeof(buf), "PAN %s CRS%+d FN%+d %s%sOUT %s RSV %d", pan, get(perf, P_COARSE, pt) - 64,
                     get(perf, P_FINE, pt) - 64, get(perf, P_REV_SW, pt) ? "REV " : "",
                     get(perf, P_CHO_SW, pt) ? "CHO " : "", output_labels[get(perf, P_OUT, pt) % 3],
                     get(perf, P_RESERVE, pt));
        } else {
            buf[0] = 0;
        }
        display_text(disp, 1, y, &font_tiny, buf, true);
    }
}

/* ---- parts ---- */

static void select_part(int pt)
{
    part = (pt + PARTS) % PARTS;
    synth_redraw();
}

/* Step arrows: the next part, round from 8 to 1 */
static void part_step(int d) { select_part(part + (d > 0 ? 1 : -1)); }

/* Shift + Group A-H: part 1-8 - the Group buttons are sections here, so
 * this is how a part is picked directly */
static bool shift_group(int g)
{
    select_part(g);
    return true;
}

static int shift_group_led(int g) { return g == part; }

static void header_tag(char *buf, size_t len) { snprintf(buf, len, "PART %d", part + 1); }

/* The performance's name, when known (e.g. read back) */
static bool sound_name(char *buf, size_t len)
{
    if (!known[0]) {
        return false;
    }
    char name[13];
    memcpy(name, perf, 12);
    name[12] = 0;
    for (int i = 11; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    snprintf(buf, len, "%s", name);
    return true;
}

/* The pads play the part edited, on its receive channel once that is known */
static int play_channel(void)
{
    int rx = byte_of(P_RX_CH, part);
    return known[rx] && perf[rx] < 16 ? perf[rx] : synth_setting(SETTING_CHANNEL);
}

/* Performance mode (System Common, panel mode, manual p. 223) */
static bool activate(void)
{
    scan_presets();
    uint8_t mode = 0, msg[16];
    int n = jv880_dt1(device_id(), JV880_SYSTEM_COMMON, &mode, 1, msg);
    midi_send_sysex(msg, n);
    return true;
}

/* ---- writing into the unit's own memory (Store: TO JV-880) ----
 * The performance goes to an internal performance (I01-I16, manual p. 223)
 * with plain DT1s, name first */

static void unit_slot_name(int slot, char *buf, size_t len) { snprintf(buf, len, "I%02d", slot + 1); }

/* What the slot holds now, once read (Auto) */
static void unit_slot_info(int slot, char *buf, size_t len)
{
    snprintf(buf, len, "%s", perf_known[slot] ? perf_names[slot] : "");
}

static void set_name(const char *name)
{
    for (int i = 0; i < 12; i++) {
        char c = *name ? *name++ : ' ';
        perf[i] = c >= 32 && c < 127 ? c : ' ';
        known[i] = true;
    }
}

static void fill_unknown(void)
{
    read_init();
    for (int i = 0; i < PERF_LEN; i++) {
        if (!known[i]) {
            perf[i] = init_perf[i];  /* never set: the init performance's */
        }
    }
}

static void unit_store(int slot, const char *name)
{
    set_name(name);
    fill_unknown();
    send_bytes(0, 12);
    send_perf(JV880_ADDR(1, slot, 0x10, 0));
    copy_name(perf_names[slot], perf);  /* what the slot holds from now on */
    perf_known[slot] = true;
    char buf[40];
    snprintf(buf, sizeof(buf), "WRITING TO I%02d", slot + 1);
    synth_notice(buf);
}

/* ---- performances (library) ---- */

static bool patch_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fread(perf, 1, PERF_LEN, f) == PERF_LEN;
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    memset(known, 1, PERF_LEN);
    as_read_ok = false;
    read_item = READ_ITEMS;  /* a read still going would bring the unit's old one back */
    read_check_at = 0;
    send_perf(TEMP_PERF);
    return true;
}

static bool patch_save(const char *path)
{
    /* The performance's own name (12 characters) follows the file's */
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    title = sp ? sp + 1 : title;
    fill_unknown();
    set_name(title);
    memset(known, 1, PERF_LEN);
    mkdir(STORAGE_ROOT "/jv880p", 0777);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(perf, 1, PERF_LEN, f) == PERF_LEN;
    fclose(f);
    return ok;
}

static int unknown_count(void)
{
    int n = 0;
    for (int i = 0; i < PERF_LEN; i++) {
        n += !known[i];
    }
    return n;
}

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"JV-880 PERF", {"Performance mode: eight parts, each", "playing a patch on its own channel.",
                     "A PART: all of the part's settings;", "B COMMON: reverb and chorus.",
                     "SHIFT + A-H: part 1-8 (A = part 1);", "STEP arrows: the next part.",
                     "Part 8 is the rhythm part."},
     {BTN_GROUP_A, BTN_GROUP_A + 1, BTN_SHIFT, BTN_STEP_LEFT, BTN_STEP_RIGHT, -1, -1, -1}},
    {"PATCHES", {"The patch is I01-I64 (internal), C (card),", "A / B (presets); part 8's is where its",
                 "kit comes from. Names of internal", "patches come with AUTO (read), the",
                 "presets' from the library (065-192).", "RESERVE: the part's voices, 28 in all."},
     {BTN_GROUP_A, BTN_AUTO, -1, -1, -1, -1, -1, -1}},
    {"READ, WRITE", {"AUTO: read the performance and the", "internal patch names (MIDI OUT to the",
                     "Maschine). REC > TO JV-880 (twice):", "write it into I01-I16.",
                     "Selecting this synth sets Performance", "mode. The pads play the part's channel."},
     {BTN_AUTO, BTN_REC, -1, -1, -1, -1, -1, -1}},
    {"IMPORT", {"CONTROL > IMPORT (twice): the unit's", "performances I01-I16 into the library as",
                "001-016 (protected), replacing the", "earlier import. Browse them with the",
                "main encoder (BROWSE). Your own saves", "start from 100."},
     {BTN_CONTROL, BTN_BROWSE, BTN_REC, -1, -1, -1, -1, -1}},
};

const synth_t synth_jv880p = {
    .name = "JV-880 PERF",
    .device = "JV-880",
    .entry = "PERF",
    .key = "jv880p",
    .patch_dir = STORAGE_ROOT "/jv880p",
    .default_patch = "000 Init",
    .first_user_patch = FIRST_USER_PATCH,
    .pages = pages,
    .page_count = sizeof(pages) / sizeof(pages[0]),
    .settings = settings,
    .setting_count = sizeof(settings) / sizeof(settings[0]),
    .param_name = param_name,
    .param_format = param_format,
    .param_bar = param_bar,
    .param_on = param_on,
    .param_units = param_units,
    .param_step = param_step,
    .param_toggle = param_toggle,
    .param_reset = param_reset,
    .param_draw = param_draw,
    .page_list = page_list,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .revert = revert,
    .unknown_count = unknown_count,
    .keep_state = true,  /* selecting it reads the unit's performance, sends nothing */
    .activate = activate,
    .activate_ms = 150,
    .volume_cc = 7,
    .volume_setting = SETTING_VOLUME_JV,
    .play_channel = play_channel,
    .instance_step = part_step,
    .shift_group = shift_group,
    .shift_group_led = shift_group_led,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_perf,
    .sysex_in = sysex_in,
    .tick = tick,
    .overview_pages = 1,
    .overview_draw = overview_draw,
    .action_name = "IMPORT",
    .action = import_start,
    .action_setting = SYNTH_NONE,
    .unit_store_label = "TO JV-880",
    .unit_store_slots = 16,
    .unit_slot_name = unit_slot_name,
    .unit_slot_info = unit_slot_info,
    .unit_store = unit_store,
    .theme_bright = {127, 90, 0},  /* amber, as the other JV-880 entries */
    .theme_dim = {14, 9, 0},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
