/*
 * The Maschine Mk2's buttons the editor uses, by their report 0x01 input
 * bit (see the button table in docs/PROTOCOL.md) - also for drivers, e.g.
 * to name the buttons a tutorial page lights up.
 */
#pragma once

#define BTN_DISPLAY_1 0
#define BTN_CONTROL 8
#define BTN_BROWSE 10
#define BTN_ARROW_LEFT 12
#define BTN_ARROW_RIGHT 13
#define BTN_ARROW_LEFT_2 19
#define BTN_ARROW_RIGHT_2 20
#define BTN_ENTER 21
#define BTN_GROUP_A 24     /* .. H = 31 */
#define BTN_REC 37
#define BTN_ERASE 38
#define BTN_MUTE 47
#define BTN_NAVIGATE 11  /* Sampling: next to Browse and Control */
#define BTN_STEP 9       /* by Control: the synth selection */
#define BTN_VOLUME 16    /* held: the main encoder sets the device volume */
#define BTN_ALL 14       /* the patch category screen */
#define BTN_PAD_MODE 42  /* the pads' screen: lowest note, scale, key */
#define BTN_GRID 35        /* synth_t.overview_draw: the patch overview */
#define BTN_STEP_LEFT 33   /* transport arrows: synth_t.instance_step */
#define BTN_STEP_RIGHT 34
#define BTN_SHIFT 39       /* + Group: synth_t.shift_group */
#define BTN_AUTO 15        /* synth_t.read: fetch the synth's current sound */
