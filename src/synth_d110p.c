/*
 * Roland D-110, patch level: the eight parts' timbres (which tone, key
 * shift...), their level, pan, key range, MIDI channel and partial reserve,
 * and the reverb. The tones themselves are edited by synth_d110.c ("D-110
 * TONE"); both are the same unit, picked separately in the synth list.
 *
 * A patch file is the D-110's own Patch Memory layout (128 bytes, manual
 * p. 118), so patches read from the unit are stored as they come. Edits
 * go out as DT1s to where the unit keeps the current patch: the System
 * Area (reverb, reserve, channels, name) and each part's Timbre Temporary
 * Area. Like the tone level, it works without a MIDI IN cable: values are
 * "--" until a patch is loaded or read back (Auto).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "d110_common.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "midi.h"
#include "storage.h"
#include "buttons.h"
#include "synth.h"

static const char *TAG = "d110p";

#define MODEL_ID 0x16
#define PARTS 8
#define PATCH_LEN 0x80     /* Total size 00 01 00: 128 bytes (7-bit addresses) */
/* Addresses (7-bit, as three bytes) */
#define SYSTEM_AREA 0x100000
#define TIMBRE_TEMP 0x030000      /* part n: + n * 0x10, rhythm part 03 01 00 */
#define PATCH_MEMORY 0x060000     /* patch n: + n * 0x80 (06 01 00 = #2) */

/* Patch Memory layout */
#define M_NAME 0x00
#define M_REVERB_MODE 0x0A
#define M_REVERB_TIME 0x0B
#define M_REVERB_LEVEL 0x0C
#define M_RESERVE 0x0D   /* parts 1-8, rhythm */
#define M_CHANNEL 0x16   /* parts 1-8, rhythm; 16 = off */
#define M_PART 0x1F      /* 12 bytes per part */
#define PART_LEN 12
#define M_RHYTHM_LEVEL 0x7F

/* ---- parameters ---- */

typedef enum { K_NUM, K_SIGNED, K_LIST, K_NOTE, K_CHANNEL, K_TONE } kind_t;

typedef struct {
    const char *name;
    int8_t offset;    /* in the patch (common) or in a part's 12 bytes */
    bool per_part;
    uint8_t max, center;
    kind_t kind;
    const char *const *labels;
} param_t;

static const char *const group_labels[] = {"a", "b", "i", "r"};
/* Manual p. 44: single assign = a repeated key cuts its previous note,
 * multi = they overlap; last/first note priority = who gives way when
 * voices run out */
static const char *const assign_labels[] = {"SNGL LAST", "SNGL FRST", "MULT LAST", "MULT FRST"};
static const char *const output_labels[] = {"MIX", "MIX", "MULTI 1", "MULTI 2", "MULTI 3",
                                             "MULTI 4", "MULTI 5", "MULTI 6"};
static const char *const reverb_labels[] = {"ROOM 1", "ROOM 2", "HALL 1", "HALL 2", "PLATE",
                                            "TAP DLY 1", "TAP DLY 2", "TAP DLY 3", "OFF"};

enum {
    P_GROUP, P_NUMBER, P_KEY_SHIFT, P_FINE, P_BENDER, P_ASSIGN, P_OUTPUT,
    P_LEVEL, P_PAN, P_KEY_LOW, P_KEY_HIGH, P_CHANNEL, P_RESERVE,
    P_REVERB_MODE, P_REVERB_TIME, P_REVERB_LEVEL, P_RHYTHM_LEVEL,
    P_RES1, P_RES2, P_RES3, P_RES4, P_RES5, P_RES6, P_RES7, P_RES8,  /* overview */
    PARAM_COUNT
};

static const param_t params[PARAM_COUNT] = {
    [P_GROUP] = {"TONE GROUP", 0, true, 3, 0, K_LIST, group_labels},
    [P_NUMBER] = {"TONE", 1, true, 63, 0, K_TONE},
    [P_KEY_SHIFT] = {"KEY SHIFT", 2, true, 48, 24, K_SIGNED},
    [P_FINE] = {"FINE TUNE", 3, true, 100, 50, K_SIGNED},
    [P_BENDER] = {"BEND RANGE", 4, true, 24, 0, K_NUM},
    [P_ASSIGN] = {"ASSIGN", 5, true, 3, 0, K_LIST, assign_labels},
    [P_OUTPUT] = {"OUTPUT", 6, true, 7, 0, K_LIST, output_labels},
    [P_LEVEL] = {"LEVEL", 8, true, 100, 0, K_NUM},
    [P_PAN] = {"PAN", 9, true, 14, 7, K_SIGNED},
    [P_KEY_LOW] = {"KEY LOW", 10, true, 127, 0, K_NOTE},
    [P_KEY_HIGH] = {"KEY HIGH", 11, true, 127, 0, K_NOTE},
    [P_CHANNEL] = {"MIDI CH", -1, true, 16, 0, K_CHANNEL},
    [P_RESERVE] = {"RESERVE", -2, true, 32, 0, K_NUM},
    [P_REVERB_MODE] = {"REVERB", M_REVERB_MODE, false, 8, 0, K_LIST, reverb_labels},
    [P_REVERB_TIME] = {"REV TIME", M_REVERB_TIME, false, 7, 0, K_NUM},
    [P_REVERB_LEVEL] = {"REV LEVEL", M_REVERB_LEVEL, false, 7, 0, K_NUM},
    [P_RHYTHM_LEVEL] = {"RHYTHM LVL", M_RHYTHM_LEVEL, false, 100, 0, K_NUM},
    [P_RES1] = {"RES 1", M_RESERVE + 0, false, 32, 0, K_NUM},
    [P_RES2] = {"RES 2", M_RESERVE + 1, false, 32, 0, K_NUM},
    [P_RES3] = {"RES 3", M_RESERVE + 2, false, 32, 0, K_NUM},
    [P_RES4] = {"RES 4", M_RESERVE + 3, false, 32, 0, K_NUM},
    [P_RES5] = {"RES 5", M_RESERVE + 4, false, 32, 0, K_NUM},
    [P_RES6] = {"RES 6", M_RESERVE + 5, false, 32, 0, K_NUM},
    [P_RES7] = {"RES 7", M_RESERVE + 6, false, 32, 0, K_NUM},
    [P_RES8] = {"RES 8", M_RESERVE + 7, false, 32, 0, K_NUM},
};

#define _ SYNTH_NONE
enum { PG_TIMBRE, PG_PART, PG_COMMON, PG_RESERVE };
static const synth_page_t pages[] = {
    [PG_TIMBRE] = {0, "TIMBRE", {P_GROUP, P_NUMBER, P_KEY_SHIFT, P_FINE, P_BENDER, P_ASSIGN, P_OUTPUT, _},
                   {_, _, _, _, _, _, _, _}},
    [PG_PART] = {1, "PART", {P_LEVEL, P_PAN, P_KEY_LOW, P_KEY_HIGH, P_CHANNEL, P_RESERVE, _, _},
                 {_, _, _, _, _, _, _, _}},
    /* The whole patch's (the rhythm part's level is on D-110 RHYTHM) */
    [PG_COMMON] = {2, "COMMON", {P_REVERB_MODE, P_REVERB_TIME, P_REVERB_LEVEL, _, _, _, _, _},
                   {_, _, _, _, _, _, _, _}},
    /* All parts at once: each column a part, its reserve on the knob */
    [PG_RESERVE] = {3, "PARTIAL RESERVE", {P_RES1, P_RES2, P_RES3, P_RES4, P_RES5, P_RES6, P_RES7, P_RES8},
                    {_, _, _, _, _, _, _, _}},
};
#undef _

#define SETTING_UNIT 1
static const synth_setting_t settings[] = {
    /* Pads play part n on this + n - 1 until the patch's channels are known */
    [SETTING_CHANNEL] = {"PART 1 CH", SET_CHANNEL, 0, 15, 1},
    [SETTING_UNIT] = {"UNIT #", SET_NUMBER, 17, 32, 17},
};

/* ---- state ---- */

static uint8_t patch[PATCH_LEN];
static bool known[PATCH_LEN];
/* The patch as read from the unit, kept at the first edit after the read
 * (for Rec + Erase) */
static uint8_t as_read[PATCH_LEN];
static bool as_read_known[PATCH_LEN], as_read_ok, read_stale;
static int part;
static bool read_asked;

static int device_id(void) { return synth_setting(SETTING_UNIT) - 1; }

/* Byte of p in the patch (for the current part) */
static int byte_of(int p)
{
    const param_t *pp = &params[p];
    if (!pp->per_part) {
        return pp->offset;
    }
    if (pp->offset == -1) {
        return M_CHANNEL + part;
    }
    if (pp->offset == -2) {
        return M_RESERVE + part;
    }
    return M_PART + part * PART_LEN + pp->offset;
}

static int addr7(int a3) { return (a3 >> 16 & 0x7F) * 16384 + (a3 >> 8 & 0x7F) * 128 + (a3 & 0x7F); }

static void dt1(int a, const uint8_t *data, int len)
{
    uint8_t msg[10 + PATCH_LEN];
    int n = 0, sum = 0;
    msg[n++] = 0xF0;
    msg[n++] = 0x41;
    msg[n++] = device_id();
    msg[n++] = MODEL_ID;
    msg[n++] = 0x12;
    msg[n++] = a >> 14 & 0x7F;
    msg[n++] = a >> 7 & 0x7F;
    msg[n++] = a & 0x7F;
    for (int i = 0; i < len; i++) {
        msg[n++] = data[i] & 0x7F;
    }
    for (int i = 5; i < n; i++) {
        sum += msg[i];
    }
    msg[n++] = (128 - (sum & 0x7F)) & 0x7F;
    msg[n++] = 0xF7;
    midi_send_sysex(msg, n);
}

static void rq1(int a, int size)
{
    uint8_t msg[] = {0xF0, 0x41, device_id(), MODEL_ID, 0x11, a >> 14 & 0x7F, a >> 7 & 0x7F, a & 0x7F,
                     size >> 14 & 0x7F, size >> 7 & 0x7F, size & 0x7F, 0, 0xF7};
    int sum = 0;
    for (int i = 5; i < 11; i++) {
        sum += msg[i];
    }
    msg[11] = (128 - (sum & 0x7F)) & 0x7F;
    midi_send_sysex(msg, sizeof(msg));
}

/* Sends patch bytes [from, from+len) to where the unit keeps them */
static void send_patch_bytes(int from, int len)
{
    for (int i = from; i < from + len;) {
        int a, n;
        if (i < M_REVERB_MODE) {                 /* name -> System 17-20 */
            a = addr7(SYSTEM_AREA) + 0x17 + (i - M_NAME);
            n = M_REVERB_MODE - i;
        } else if (i < M_PART) {                 /* reverb, reserve, channels -> System 01-15 */
            a = addr7(SYSTEM_AREA) + 0x01 + (i - M_REVERB_MODE);
            n = M_PART - i;
        } else if (i < M_RHYTHM_LEVEL) {         /* part parameters -> Timbre Temporary */
            int p = (i - M_PART) / PART_LEN, off = (i - M_PART) % PART_LEN;
            a = addr7(TIMBRE_TEMP) + p * 0x10 + off;
            n = PART_LEN - off;
        } else {                                 /* rhythm part output level */
            a = addr7(TIMBRE_TEMP) + 0x80 + 8;
            n = 1;
        }
        if (n > from + len - i) {
            n = from + len - i;
        }
        dt1(a, &patch[i], n);
        i += n;
    }
}

/* ---- the tones' names, from the D-110 TONE library (001-128 = a, b) ---- */

static char tone_names[128][11];
static bool names_read;

static void read_tone_names(void)
{
    if (names_read) {
        return;
    }
    names_read = true;
    DIR *dir = opendir(STORAGE_ROOT "/d110");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        const char *sp = strchr(e->d_name, ' ');
        if (n >= 1 && n <= 128 && sp) {
            snprintf(tone_names[n - 1], sizeof(tone_names[0]), "%s", sp + 1);
        }
    }
    if (dir) {
        closedir(dir);
    }
}

/* ---- parameters ---- */

static int value(int p) { return patch[byte_of(p)]; }
static bool value_known(int p) { return known[byte_of(p)]; }

static const char *param_name(int p) { return params[p].name; }

static void format_note(int v, char *buf, size_t len)
{
    static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(buf, len, "%s%d", names[v % 12], v / 12 - 1);  /* 60 = C4, as the D-110 */
}

static void param_format(int p, char *buf, size_t len)
{
    const param_t *pp = &params[p];
    if (!value_known(p)) {
        snprintf(buf, len, "--");
        return;
    }
    int v = value(p);
    switch (pp->kind) {
    case K_SIGNED:
        snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        break;
    case K_LIST:
        snprintf(buf, len, "%s", pp->labels[v <= pp->max ? v : 0]);
        break;
    case K_NOTE:
        format_note(v, buf, len);
        break;
    case K_CHANNEL:
        snprintf(buf, len, v >= 16 ? "OFF" : "%d", v + 1);
        break;
    case K_TONE:
        snprintf(buf, len, "%d", v + 1);
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    if (!value_known(p) || params[p].kind == K_LIST || params[p].kind == K_TONE) {
        return -1;
    }
    return value(p) * 1000 / params[p].max;
}

static bool param_on(int p) { return false; }

static int param_units(int p)
{
    int max = params[p].max;
    return max <= 3 ? 70 : max <= 8 ? 50 : max <= 16 ? 40 : max <= 48 ? 20 : 13;
}

static void set_value(int p, int v)
{
    if (read_stale) {
        memcpy(as_read, patch, PATCH_LEN);
        memcpy(as_read_known, known, PATCH_LEN);
        as_read_ok = true;
        read_stale = false;
    }
    int at = byte_of(p);
    patch[at] = v;
    known[at] = true;
    if (at >= M_RESERVE && at < M_RESERVE + 9) {
        /* The unit takes partial reserves only as all 9 parts together */
        send_patch_bytes(M_RESERVE, 9);
    } else {
        send_patch_bytes(at, 1);
    }
    if (p == P_GROUP || (p >= P_RES1 && p <= P_RES8) || p == P_RESERVE) {
        synth_redraw();  /* the tone's name / the reserve total depend on it */
    }
}

/* The other reserves' sum, so a reserve can't take the total over 32 */
static int reserve_others(int at)
{
    int sum = 0;
    for (int i = 0; i < 9; i++) {
        if (M_RESERVE + i != at && known[M_RESERVE + i]) {
            sum += patch[M_RESERVE + i];
        }
    }
    return sum;
}

static bool param_step(int p, int steps)
{
    int v = value_known(p) ? value(p) : params[p].center;
    int nv = v + steps, max = params[p].max;
    int at = byte_of(p);
    if (at >= M_RESERVE && at < M_RESERVE + 9) {
        int room = 32 - reserve_others(at);  /* the unit has 32 partials in all */
        max = room < 0 ? 0 : room < max ? room : max;
        if (steps > 0 && v >= max) {
            return false;
        }
    }
    nv = nv < 0 ? 0 : nv > max ? max : nv;
    if (nv == v && value_known(p)) {
        return false;
    }
    set_value(p, nv);
    return true;
}

static void param_toggle(int p) {}
static bool param_reset(int p) { return false; }

/* ---- pictures ---- */

/* Tone number: its name below (a/b from the tone library) */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    if (p != P_NUMBER || !value_known(p) || !known[byte_of(P_GROUP)]) {
        return SYNTH_DRAW_NONE;
    }
    int group = patch[byte_of(P_GROUP)], n = value(p);
    char num[8], name[16];
    snprintf(num, sizeof(num), "%s%d%d", group_labels[group & 3], n / 8 + 1, n % 8 + 1);
    read_tone_names();
    snprintf(name, sizeof(name), "%s", group < 2 ? tone_names[group * 64 + n] : "");
    display_text(disp, x + (w - display_text_width(&font_medium, num)) / 2, y + 1 - font_medium.cap_top,
                 &font_medium, num, true);
    display_text(disp, x + (w - display_text_width(&font_small, name)) / 2, y + 15 - font_small.cap_top,
                 &font_small, name, true);
    return SYNTH_DRAW_FULL;
}

/* Reserve page: the total against the D-110's 32 partials, in the
 * (unused) switch row of both displays; inverted if over */
static void page_draw(int pg, int disp)
{
    if (pg != PG_RESERVE) {
        return;
    }
    int total = 0;
    bool all = true;
    for (int i = 0; i < 9; i++) {
        total += patch[M_RESERVE + i];
        all &= known[M_RESERVE + i];
    }
    char buf[40];
    if (!all) {
        snprintf(buf, sizeof(buf), disp ? "" : "TOTAL --");
    } else if (disp == 0) {
        snprintf(buf, sizeof(buf), "TOTAL %d / 32", total);
    } else {
        snprintf(buf, sizeof(buf), total <= 32 ? "RHYTHM %d  FREE %d" : "RHYTHM %d  OVER BY %d",
                 patch[M_RESERVE + 8], total <= 32 ? 32 - total : total - 32);
    }
    bool over = all && total > 32;
    display_fill_rect(disp, 0, 13, 255, 12, over);
    display_text(disp, 3, 15 - font_small.cap_top, &font_small, buf, !over);
}


/* ---- parts ---- */

static int play_channel(void)
{
    int ch = patch[M_CHANNEL + part];
    if (known[M_CHANNEL + part] && ch < 16) {
        return ch;
    }
    return (synth_setting(SETTING_CHANNEL) + part) & 0x0F;
}

static void instance_step(int d)
{
    part = (part + d + PARTS) % PARTS;
    char buf[32];
    snprintf(buf, sizeof(buf), "PART %d  (channel %d)", part + 1, play_channel() + 1);
    synth_notice(buf);
    synth_redraw();
}

static int instance(void) { return 0; }  /* one patch for all parts */

/* "PT2 AcouBass 1": the part and its tone (a/b by name, i by number) */
static void header_tag(char *buf, size_t len)
{
    int g = M_PART + part * PART_LEN;
    if (!known[g] || !known[g + 1]) {
        snprintf(buf, len, "PT%d", part + 1);
        return;
    }
    int group = patch[g] & 3, n = patch[g + 1] & 63;
    read_tone_names();
    if (group < 2 && tone_names[group * 64 + n][0]) {
        snprintf(buf, len, "PT%d %s", part + 1, tone_names[group * 64 + n]);
    } else {
        snprintf(buf, len, "PT%d %s%d%d", part + 1, group_labels[group], n / 8 + 1, n % 8 + 1);
    }
}

static bool sound_name(char *buf, size_t len)
{
    if (!known[M_NAME]) {
        return false;
    }
    char name[11];
    memcpy(name, &patch[M_NAME], 10);
    name[10] = 0;
    for (int i = 9; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    snprintf(buf, len, "%s", name);
    return true;
}

/* ---- reading back (optional, needs the D-110's MIDI OUT) ---- */

/* Import: the unit's 64 patches into the library (001-064) */
#define IMPORT_COUNT 64
#define IMPORT_TIMEOUT_US 1000000
static bool importing;
static int imp_index, imp_saved;
static int64_t imp_deadline;

static void read_state(void)
{
    if (importing) {
        importing = false;
        synth_patches_changed();
        synth_notice("IMPORT stopped");
        return;
    }
    read_asked = true;
    rq1(addr7(SYSTEM_AREA), 0x21);
    rq1(addr7(TIMBRE_TEMP), 0x80 + 0x10);
}

/* ---- writing into the unit's own memory (Store: TO D-110) ----
 * Write Request (40 02 00): the patch as now in the temporary areas into
 * internal patch memory (I-11..I-88); the unit answers at 40 10 00 */
#define WRITE_RESULT (0x40 * 16384 + 0x10 * 128)

static void unit_slot_name(int slot, char *buf, size_t len)
{
    snprintf(buf, len, "I-%d%d", slot / 8 + 1, slot % 8 + 1);
}

static void unit_store(int slot, const char *name)
{
    for (int i = 0; i < 10; i++) {
        char c = *name ? *name++ : ' ';
        patch[M_NAME + i] = c >= 32 && c < 127 ? c : ' ';
        known[M_NAME + i] = true;
    }
    send_patch_bytes(M_NAME, 10);
    uint8_t req[2] = {slot & 63, 0};  /* slot, internal */
    dt1(0x40 * 16384 + 0x02 * 128, req, 2);
    char slot_name[8], buf[40];
    unit_slot_name(slot, slot_name, sizeof(slot_name));
    snprintf(buf, sizeof(buf), "WRITING PATCH TO %s", slot_name);
    synth_notice(buf);
}

static void import_next(void)
{
    imp_deadline = esp_timer_get_time() + IMPORT_TIMEOUT_US;
    rq1(addr7(PATCH_MEMORY) + imp_index * PATCH_LEN, PATCH_LEN);
}

static void import_start(void)
{
    if (importing) {
        return;
    }
    DIR *dir = opendir(STORAGE_ROOT "/d110p");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        if (n >= 1 && n <= IMPORT_COUNT) {
            char path[300];
            snprintf(path, sizeof(path), STORAGE_ROOT "/d110p/%s", e->d_name);
            remove(path);
        }
    }
    if (dir) {
        closedir(dir);
    }
    mkdir(STORAGE_ROOT "/d110p", 0777);  /* fails harmlessly if it exists */
    importing = true;
    imp_index = imp_saved = 0;
    synth_notice("IMPORT: asking the D-110...");
    import_next();
}

static void import_save(const uint8_t *data)
{
    char name[11];
    memcpy(name, data, 10);
    name[10] = 0;
    for (int i = 9; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    for (char *c = name; *c; c++) {
        if (*c == '/' || *c < 32 || *c > 126) {
            *c = '_';
        }
    }
    char path[96];
    snprintf(path, sizeof(path), STORAGE_ROOT "/d110p/%03d %s", imp_index + 1, name[0] ? name : "Patch");
    FILE *f = fopen(path, "wb");
    if (f) {
        imp_saved += fwrite(data, 1, PATCH_LEN, f) == PATCH_LEN;
        fclose(f);
    }
}

static void tick(int64_t now)
{
    if (importing && now > imp_deadline) {
        importing = false;
        synth_patches_changed();
        synth_notice(imp_index ? "IMPORT stopped: no reply" : "IMPORT: no reply - D-110 MIDI OUT connected?");
    }
}

static void sysex_in(const uint8_t *d, size_t len)
{
    if (len < 11 || d[1] != 0x41 || d[2] != device_id() || d[3] != MODEL_ID || d[4] != 0x12) {
        return;
    }
    int sum = 0;
    for (size_t i = 5; i < len - 1; i++) {
        sum += d[i];
    }
    if (sum & 0x7F) {
        return;
    }
    int a = d[5] << 14 | d[6] << 7 | d[7], n = (int)len - 10;
    const uint8_t *data = d + 8;
    if (a == WRITE_RESULT) {
        static const char *const results[] = {"WRITTEN TO D-110", "WRITE: CARD NOT READY",
                                              "WRITE: MEMORY PROTECTED", "WRITE: WRONG MODE"};
        synth_notice(results[data[0] & 3]);
        return;
    }
    if (importing && a == addr7(PATCH_MEMORY) + imp_index * PATCH_LEN && n == PATCH_LEN) {
        import_save(data);
        char buf[32];
        snprintf(buf, sizeof(buf), "IMPORT %d / %d", imp_index + 1, IMPORT_COUNT);
        synth_notice(buf);
        if (++imp_index >= IMPORT_COUNT) {
            importing = false;
            synth_patches_changed();
            snprintf(buf, sizeof(buf), "IMPORTED %d PATCHES", imp_saved);
            synth_notice(buf);
        } else {
            import_next();
        }
        return;
    }
    bool got = false;
    for (int i = 0; i < n; i++) {
        int at = -1, off;
        if ((off = a + i - addr7(SYSTEM_AREA)) >= 0 && off < 0x21) {
            if (off >= 0x01 && off <= 0x15) {
                at = M_REVERB_MODE + off - 0x01;
            } else if (off >= 0x17 && off <= 0x20) {
                at = M_NAME + off - 0x17;
            }
        } else if ((off = a + i - addr7(TIMBRE_TEMP)) >= 0 && off < 0x90) {
            if (off < 0x80 && off % 0x10 < PART_LEN) {
                at = M_PART + off / 0x10 * PART_LEN + off % 0x10;
            } else if (off == 0x80 + 8) {
                at = M_RHYTHM_LEVEL;
            }
        }
        if (at >= 0) {
            patch[at] = data[i];
            known[at] = true;
            read_stale = true;
            got = true;
        }
    }
    if (got) {
        synth_state_received();  /* the unit answered: no fallback */
        uint16_t mask = 0;
        for (int i = 0; i < 9; i++) {
            if (known[M_CHANNEL + i] && patch[M_CHANNEL + i] < 16) {
                mask |= 1 << patch[M_CHANNEL + i];
            }
        }
        d110_set_channels(mask);  /* Volume goes to these */
        if (read_asked && known[M_PART]) {
            read_asked = false;
            synth_sound_replaced();
        }
        synth_redraw();
    }
}

/* ---- patches ---- */

static bool patch_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fread(patch, 1, PATCH_LEN, f) == PATCH_LEN;
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    memset(known, 1, sizeof(known));
    read_stale = as_read_ok = false;
    send_patch_bytes(0, PATCH_LEN);
    return true;
}

/* Rec + Erase on the unit's own patch: back to how it was read (only the
 * bytes that were) */
static bool revert(void)
{
    if (!as_read_ok) {
        return false;
    }
    memcpy(patch, as_read, PATCH_LEN);
    memcpy(known, as_read_known, PATCH_LEN);
    for (int i = 0; i < PATCH_LEN;) {
        int j = i;
        while (j < PATCH_LEN && known[j]) {
            j++;
        }
        if (j > i) {
            send_patch_bytes(i, j - i);
        }
        i = j + 1;
    }
    return true;
}

static bool patch_save(const char *path)
{
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    title = sp ? sp + 1 : title;
    for (int i = 0; i < 10; i++) {
        char c = *title ? *title++ : ' ';
        patch[M_NAME + i] = c >= 32 && c < 127 ? c : ' ';
        known[M_NAME + i] = true;
    }
    send_patch_bytes(M_NAME, 10);
    mkdir(STORAGE_ROOT "/d110p", 0777);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(patch, 1, PATCH_LEN, f) == PATCH_LEN;
    fclose(f);
    return ok;
}

static int unknown_count(void)
{
    int n = 0;
    for (int i = 0; i < PATCH_LEN; i++) {
        n += !known[i];
    }
    return n;
}

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"PATCH LEVEL", {"What each part plays (its tone), level,", "pan, key range, channel and partial",
                     "reserve; the reverb. STEP arrows: part.", "D: all parts' reserves against the",
                     "D-110's 32 partials - a part with 0", "still plays if partials are free.",
                     "Patches are setups, not sounds."},
     {BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_GROUP_A + 3, -1, -1, -1, -1, -1}},
    {"READ, IMPORT, WRITE", {"AUTO: read the D-110's current patch.", "CONTROL > IMPORT: its 64 patches",
                             "into the library (001-064).", "REC > TO D-110 (twice): write the patch",
                             "into its memory, I-11..I-88."},
     {BTN_AUTO, BTN_CONTROL, BTN_REC, -1, -1, -1, -1, -1}},
    {"VOLUME", {"Hold VOLUME and turn the main encoder:", "Main Volume on all the D-110's parts."},
     {BTN_VOLUME, -1, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_d110p = {
    .name = "D-110 PATCH",
    .key = "d110p",
    .patch_dir = STORAGE_ROOT "/d110p",
    .default_patch = "",
    .first_user_patch = 100,  /* 001-064: imported from the unit (Setup: IMPORT) */
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
    .param_draw = param_draw,
    .param_reset = param_reset,
    .patch_load = patch_load,
    .patch_save = patch_save,
    .revert = revert,
    .unknown_count = unknown_count,
    .page_draw = page_draw,
    .play_channel = play_channel,
    .instance_step = instance_step,
    .instance = instance,
    .header_tag = header_tag,
    .sound_name = sound_name,
    .read = read_state,
    .sysex_in = sysex_in,
    .tick = tick,
    .unit_store_label = "TO D-110",
    .unit_store_slots = 64,
    .unit_slot_name = unit_slot_name,
    .unit_store = unit_store,
    .action_name = "IMPORT",
    .action = import_start,
    .volume_get = d110_volume_get,
    .volume_set = d110_volume_set,
    .keep_state = true,
    .theme_bright = {0, 100, 127},  /* cyan, as the tone level */
    .theme_dim = {0, 9, 12},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
