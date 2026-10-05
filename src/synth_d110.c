/*
 * Roland D-110: tone editing (LA synthesis: a tone = common part + four
 * partials), laid out like Roland's own PG-10 programmer: Group A-H = its
 * eight parameter groups, Shift + Group A-D picks the partials being edited
 * (several at once, like the PG-10's partial select), the transport Step
 * arrows pick the part (1-8) whose tone is edited and played.
 *
 * Every change goes out at once as a DT1 to the part's Tone Temporary Area
 * (manual p. 117). Nothing is read back - the editor works without a MIDI
 * IN cable: a part's values are "--" until a tone is loaded into it or the
 * parameter is changed.
 *
 * A patch file is a tone: its 246 bytes as in the Tone Temporary Area.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d110_common.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "midi.h"
#include "storage.h"
#include "buttons.h"
#include "synth.h"

static const char *TAG = "d110";

#define TONE_LEN 0xF6         /* 14 common + 4 x 58 partial */
#define COMMON_LEN 0x0E
#define PARTIAL_LEN 0x3A
#define TEMP_TONE_BASE 0x040000  /* part 1; parts 0xF6 apart (7-bit address) */
#define TEMP_TIMBRE_BASE 0x030000  /* part 1; parts 0x10 apart */
#define WRITE_REQUEST 0x400000     /* manual p. 120 */
#define WRITE_RESULT (0x40 * 16384 + 0x10 * 128)  /* 40 10 00 */
#define MODEL_ID 0x16
#define PARTS 8

/* Common */
#define C_STRUCT12 0x0A
#define C_STRUCT34 0x0B
#define C_MUTE 0x0C  /* bit n = partial n+1 sounds */
#define C_ENV_MODE 0x0D

/* ---- parameters ---- */

typedef enum {
    K_NUM,     /* 0..max as is */
    K_SIGNED,  /* value - center */
    K_NOTE,    /* WG pitch coarse: 0 = C1 */
    K_KEYF,    /* key follow ratios */
    K_BIAS,    /* bias point: <A1..<C7, >A1..>C7 */
    K_LIST,
    K_PCM,     /* PCM wave 0-127 shown 1-128 */
    K_SWITCH,
    K_STRUCT,  /* 0-12 shown 1-13 */
    K_EG,      /* envelope time/level: value only, the envelope is drawn */
} kind_t;

typedef struct {
    const char *name;
    int16_t offset;  /* in the common part, or in each partial (P_ flag) */
    uint8_t max, center;
    kind_t kind;
    bool synth_only;  /* not used by PCM partials (manual p. 99) */
    const char *const *labels;
    bool pcm_only;    /* not used by synth partials */
} d110_param_t;

static const char *const keyf_labels[] = {"-1", "-1/2", "-1/4", "0", "1/8", "1/4", "3/8", "1/2",
                                          "5/8", "3/4", "7/8", "1", "5/4", "3/2", "2", "s1", "s2"};
/* PCM sound names (manual p. 103-104), shortened to fit a column; L =
 * looped. Bank 2: the drums again (1-30), Loop-1..64, Jam-1..34. */
static const char *const pcm_bank1[128] = {
    "BassDrum 1", "BassDrum 2", "BassDrum 3", "Snare 1", "Snare 2", "Snare 3", "Snare 4", "Tom 1",
    "Tom 2", "Hi-Hat", "Hi-Hat L", "Crash 1", "Crash 2 L", "Ride 1", "Ride 2 L", "Cup", "China 1",
    "China 2 L", "Rim Shot", "Hand Clap", "Mute Conga", "Conga", "Bongo", "Cowbell", "Tambourine",
    "Agogo", "Claves", "Timbale Hi", "Timbale Lo", "Cabasa", "TimpAttack", "Timpani", "Piano High",
    "Piano Low", "PianoThump", "Organ Perc", "Trumpet", "Lips", "Trombone", "Clarinet",
    "Flute High", "Flute Low", "Steamer", "Ind. Flute", "Breath", "Vibes High", "Vibes Low",
    "Marimba", "Xylo High", "Xylo Low", "Kalimba", "Wind Bell", "Chime Bar", "Hammer", "Guiro",
    "Chink", "Nails", "Fretless", "Pull Bass", "Slap Bass", "Thump Bass", "Ac. Bass", "El. Bass",
    "Gut Guitar", "Steel Gtr", "Dirty Gtr", "Pizzicato", "Harp", "Contrabass", "Cello", "Violin 1",
    "Violin 2", "Koto", "Drawbars L", "Hi Organ L", "Lo Organ L", "Trumpet L", "Trombone L",
    "Sax 1 L", "Sax 2 L", "Reed L", "SlapBass L", "Ac.Bass L", "El.Bass1 L", "El.Bass2 L",
    "Gut Gtr L", "SteelGtr L", "El. Gtr L", "Clav L", "Cello L", "Violin L", "E.Piano1 L",
    "E.Piano2 L", "Harpsi1 L", "Harpsi2 L", "TelBell L", "F.Voice1 L", "F.Voice2 L", "M.Voice1 L",
    "M.Voice2 L", "Spectrum1", "Spectrum2", "Spectrum3", "Spectrum4", "Spectrum5", "Spectrum6",
    "Spectrum7", "Spectrum8", "Spectrum9", "Spectrum10", "Noise L", "Shot 1", "Shot 2", "Shot 3",
    "Shot 4", "Shot 5", "Shot 6", "Shot 7", "Shot 8", "Shot 9", "Shot 10", "Shot 11", "Shot 12",
    "Shot 13", "Shot 14", "Shot 15", "Shot 16", "Shot 17",
};

static void pcm_name(int bank, int n, char *buf, size_t len)
{
    if (bank == 0 || n < 30) {
        snprintf(buf, len, "%s", pcm_bank1[n & 127]);
    } else if (n < 94) {
        snprintf(buf, len, "Loop %d", n - 29);
    } else {
        snprintf(buf, len, "Jam %d", n - 93);
    }
}

static const char *const wave_labels[] = {"SQU", "SAW"};
static const char *const bank_labels[] = {"1", "2"};
static const char *const env_mode_labels[] = {"NORMAL", "NO SUS"};

/* Common parameters, then partial parameters (offset within a partial) */
enum {
    P_STRUCT12, P_STRUCT34, P_ENV_MODE,
    P_ON1, P_ON2, P_ON3, P_ON4,  /* partial mute bits, as switches */
    COMMON_PARAMS,
    /* partial */
    P_COARSE = COMMON_PARAMS, P_FINE, P_KEYF, P_BENDER, P_WAVE, P_BANK, P_PCM, P_PW, P_PW_VELO,
    P_PENV_DEPTH, P_PENV_VELO, P_PENV_TKF, P_PT1, P_PT2, P_PT3, P_PT4, P_PL0, P_PL1, P_PL2,
    P_PEND, P_LFO_RATE, P_LFO_DEPTH, P_LFO_MOD,
    P_CUTOFF, P_RESO, P_FKEYF, P_FBIAS_PT, P_FBIAS_LVL, P_FENV_DEPTH, P_FENV_VELO, P_FENV_DKF,
    P_FENV_TKF, P_FT1, P_FT2, P_FT3, P_FT4, P_FT5, P_FL1, P_FL2, P_FL3, P_FSUS,
    P_LEVEL, P_AVELO, P_ABP1, P_ABL1, P_ABP2, P_ABL2, P_AENV_TKF, P_AT1_VFOL,
    P_AT1, P_AT2, P_AT3, P_AT4, P_AT5, P_AL1, P_AL2, P_AL3, P_ASUS,
    PARAM_COUNT
};

/* Waveform and PCM bank share one byte (0x04): bit 0 = SAW, bit 1 = bank 2 */
#define WAVE_BANK_OFFSET 0x04

static const d110_param_t params[PARAM_COUNT] = {
    [P_STRUCT12] = {"STRUCT 1&2", C_STRUCT12, 12, 0, K_STRUCT},
    [P_STRUCT34] = {"STRUCT 3&4", C_STRUCT34, 12, 0, K_STRUCT},
    [P_ENV_MODE] = {"ENV MODE", C_ENV_MODE, 1, 0, K_LIST, false, env_mode_labels},
    [P_ON1] = {"P1 ON", C_MUTE, 1, 0, K_SWITCH},
    [P_ON2] = {"P2 ON", C_MUTE, 1, 0, K_SWITCH},
    [P_ON3] = {"P3 ON", C_MUTE, 1, 0, K_SWITCH},
    [P_ON4] = {"P4 ON", C_MUTE, 1, 0, K_SWITCH},

    [P_COARSE] = {"COARSE", 0x00, 96, 0, K_NOTE},
    [P_FINE] = {"FINE", 0x01, 100, 50, K_SIGNED},
    [P_KEYF] = {"KEY FOLLOW", 0x02, 16, 0, K_KEYF},
    [P_BENDER] = {"BENDER", 0x03, 1, 0, K_SWITCH},
    [P_WAVE] = {"WAVE", WAVE_BANK_OFFSET, 1, 0, K_LIST, true, wave_labels},
    [P_BANK] = {"PCM BANK", WAVE_BANK_OFFSET, 1, 0, K_LIST, false, bank_labels, true},
    [P_PCM] = {"PCM WAVE", 0x05, 127, 0, K_PCM, false, NULL, true},
    [P_PW] = {"PULSE W", 0x06, 100, 0, K_NUM, true},
    [P_PW_VELO] = {"PW VELO", 0x07, 14, 7, K_SIGNED, true},
    [P_PENV_DEPTH] = {"PENV DEPTH", 0x08, 10, 0, K_NUM},
    [P_PENV_VELO] = {"PENV VELO", 0x09, 3, 0, K_NUM},
    [P_PENV_TKF] = {"PENV TKF", 0x0A, 4, 0, K_NUM},
    [P_PT1] = {"T1", 0x0B, 100, 0, K_EG},
    [P_PT2] = {"T2", 0x0C, 100, 0, K_EG},
    [P_PT3] = {"T3", 0x0D, 100, 0, K_EG},
    [P_PT4] = {"T4", 0x0E, 100, 0, K_EG},
    [P_PL0] = {"L0", 0x0F, 100, 50, K_EG},
    [P_PL1] = {"L1", 0x10, 100, 50, K_EG},
    [P_PL2] = {"L2", 0x11, 100, 50, K_EG},
    [P_PEND] = {"END", 0x13, 100, 50, K_EG},
    [P_LFO_RATE] = {"LFO RATE", 0x14, 100, 0, K_NUM},
    [P_LFO_DEPTH] = {"LFO DEPTH", 0x15, 100, 0, K_NUM},
    [P_LFO_MOD] = {"MOD SENS", 0x16, 100, 0, K_NUM},
    [P_CUTOFF] = {"CUTOFF", 0x17, 100, 0, K_NUM, true},
    [P_RESO] = {"RESONANCE", 0x18, 30, 0, K_NUM, true},
    [P_FKEYF] = {"KEY FOLLOW", 0x19, 14, 0, K_KEYF, true},
    [P_FBIAS_PT] = {"BIAS POINT", 0x1A, 127, 0, K_BIAS, true},
    [P_FBIAS_LVL] = {"BIAS LEVEL", 0x1B, 14, 7, K_SIGNED, true},
    [P_FENV_DEPTH] = {"ENV DEPTH", 0x1C, 100, 0, K_NUM, true},
    [P_FENV_VELO] = {"ENV VELO", 0x1D, 100, 0, K_NUM, true},
    [P_FENV_DKF] = {"DEPTH KF", 0x1E, 4, 0, K_NUM, true},
    [P_FENV_TKF] = {"TKF", 0x1F, 4, 0, K_EG, true},
    [P_FT1] = {"T1", 0x20, 100, 0, K_EG, true},
    [P_FT2] = {"T2", 0x21, 100, 0, K_EG, true},
    [P_FT3] = {"T3", 0x22, 100, 0, K_EG, true},
    [P_FT4] = {"T4", 0x23, 100, 0, K_EG, true},
    [P_FT5] = {"T5", 0x24, 100, 0, K_EG, true},
    [P_FL1] = {"L1", 0x25, 100, 0, K_EG, true},
    [P_FL2] = {"L2", 0x26, 100, 0, K_EG, true},
    [P_FL3] = {"L3", 0x27, 100, 0, K_EG, true},
    [P_FSUS] = {"SUS", 0x28, 100, 0, K_EG, true},
    [P_LEVEL] = {"LEVEL", 0x29, 100, 0, K_NUM},
    [P_AVELO] = {"VELOCITY", 0x2A, 100, 50, K_SIGNED},
    [P_ABP1] = {"BIAS PT 1", 0x2B, 127, 0, K_BIAS},
    [P_ABL1] = {"BIAS LVL 1", 0x2C, 12, 12, K_SIGNED},
    [P_ABP2] = {"BIAS PT 2", 0x2D, 127, 0, K_BIAS},
    [P_ABL2] = {"BIAS LVL 2", 0x2E, 12, 12, K_SIGNED},
    [P_AENV_TKF] = {"TKF", 0x2F, 4, 0, K_EG},
    [P_AT1_VFOL] = {"VFOL", 0x30, 4, 0, K_EG},  /* T1 velocity follow; on the envelope page */
    [P_AT1] = {"T1", 0x31, 100, 0, K_EG},
    [P_AT2] = {"T2", 0x32, 100, 0, K_EG},
    [P_AT3] = {"T3", 0x33, 100, 0, K_EG},
    [P_AT4] = {"T4", 0x34, 100, 0, K_EG},
    [P_AT5] = {"T5", 0x35, 100, 0, K_EG},
    [P_AL1] = {"L1", 0x36, 100, 0, K_EG},
    [P_AL2] = {"L2", 0x37, 100, 0, K_EG},
    [P_AL3] = {"L3", 0x38, 100, 0, K_EG},
    [P_ASUS] = {"SUS", 0x39, 100, 0, K_EG},
};

/* ---- pages: the PG-10's eight parameter groups ---- */

#define _ SYNTH_NONE
enum { PG_COMMON, PG_WG_PITCH, PG_WG_FORM, PG_PITCH_ENV, PG_TVF_FREQ, PG_TVF_ENV, PG_TVF_ENV2,
       PG_TVA_LEVEL, PG_TVA_ENV, PG_TVA_ENV2 };
static const synth_page_t pages[] = {
    /* Encoders 5 and 7, under the structure pictures, change them too */
    [PG_COMMON] = {0, "COMMON", {P_STRUCT12, P_STRUCT34, P_ENV_MODE, _, P_STRUCT12, _, P_STRUCT34, _},
                   {P_ON1, P_ON2, P_ON3, P_ON4, _, _, _, _}},
    [PG_WG_PITCH] = {1, "WG PITCH/MOD",
                     {P_COARSE, P_FINE, P_KEYF, P_LFO_RATE, P_LFO_DEPTH, P_LFO_MOD, _, _},
                     {_, _, _, _, _, _, P_BENDER, _}},
    [PG_WG_FORM] = {2, "WG FORM/ENV",
                    {P_WAVE, P_BANK, P_PCM, P_PW, P_PW_VELO, P_PENV_DEPTH, P_PENV_VELO, P_PENV_TKF},
                    {_, _, _, _, _, _, _, _}},
    [PG_PITCH_ENV] = {3, "PITCH ENV", {P_PT1, P_PT2, P_PT3, P_PT4, P_PL0, P_PL1, P_PL2, P_PEND},
                      {_, _, _, _, _, _, _, _}},
    [PG_TVF_FREQ] = {4, "TVF FREQ",
                     {P_CUTOFF, P_RESO, P_FKEYF, P_FBIAS_PT, P_FBIAS_LVL, P_FENV_DEPTH, P_FENV_VELO,
                      P_FENV_DKF},
                     {_, _, _, _, _, _, _, _}},
    /* Envelopes stage by stage (time, then the level it goes to), unlike
     * the PG-10, which has no room for T5 and L3: release continues on page
     * 2 with the envelope's scaling (time keyfollow, velocity) */
    [PG_TVF_ENV] = {5, "TVF ENV", {P_FT1, P_FL1, P_FT2, P_FL2, P_FT3, P_FL3, P_FT4, P_FSUS},
                    {_, _, _, _, _, _, _, _}, 1},
    [PG_TVF_ENV2] = {5, "TVF ENV", {P_FT5, P_FENV_TKF, _, _, _, _, _, _}, {_, _, _, _, _, _, _, _}, 1},
    [PG_TVA_LEVEL] = {6, "TVA LEVEL",
                      {P_LEVEL, P_AVELO, P_ABP1, P_ABL1, P_ABP2, P_ABL2, _, _},
                      {_, _, _, _, _, _, _, _}},
    [PG_TVA_ENV] = {7, "TVA ENV", {P_AT1, P_AL1, P_AT2, P_AL2, P_AT3, P_AL3, P_AT4, P_ASUS},
                    {_, _, _, _, _, _, _, _}, 1},
    [PG_TVA_ENV2] = {7, "TVA ENV", {P_AT5, P_AENV_TKF, P_AT1_VFOL, _, _, _, _, _},
                     {_, _, _, _, _, _, _, _}, 1},
};
#undef _

/* ---- settings ---- */

#define SETTING_UNIT 1
static const synth_setting_t settings[] = {
    /* Part n plays on this + n - 1 (the D-110's own default: parts on 2-9) */
    [SETTING_CHANNEL] = {"PART 1 CH", SET_CHANNEL, 0, 15, 1},
    /* Exclusive unit number (SYSTEM: Exclu Unit#), device ID = unit - 1 */
    [SETTING_UNIT] = {"UNIT #", SET_NUMBER, 17, 32, 17},
};

/* ---- state ---- */

static uint8_t tone[PARTS][TONE_LEN], init_tone[TONE_LEN];
static bool known[PARTS][TONE_LEN];
static bool init_read;
static int part;                  /* 0-7 */
static uint8_t partial_sel = 1;   /* bit n = partial n+1 edited */

static int first_partial(void)
{
    for (int i = 0; i < 4; i++) {
        if (partial_sel & (1 << i)) {
            return i;
        }
    }
    return 0;
}

static bool is_partial_param(int p) { return p >= COMMON_PARAMS; }

/* Byte offset of p in a tone, for partial i */
static int byte_of(int p, int i)
{
    return is_partial_param(p) ? COMMON_LEN + i * PARTIAL_LEN + params[p].offset : params[p].offset;
}

/* Partial i is a PCM partial in the tone's structure (manual p. 133) */
static bool partial_is_pcm(int i)
{
    /* top / bottom partial of each structure: 0 = synth, 1 = PCM */
    static const uint8_t top[13] = {0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 1};
    static const uint8_t bottom[13] = {0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1};
    int s = tone[part][i < 2 ? C_STRUCT12 : C_STRUCT34] % 13;
    return (i % 2 == 0 ? top : bottom)[s];
}

static void read_init(void)
{
    if (init_read) {
        return;
    }
    FILE *f = fopen(STORAGE_ROOT "/d110/000 Init", "rb");
    if (f) {
        init_read = fread(init_tone, 1, TONE_LEN, f) == TONE_LEN;
        fclose(f);
    }
}

/* Raw value of p for partial i (decoded for the shared wave/bank byte) */
static int raw_value(int p, int i)
{
    uint8_t b = tone[part][byte_of(p, i)];
    switch (p) {
    case P_WAVE: return b & 1;
    case P_BANK: return (b >> 1) & 1;
    case P_ON1: case P_ON2: case P_ON3: case P_ON4: return (b >> (p - P_ON1)) & 1;
    default: return b;
    }
}

static bool value_known(int p)
{
    return known[part][byte_of(p, first_partial())];
}

/* ---- sending ---- */

static int device_id(void) { return synth_setting(SETTING_UNIT) - 1; }

/* DT1 of tone bytes [from, from+len) of the current part */
static void send_bytes(int from, int len)
{
    uint8_t msg[8 + TONE_LEN + 2];
    int addr = TEMP_TONE_BASE;
    /* 7-bit address arithmetic: parts are 0xF6 bytes apart */
    int linear = part * TONE_LEN + from;
    /* TEMP_TONE_BASE is written as its three address bytes (04 00 00) */
    int a = (addr >> 16 & 0x7F) * 16384 + (addr >> 8 & 0x7F) * 128 + (addr & 0x7F) + linear;
    int n = 0;
    msg[n++] = 0xF0;
    msg[n++] = 0x41;
    msg[n++] = device_id();
    msg[n++] = MODEL_ID;
    msg[n++] = 0x12;
    msg[n++] = a >> 14 & 0x7F;
    msg[n++] = a >> 7 & 0x7F;
    msg[n++] = a & 0x7F;
    for (int i = 0; i < len; i++) {
        msg[n++] = tone[part][from + i] & 0x7F;
    }
    int sum = 0;
    for (int i = 5; i < n; i++) {
        sum += msg[i];
    }
    msg[n++] = (128 - (sum & 0x7F)) & 0x7F;
    msg[n++] = 0xF7;
    midi_send_sysex(msg, n);
}

/* ---- reading back (optional: needs the D-110's MIDI OUT on the
 * Maschine's MIDI IN) ---- */

static bool read_asked;  /* Auto pressed: the reply replaces the part's patch */

static int temp_base(void) { return (TEMP_TONE_BASE >> 16 & 0x7F) * 16384 + (TEMP_TONE_BASE >> 8 & 0x7F) * 128; }

/* RQ1 for the current part's whole tone; the reply is a DT1 (sysex_in) */
static void request_tone(void)
{
    int a = temp_base() + part * TONE_LEN;
    uint8_t msg[] = {0xF0, 0x41, device_id(), MODEL_ID, 0x11,
                     a >> 14 & 0x7F, a >> 7 & 0x7F, a & 0x7F,
                     0x00, TONE_LEN >> 7, TONE_LEN & 0x7F, 0, 0xF7};
    int sum = 0;
    for (int i = 5; i < 11; i++) {
        sum += msg[i];
    }
    msg[11] = (128 - (sum & 0x7F)) & 0x7F;
    midi_send_sysex(msg, sizeof(msg));
}

/* ---- import: the D-110's own tones into the library ----
 * The part's timbre is pointed at each tone in turn (group a, b, i x 64),
 * the tone read back and saved as patch 001-192; then the part gets its
 * own timbre back. Needs the D-110's MIDI OUT connected. */
#define IMPORT_GROUPS 3
#define IMPORT_COUNT (IMPORT_GROUPS * 64)
#define IMPORT_SETTLE_US 80000     /* after setting the timbre, before asking */
#define IMPORT_TIMEOUT_US 1000000
typedef enum { IMP_IDLE, IMP_ORIGINAL, IMP_SET, IMP_SETTLE, IMP_REPLY } imp_stage_t;
static imp_stage_t imp_stage;
static int imp_index, imp_saved, imp_skipped;
/* The presets' contents, to skip internal tones that are just copies */
static uint32_t imp_hash[128];
static uint8_t imp_cats[1000];  /* the library's category file, by patch number */
#define CATEGORY_PATH STORAGE_ROOT "/d110/categories"

/* Categories (one per display button), guessed from the tone names at
 * import: the first matching rule wins; what's left is bells, percussion
 * and effects. Must match the order of categories[]. */
static const char *const categories[8] = {"PIANO/KEYS", "ORGAN", "STRINGS", "BRASS/WIND",
                                          "SYNTH/PAD", "BASS", "PLUCK/GTR", "BELL/PERC"};
static int category_of(const char *name)
{
    static const struct { uint8_t cat; const char *words; } rules[] = {
        {5, "FANTASY|HARMO PAN|CHORALE|GLASSES|SOUNDTRACK|ATMOSPHERE|SPACE HORN|ICE RAINS|"
            "OBOE 2002|ECHO PAN|RESO SYNTH|STEAM PAD|VIBESTRING|SYN LEAD|PAD"},
        {3, "CONTRABASS"},
        {4, "BASSOON"},
        {6, "BASS|FRETLESS"},
        {1, "PIANO|HONKY|HARPSI|CLAV|CELESTA"},
        {2, "ORGAN|ACCORDION"},
        {3, "VIOLIN|CELLO|PIZZ|STRING"},
        {4, "BRASS|TRUMPET|TROMBONE|HORN|TUBA|FLUTE|PICCOLO|RECORDER|PAN PIPE|BOTTLE|BREATH|"
            "WHISTLE|SAX|CLARINET|OBOE|HARMONICA|SHAKUHACHI"},
        {7, "GUITAR|GTR|HARP|KOTO|SHAMISEN|JAMISEN|SITAR"},
    };
    char up[16];
    size_t n = 0;
    for (; name[n] && n < sizeof(up) - 1; n++) {
        up[n] = toupper((unsigned char)name[n]);
    }
    up[n] = 0;
    if (strcmp(up, "SHO") == 0) {
        return 4;
    }
    for (size_t r = 0; r < sizeof(rules) / sizeof(rules[0]); r++) {
        const char *w = rules[r].words;
        while (*w) {
            const char *bar = strchr(w, '|');
            size_t len = bar ? (size_t)(bar - w) : strlen(w);
            char word[24];
            snprintf(word, sizeof(word), "%.*s", (int)len, w);
            if (strstr(up, word)) {
                return rules[r].cat;
            }
            w += len + (bar ? 1 : 0);
        }
    }
    return 8;
}

static uint32_t tone_hash(const uint8_t *t)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < TONE_LEN; i++) {
        h = (h ^ t[i]) * 16777619u;
    }
    return h;
}
static uint8_t imp_original[2];  /* the part's tone group and number */
static int64_t imp_deadline;

static void send_timbre(const uint8_t *group_number)
{
    int a = (TEMP_TIMBRE_BASE >> 16 & 0x7F) * 16384 + (TEMP_TIMBRE_BASE >> 8 & 0x7F) * 128 + part * 0x10;
    uint8_t msg[] = {0xF0, 0x41, device_id(), MODEL_ID, 0x12, a >> 14 & 0x7F, a >> 7 & 0x7F, a & 0x7F,
                     group_number[0], group_number[1], 0, 0xF7};
    int sum = 0;
    for (int i = 5; i < 10; i++) {
        sum += msg[i];
    }
    msg[10] = (128 - (sum & 0x7F)) & 0x7F;
    midi_send_sysex(msg, sizeof(msg));
}

static void request_timbre(void)
{
    int a = (TEMP_TIMBRE_BASE >> 16 & 0x7F) * 16384 + (TEMP_TIMBRE_BASE >> 8 & 0x7F) * 128 + part * 0x10;
    uint8_t msg[] = {0xF0, 0x41, device_id(), MODEL_ID, 0x11, a >> 14 & 0x7F, a >> 7 & 0x7F, a & 0x7F,
                     0x00, 0x00, 0x02, 0, 0xF7};
    int sum = 0;
    for (int i = 5; i < 11; i++) {
        sum += msg[i];
    }
    msg[11] = (128 - (sum & 0x7F)) & 0x7F;
    midi_send_sysex(msg, sizeof(msg));
}

static void import_finish(const char *why)
{
    if (imp_stage != IMP_ORIGINAL) {
        send_timbre(imp_original);  /* the part's own tone back */
    }
    imp_stage = IMP_IDLE;
    FILE *cf = fopen(CATEGORY_PATH, "wb");
    if (cf) {
        fwrite(imp_cats, 1, sizeof(imp_cats), cf);
        fclose(cf);
    }
    synth_patches_changed();
    char buf[48];
    if (why) {
        snprintf(buf, sizeof(buf), "%s", why);
    } else {
        snprintf(buf, sizeof(buf), "IMPORTED %d TONES (%d COPIES SKIPPED)", imp_saved, imp_skipped);
    }
    synth_notice(buf);
    ESP_LOGI(TAG, "import: %s (%d saved)", why ? why : "done", imp_saved);
    request_tone();
}

static void import_save(void)
{
    /* An internal tone that is just a copy of a preset isn't saved */
    uint32_t h = tone_hash(tone[part]);
    if (imp_index < 128) {
        imp_hash[imp_index] = h;
    } else {
        for (int k = 0; k < 128; k++) {
            if (imp_hash[k] == h) {
                imp_skipped++;
                return;
            }
        }
    }
    char name[11];
    memcpy(name, tone[part], 10);
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
    snprintf(path, sizeof(path), STORAGE_ROOT "/d110/%03d %s", imp_index + 1, name[0] ? name : "Tone");
    FILE *f = fopen(path, "wb");
    if (f) {
        imp_saved += fwrite(tone[part], 1, TONE_LEN, f) == TONE_LEN;
        fclose(f);
        imp_cats[imp_index + 1] = category_of(name);
    }
}

static void import_start(void)
{
    if (imp_stage != IMP_IDLE) {
        return;
    }
    /* Earlier imports out of the way (001-192); own patches start at 200 */
    DIR *dir = opendir(STORAGE_ROOT "/d110");
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        int n = atoi(e->d_name);
        if (n >= 1 && n <= IMPORT_COUNT) {
            char path[300];
            snprintf(path, sizeof(path), STORAGE_ROOT "/d110/%s", e->d_name);
            remove(path);
        }
    }
    if (dir) {
        closedir(dir);
    }
    /* Keep the categories of everything else (own patches) */
    memset(imp_cats, 0, sizeof(imp_cats));
    FILE *cf = fopen(CATEGORY_PATH, "rb");
    if (cf) {
        if (fread(imp_cats, 1, sizeof(imp_cats), cf) == 0) {
            memset(imp_cats, 0, sizeof(imp_cats));
        }
        fclose(cf);
    }
    for (int n = 1; n <= IMPORT_COUNT; n++) {
        imp_cats[n] = 0;
    }
    imp_index = imp_saved = imp_skipped = 0;
    imp_stage = IMP_ORIGINAL;
    imp_deadline = esp_timer_get_time() + IMPORT_TIMEOUT_US;
    request_timbre();
    synth_notice("IMPORT: asking the D-110...");
}

static void tick(int64_t now)
{
    switch (imp_stage) {
    case IMP_SET: {
        uint8_t gn[2] = {imp_index / 64, imp_index % 64};
        send_timbre(gn);
        imp_stage = IMP_SETTLE;
        imp_deadline = now + IMPORT_SETTLE_US;
        break;
    }
    case IMP_SETTLE:
        if (now > imp_deadline) {
            request_tone();
            imp_stage = IMP_REPLY;
            imp_deadline = now + IMPORT_TIMEOUT_US;
        }
        break;
    case IMP_ORIGINAL:
    case IMP_REPLY:
        if (now > imp_deadline) {
            import_finish(imp_stage == IMP_ORIGINAL ? "IMPORT: no reply - D-110 MIDI OUT connected?"
                                                    : "IMPORT stopped: no reply");
        }
        break;
    default:
        break;
    }
}

static void sysex_in(const uint8_t *d, size_t len)
{
    /* DT1 from this unit: F0 41 dev 16 12 a a a data... sum F7 */
    if (len < 11 || d[1] != 0x41 || d[2] != device_id() || d[3] != MODEL_ID || d[4] != 0x12) {
        return;
    }
    int sum = 0;
    for (size_t i = 5; i < len - 1; i++) {
        sum += d[i];
    }
    if (sum & 0x7F) {
        ESP_LOGW(TAG, "DT1 checksum error");
        return;
    }
    int n = (int)len - 10;
    int timbre_at = (d[5] << 14 | d[6] << 7 | d[7]) -
                    ((TEMP_TIMBRE_BASE >> 16 & 0x7F) * 16384 + (TEMP_TIMBRE_BASE >> 8 & 0x7F) * 128);
    if (imp_stage == IMP_ORIGINAL && timbre_at == part * 0x10 && n >= 2) {
        imp_original[0] = d[8];
        imp_original[1] = d[9];
        imp_stage = IMP_SET;
        return;
    }
    if ((d[5] << 14 | d[6] << 7 | d[7]) == WRITE_RESULT && len >= 11) {
        static const char *const results[] = {"WRITTEN TO D-110", "WRITE: CARD NOT READY",
                                              "WRITE: MEMORY PROTECTED", "WRITE: WRONG MODE"};
        synth_notice(results[d[8] & 3]);
        return;
    }
    int at = (d[5] << 14 | d[6] << 7 | d[7]) - temp_base();
    if (at < 0 || at + n > PARTS * TONE_LEN) {
        return;  /* not a tone temporary area */
    }
    bool shown = false;
    for (int i = 0; i < n; i++) {
        int p = (at + i) / TONE_LEN, off = (at + i) % TONE_LEN;
        tone[p][off] = d[8 + i];
        known[p][off] = true;
        shown |= p == part;
    }
    if (imp_stage == IMP_REPLY) {
        if (at == part * TONE_LEN && n == TONE_LEN) {
            import_save();
            char buf[32];
            snprintf(buf, sizeof(buf), "IMPORT %d / %d", imp_index + 1, IMPORT_COUNT);
            synth_notice(buf);
            if (++imp_index >= IMPORT_COUNT) {
                import_finish(NULL);
            } else {
                imp_stage = IMP_SET;
            }
        }
        return;
    }
    ESP_LOGI(TAG, "read %d bytes of part %d's tone", n, at / TONE_LEN + 1);
    if (shown) {
        synth_state_received();  /* the unit answered: no fallback */
        if (read_asked) {
            /* Auto (read) on purpose: the unit's tone replaces whatever
             * library patch the part had; reads on part changes don't */
            read_asked = false;
            synth_sound_replaced();
        }
        char name[11];
        memcpy(name, tone[part], 10);
        name[10] = 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "READ PART %d: %s", part + 1, name);
        synth_notice(buf);
        synth_redraw();
    }
}

/* ---- writing into the unit's own memory (Store: TO D-110) ----
 * Its Write Request area (40 xx xx, manual p. 120) does what its WRITE
 * button does: the part's tone, as now in the temporary area, into an
 * internal tone (i11-i88). The unit answers with a result (40 10 00). */

static void unit_slot_name(int slot, char *buf, size_t len)
{
    snprintf(buf, len, "i%d%d", slot / 8 + 1, slot % 8 + 1);
}

static void unit_store(int slot, const char *name)
{
    /* The name first (the temporary tone's), then the write */
    for (int i = 0; i < 10; i++) {
        char c = *name ? *name++ : ' ';
        tone[part][i] = c >= 32 && c < 127 ? c : ' ';
        known[part][i] = true;
    }
    send_bytes(0, 10);
    int a = (WRITE_REQUEST >> 16 & 0x7F) * 16384 + part * 2;
    uint8_t msg[] = {0xF0, 0x41, device_id(), MODEL_ID, 0x12, a >> 14 & 0x7F, a >> 7 & 0x7F, a & 0x7F,
                     slot & 63, 0, 0, 0xF7};  /* slot, internal */
    int sum = 0;
    for (int i = 5; i < 10; i++) {
        sum += msg[i];
    }
    msg[10] = (128 - (sum & 0x7F)) & 0x7F;
    midi_send_sysex(msg, sizeof(msg));
    char slot_name[8], buf[40];
    unit_slot_name(slot, slot_name, sizeof(slot_name));
    snprintf(buf, sizeof(buf), "WRITING PART %d TO %s", part + 1, slot_name);
    synth_notice(buf);
}

static void read_tone(void)
{
    read_asked = true;
    if (imp_stage != IMP_IDLE) {
        import_finish("IMPORT stopped");  /* Auto during an import stops it */
        return;
    }
    request_tone();
}

/* Sets p to v in partial i (or the common part) and sends it */
static void set_one(int p, int i, int v)
{
    {
        int at = byte_of(p, i);
        uint8_t b = tone[part][at];
        switch (p) {
        case P_WAVE: b = (b & ~1) | (v & 1); break;
        case P_BANK: b = (b & ~2) | ((v & 1) << 1); break;
        case P_ON1: case P_ON2: case P_ON3: case P_ON4: {
            int bit = 1 << (p - P_ON1);
            if (!known[part][at]) {
                b = 0x0F;  /* unknown mute byte: assume all on */
            }
            b = v ? (b | bit) : (b & ~bit);
            break;
        }
        default: b = v; break;
        }
        tone[part][at] = b;
        known[part][at] = true;
        send_bytes(at, 1);
    }
    if (p == P_BANK || p == P_FBIAS_PT || p == P_ABP1 || p == P_ABP2) {
        synth_redraw();  /* another column's picture depends on it */
    }
}

/* Sets p to v in every selected partial (or the common part) */
static void set_value(int p, int v)
{
    for (int i = 0; i < (is_partial_param(p) ? 4 : 1); i++) {
        if (!is_partial_param(p) || (partial_sel & (1 << i))) {
            set_one(p, i, v);
        }
    }
}

/* ---- formatting ---- */

static const char *const note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static const char *param_name(int p) { return params[p].name; }

/* Parameters the partial's kind doesn't use: the WG waveform, pulse width
 * and the whole TVF for PCM partials, the PCM wave for synth ones */
static bool not_used(int p)
{
    if (!is_partial_param(p)) {
        return false;
    }
    bool pcm = partial_is_pcm(first_partial());
    return pcm ? params[p].synth_only : params[p].pcm_only;
}

static void param_format(int p, char *buf, size_t len)
{
    const d110_param_t *pp = &params[p];
    if (!value_known(p)) {
        snprintf(buf, len, "--");
        return;
    }
    if (not_used(p)) {
        snprintf(buf, len, "n/a");
        return;
    }
    int v = raw_value(p, first_partial());
    switch (pp->kind) {
    case K_SIGNED:
        snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        break;
    case K_NOTE:
        snprintf(buf, len, "%s%d", note_names[v % 12], v / 12 + 1);
        break;
    case K_KEYF:
        snprintf(buf, len, "%s", keyf_labels[v <= 16 ? v : 0]);
        break;
    case K_BIAS: {
        int note = 33 + (v & 63);  /* A1 .. C7 */
        snprintf(buf, len, "%c%s%d", v < 64 ? '<' : '>', note_names[note % 12], note / 12 - 1);
        break;
    }
    case K_LIST:
        snprintf(buf, len, "%s", pp->labels[v <= pp->max ? v : 0]);
        break;
    case K_PCM:
    case K_STRUCT:
        snprintf(buf, len, "%d", v + 1);
        break;
    case K_EG:
        if (pp->center) {
            snprintf(buf, len, v - pp->center ? "%+d" : "0", v - pp->center);
        } else {
            snprintf(buf, len, "%d", v);
        }
        break;
    default:
        snprintf(buf, len, "%d", v);
        break;
    }
}

static int param_bar(int p)
{
    const d110_param_t *pp = &params[p];
    if (!value_known(p) || not_used(p) || pp->kind == K_EG || pp->kind == K_LIST) {
        return -1;
    }
    return raw_value(p, first_partial()) * 1000 / pp->max;
}

static bool param_on(int p)
{
    return value_known(p) && raw_value(p, first_partial());
}

static int param_units(int p)
{
    int max = params[p].max;
    return max <= 1 ? 80 : max <= 4 ? 70 : max <= 16 ? 40 : max <= 30 ? 25
           : params[p].kind == K_PCM ? 13 : 10;  /* picking a wave: a bit slower */
}

/* p's current value in partial i, or the init tone's if not known */
static int value_or_init(int p, int i)
{
    if (known[part][byte_of(p, i)]) {
        return raw_value(p, i);
    }
    read_init();
    int v = init_tone[byte_of(p, i)];
    return p == P_WAVE ? v & 1 : p == P_BANK ? (v >> 1) & 1 : v;
}

/* With several partials selected, each moves by the same steps from its
 * own value, so their differences stay (the PG-10's absolute knobs set
 * them all to one value instead) */
static bool param_step(int p, int steps)
{
    bool changed = false;
    for (int i = 0; i < (is_partial_param(p) ? 4 : 1); i++) {
        if (is_partial_param(p) && !(partial_sel & (1 << i))) {
            continue;
        }
        int v = value_or_init(p, i), nv = v + steps;
        nv = nv < 0 ? 0 : nv > params[p].max ? params[p].max : nv;
        if (nv != v || !known[part][byte_of(p, i)]) {
            set_one(p, i, nv);
            changed = true;
        }
    }
    return changed;
}

static void param_toggle(int p) { set_value(p, value_known(p) ? !raw_value(p, first_partial()) : 0); }

static bool param_reset(int p)
{
    read_init();
    int v = init_tone[byte_of(p, first_partial())];
    if (p == P_WAVE) v &= 1;
    if (p == P_BANK) v = (v >> 1) & 1;
    if (p >= P_ON1 && p <= P_ON4) v = 1;
    if (value_known(p) && raw_value(p, first_partial()) == v) {
        return false;
    }
    set_value(p, v);
    return true;
}

/* ---- pictures ---- */

/* A bias over the keyboard (A1-C7, left to right): flat up to the bias
 * point, then sloping up or down (the level's sign) towards the end the
 * direction points to (< = below the point, > = above) */
static void draw_bias(int disp, int x, int y, int w, int h, int point, int level, int range)
{
    int mid = y + h / 2;
    for (int i = 0; i < w; i += 3) {
        display_pixel(disp, x + i, mid, true);
    }
    int px = x + (point & 63) * (w - 1) / 63;
    bool below = point < 64;
    int end_y = mid - level * (h / 2 - 1) / range;
    if (below) {
        display_line(disp, x, end_y, px, mid, true);
        display_line(disp, px, mid, x + w - 1, mid, true);
    } else {
        display_line(disp, x, mid, px, mid, true);
        display_line(disp, px, mid, x + w - 1, end_y, true);
    }
    display_line(disp, px, mid - 2, px, mid + 2, true);  /* the point */
}

/* PCM wave: its number, and its name below instead of a bar; bias levels:
 * the curve with their bias point */
static int param_draw(int p, int disp, int x, int y, int w, int h)
{
    if ((p == P_FBIAS_LVL || p == P_ABL1 || p == P_ABL2) && value_known(p) && !not_used(p)) {
        int pt = p == P_FBIAS_LVL ? P_FBIAS_PT : p == P_ABL1 ? P_ABP1 : P_ABP2;
        int lvl = raw_value(p, first_partial()) - params[p].center;
        int range = p == P_FBIAS_LVL ? 7 : 12;
        draw_bias(disp, x + 4, y, w - 8, font_large.cap_height + 2, raw_value(pt, first_partial()), lvl,
                  range);
        return SYNTH_DRAW_LABEL;
    }
    if (params[p].kind != K_PCM || !value_known(p) || not_used(p)) {
        return SYNTH_DRAW_NONE;
    }
    char num[8], name[16];
    int i = first_partial();
    param_format(p, num, sizeof(num));
    pcm_name(raw_value(P_BANK, i), raw_value(p, i), name, sizeof(name));
    display_text(disp, x + (w - display_text_width(&font_medium, num)) / 2, y + 1 - font_medium.cap_top,
                 &font_medium, num, true);
    display_text(disp, x + (w - display_text_width(&font_small, name)) / 2, y + 15 - font_small.cap_top,
                 &font_small, name, true);
    return SYNTH_DRAW_FULL;
}

#define BAND_Y 14
#define BAND_H 38
#define EG_TEXT_CAP_Y 55

/* Envelope columns: "T1 50" at the bottom, the envelope gets the rest */
static bool column_draw(int p, int disp, int x)
{
    if (params[p].kind != K_EG) {
        return false;
    }
    char text[16], val[8];
    param_format(p, val, sizeof(val));
    snprintf(text, sizeof(text), "%s %s", params[p].name, val);
    int tw = display_text_width(&font_small, text);
    display_text(disp, x + (63 - tw) / 2, EG_TEXT_CAP_Y - font_small.cap_top, &font_small, text, true);
    return true;
}

/* An envelope over both displays: points at levels lv[] (0-100), reached
 * after times tm[] (0-100, wider the slower); a held sustain segment after
 * point sus_at; centre line for the pitch envelope */
static void draw_env(int disp, const int *lv, const int *tm, int n, int sus_at, bool centred)
{
    display_fill_rect(disp, 0, BAND_Y, 256, BAND_H, false);
    if (centred) {
        for (int x = disp * 256; x < disp * 256 + 255; x += 4) {
            display_pixel(disp, x % 256, BAND_Y + BAND_H / 2, true);
        }
    }
    const int sus_w = 40;
    int total = sus_w;
    for (int i = 1; i < n; i++) {
        total += 6 + tm[i];
    }
    float scale = 505.0f / total;
    float x = 2;
    for (int i = 1; i < n; i++) {
        float w = (6 + tm[i]) * scale;
        int x0 = (int)x, x1 = (int)(x + w);
        int y0 = BAND_Y + BAND_H - 1 - lv[i - 1] * (BAND_H - 1) / 100;
        int y1 = BAND_Y + BAND_H - 1 - lv[i] * (BAND_H - 1) / 100;
        /* only the part of the line on this display */
        for (int px = x0; px <= x1; px++) {
            if (px / 256 != disp || px % 256 > 254) {
                continue;
            }
            int py = x1 > x0 ? y0 + (y1 - y0) * (px - x0) / (x1 - x0) : y1;
            int pyn = x1 > x0 && px < x1 ? y0 + (y1 - y0) * (px + 1 - x0) / (x1 - x0) : py;
            int a = py < pyn ? py : pyn, b = py < pyn ? pyn : py;
            for (int yy = a; yy <= b; yy++) {
                display_pixel(disp, px % 256, yy, true);
            }
        }
        x += w;
        if (i == sus_at) {
            int xs = (int)x, xe = (int)(x + sus_w * scale);
            for (int px = xs; px <= xe; px++) {
                if (px / 256 == disp && px % 256 < 255) {
                    display_pixel(disp, px % 256, y1, true);
                }
            }
            x += sus_w * scale;
        }
    }
}

static int pv(int off) { return tone[part][COMMON_LEN + first_partial() * PARTIAL_LEN + off]; }

static bool partial_known(void)
{
    return known[part][COMMON_LEN + first_partial() * PARTIAL_LEN + 0x0B];
}

/* Structure of a partial pair, as in the manual (p. 133): the two partials
 * (S synth / P PCM, hollow when muted), mixed, ring modulated (R) with or
 * without the top partial alongside, or separate */
static void draw_structure(int disp, int x, int y, int pair)
{
    static const char type[13] = {'M', 'R', 'M', 'R', 'R', 'M', 'R', 'S', 'S', 'r', 'r', 'r', 'r'};
    int s = tone[part][pair ? C_STRUCT34 : C_STRUCT12] % 13;
    uint8_t mute = known[part][C_MUTE] ? tone[part][C_MUTE] : 0x0F;
    char label[24];
    snprintf(label, sizeof(label), "%d&%d  STRUCTURE %d", pair * 2 + 1, pair * 2 + 2, s + 1);
    display_text(disp, x, y - font_tiny.cap_top, &font_tiny, label, true);
    const int bw = 22, bh = 10, top_y = y + 10, bot_y = y + 30, bx = x + 4;
    for (int k = 0; k < 2; k++) {
        int i = pair * 2 + k, by = k ? bot_y : top_y;
        bool on = mute & (1 << i);
        char t[4];
        snprintf(t, sizeof(t), "%d%c", i + 1, partial_is_pcm(i) ? 'P' : 'S');
        display_round_rect(disp, bx, by, bw, bh, on, true);
        display_text(disp, bx + (bw - display_text_width(&font_tiny, t)) / 2, by + 2 - font_tiny.cap_top,
                     &font_tiny, t, !on);
    }
    int ty = top_y + bh / 2, by_ = bot_y + bh / 2, ox = bx + bw, out = x + 110;
    int my = (ty + by_) / 2;
    switch (type[s]) {
    case 'M':  /* mixed */
        display_line(disp, ox, ty, ox + 30, ty, true);
        display_line(disp, ox, by_, ox + 30, by_, true);
        display_line(disp, ox + 30, ty, ox + 30, by_, true);
        display_line(disp, ox + 30, my, out, my, true);
        break;
    case 'S':  /* separate outputs */
        display_line(disp, ox, ty, out, ty, true);
        display_line(disp, ox, by_, out, by_, true);
        break;
    default: {  /* ring modulator, with the top partial alongside ('R') or not */
        int rx = ox + 30;
        display_line(disp, ox, ty, rx, my - 2, true);
        display_line(disp, ox, by_, rx, my + 2, true);
        display_rect(disp, rx, my - 5, 11, 11, true);
        display_text(disp, rx + 3, my - 3 - font_tiny.cap_top, &font_tiny, "R", true);
        display_line(disp, rx + 11, my, out, my, true);
        if (type[s] == 'R') {
            display_line(disp, ox, ty, ox + 10, ty - 6, true);
            display_line(disp, ox + 10, ty - 6, rx + 22, ty - 6, true);
            display_line(disp, rx + 22, ty - 6, rx + 22, my, true);
        }
        break;
    }
    }
}

static void page_draw(int pg, int disp)
{
    if (pg == PG_COMMON && disp == 1) {
        display_fill_rect(1, 0, 12, 256, 52, false);
        if (!known[part][C_STRUCT12]) {
            display_text(1, 4, 30, &font_small, "load a tone to see it", true);
            return;
        }
        draw_structure(1, 2, 16, 0);
        draw_structure(1, 130, 16, 1);
        return;
    }
    bool pitch = pg == PG_PITCH_ENV;
    bool tvf = pg == PG_TVF_ENV || pg == PG_TVF_ENV2;
    bool tva = pg == PG_TVA_ENV || pg == PG_TVA_ENV2;
    if (!pitch && !tvf && !tva) {
        return;
    }
    if (!partial_known()) {
        display_fill_rect(disp, 0, BAND_Y, 256, BAND_H, false);
        return;
    }
    if (pitch) {
        /* L0 -> T1 L1 -> T2 L2 -> T3 sustain (always centre) -> T4 end */
        int lv[5] = {pv(0x0F), pv(0x10), pv(0x11), 50, pv(0x13)};
        int tm[5] = {0, pv(0x0B), pv(0x0C), pv(0x0D), pv(0x0E)};
        draw_env(disp, lv, tm, 5, 3, true);
    } else {
        /* 0 -> T1 L1 -> T2 L2 -> T3 L3 -> T4 sustain -> T5 0 */
        int b = tvf ? 0x20 : 0x31;
        int lv[6] = {0, pv(b + 5), pv(b + 6), pv(b + 7), pv(b + 8), 0};
        int tm[6] = {0, pv(b), pv(b + 1), pv(b + 2), pv(b + 3), pv(b + 4)};
        draw_env(disp, lv, tm, 6, 4, false);
    }
}

/* ---- parts and partials ---- */

static int play_channel(void) { return (synth_setting(SETTING_CHANNEL) + part) & 0x0F; }

static void instance_step(int d)
{
    part = (part + d + PARTS) % PARTS;
    char buf[32];
    snprintf(buf, sizeof(buf), "PART %d  (channel %d)", part + 1, play_channel() + 1);
    synth_notice(buf);
    synth_redraw();
    request_tone();  /* fills the values in if the D-110's MIDI OUT is connected */
}

static bool shift_group(int g)
{
    if (g > 3) {
        return false;
    }
    uint8_t sel = partial_sel ^ (1 << g);
    if (!sel) {
        return false;  /* at least one partial stays selected */
    }
    partial_sel = sel;
    return true;
}

/* Step arrows: every selected partial moves one along (1 -> 2, 1+3 ->
 * 2+4), round from 4 to 1; Shift + arrows change the part */
static void shift_step(int d)
{
    partial_sel = d > 0 ? ((partial_sel << 1) | (partial_sel >> 3)) & 0x0F
                        : ((partial_sel >> 1) | (partial_sel << 3)) & 0x0F;
    char buf[16], tag[16];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (partial_sel & (1 << i)) {
            tag[n++] = '1' + i;
        }
    }
    tag[n] = 0;
    snprintf(buf, sizeof(buf), "PARTIAL %s", tag);
    synth_notice(buf);
}

static int shift_group_led(int g) { return g > 3 ? -1 : (partial_sel >> g) & 1; }

static int instance(void) { return part; }

/* The part's tone name, when its values are known (e.g. read back) */
static bool sound_name(char *buf, size_t len)
{
    if (!known[part][0]) {
        return false;
    }
    char name[11];
    memcpy(name, tone[part], 10);
    name[10] = 0;
    for (int i = 9; i >= 0 && name[i] == ' '; i--) {
        name[i] = 0;
    }
    snprintf(buf, len, "%s", name);
    return true;
}

static void header_tag(char *buf, size_t len)
{
    char p[8];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (partial_sel & (1 << i)) {
            p[n++] = '1' + i;
        }
    }
    p[n] = 0;
    snprintf(buf, len, "PT%d P%s", part + 1, p);
}

/* ---- patches (tones) ---- */

static bool patch_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fread(tone[part], 1, TONE_LEN, f) == TONE_LEN;
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "can't read %s", path);
        return false;
    }
    memset(known[part], 1, TONE_LEN);
    send_bytes(0, TONE_LEN);
    return true;
}

static bool patch_save(const char *path)
{
    /* The tone's own name (10 characters) follows the file's */
    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;
    const char *sp = strchr(title, ' ');
    title = sp ? sp + 1 : title;
    read_init();
    for (int i = 0; i < TONE_LEN; i++) {
        if (!known[part][i]) {
            tone[part][i] = init_tone[i];  /* never set: saved as the init tone's */
        }
    }
    for (int i = 0; i < 10; i++) {
        char c = *title ? *title++ : ' ';
        tone[part][i] = c >= 32 && c < 127 ? c : ' ';
    }
    memset(known[part], 1, TONE_LEN);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(tone[part], 1, TONE_LEN, f) == TONE_LEN;
    fclose(f);
    return ok;
}

static int unknown_count(void)
{
    int n = 0;
    for (int p = 0; p < PARAM_COUNT; p++) {
        int count = is_partial_param(p) ? 4 : 1;
        for (int i = 0; i < count; i++) {
            n += !known[part][byte_of(p, i)];
        }
    }
    return n;
}

/* Tutorial (Shift + Control): what's special here */
static const synth_tip_t tips[] = {
    {"D-110 LEVELS", {"Patch: 8 parts + rhythm on their own", "channels (D-110 PATCH). A part plays a",
                      "timbre: a tone and how it plays.", "A tone: up to 4 partials, in two pairs a",
                      "STRUCTURE mixes or ring-modulates.", "Rhythm: one drum kit (D-110 RHYTHM).",
                      "This level edits the tones. STEP: levels"},
     {BTN_STEP, -1, -1, -1, -1, -1, -1, -1}},
    {"PARTIALS", {"STEP arrows: the next partial.", "SHIFT + A-D: pick several - a knob then",
                  "moves them all alike (differences stay).", "While SHIFT is held, A-D show the pick;",
                  "the header shows it too (PT1 P13).", "PCM partials have no filter (TVF): n/a.",
                  "P1-P4 ON on A COMMON mute partials."},
     {BTN_STEP_LEFT, BTN_STEP_RIGHT, BTN_SHIFT, BTN_GROUP_A, BTN_GROUP_A + 1, BTN_GROUP_A + 2,
      BTN_GROUP_A + 3, -1}},
    {"PARTS", {"SHIFT + STEP arrows: the part (1-8).", "The pads play its channel; each part",
               "keeps its own tone and loaded patch.", "Header: PT2 = part 2."},
     {BTN_SHIFT, BTN_STEP_LEFT, BTN_STEP_RIGHT, -1, -1, -1, -1, -1}},
    {"READ, IMPORT, WRITE", {"AUTO: read the part's tone from the", "D-110 (its MIDI OUT to the Maschine).",
                             "CONTROL > IMPORT: all its tones into", "the library (001-192).",
                             "REC > TO D-110 (twice): write the tone", "into the D-110's memory, i11-i88."},
     {BTN_AUTO, BTN_CONTROL, BTN_REC, -1, -1, -1, -1, -1}},
    {"VOLUME", {"Hold VOLUME and turn the main encoder:", "Main Volume on all the D-110's parts",
                "(the same for its three levels)."},
     {BTN_VOLUME, -1, -1, -1, -1, -1, -1, -1}},
};

const synth_t synth_d110 = {
    .name = "D-110 TONE",
    .key = "d110",
    .patch_dir = STORAGE_ROOT "/d110",
    .default_patch = "000 Init",
    .first_user_patch = 200,  /* 001-192: imported from the unit (Setup: IMPORT) */
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
    .unknown_count = unknown_count,
    .page_draw = page_draw,
    .column_draw = column_draw,
    .play_channel = play_channel,
    .instance_step = shift_step,  /* arrows: the partial (most used) */
    .shift_group = shift_group,
    .shift_group_led = shift_group_led,
    .shift_step = instance_step,  /* Shift + arrows: the part */
    .header_tag = header_tag,
    .instance = instance,
    .sound_name = sound_name,
    .read = read_tone,
    .sysex_in = sysex_in,
    .tick = tick,
    .unit_store_label = "TO D-110",
    .unit_store_slots = 64,
    .unit_slot_name = unit_slot_name,
    .unit_store = unit_store,
    .action_name = "IMPORT",
    .action = import_start,
    .categories = categories,
    .volume_get = d110_volume_get,
    .volume_set = d110_volume_set,
    .keep_state = true,  /* selecting it reads the part's tone, sends nothing */
    .theme_bright = {0, 100, 127},  /* cyan */
    .theme_dim = {0, 9, 12},
    .tips = tips,
    .tip_count = sizeof(tips) / sizeof(tips[0]),
};
