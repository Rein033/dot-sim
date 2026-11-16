/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Kleurgeheugen: watch and hear the sequence, then play it back.
 *        Each round adds one. The D-pad or a tap picks a colour.
 */
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define MAXSEQ 64
#define CX (KK_W / 2)
#define CY 258
#define PAD 72

static const uint16_t COL[4] = { KK_RGB(60, 200, 90), KK_RGB(230, 60, 70), KK_RGB(250, 200, 40), KK_RGB(50, 140, 240) };
static const uint8_t LEDC[4][3] = { { 0, 90, 20 }, { 90, 0, 0 }, { 90, 70, 0 }, { 0, 30, 90 } };
static const uint16_t HZ[4] = { 392, 330, 262, 523 };   /* G E C C' */
/* up, right, down, left: the four pads around the centre */
static const int PX[4] = { 0, 1, 0, -1 }, PY[4] = { -1, 0, 1, 0 };

static uint8_t seq[MAXSEQ];
static int len, pos, best;
static int lit = -1;
static uint32_t lit_until, next_at;
static enum { TITLE, SHOW, INPUT, WRONG } mode;

static void light(int i, uint32_t ms)
{
    lit = i;
    lit_until = kk_ms() + ms;
    kk_tone(HZ[i], (uint16_t)ms);
    kk_led(LEDC[i][0], LEDC[i][1], LEDC[i][2]);
}

static void new_round(void)
{
    if ( len < MAXSEQ ) { seq[len++] = (uint8_t)kk_rand(4); }
    pos = 0;
    mode = SHOW;
    next_at = kk_ms() + 700;
}

static void press(int i)
{
    if ( INPUT != mode ) { return; }
    if ( seq[pos] == i )
    {
        light(i, 220);
        if ( ++pos == len )
        {
            if ( len > best ) { best = len; kk_best_put("kleur", best); }
            new_round();
            next_at = kk_ms() + 1000;
        }
    }
    else
    {
        lit = -1;
        mode = WRONG;
        kk_tone(110, 600);
        kk_led(100, 0, 0);
    }
}

static int pad_at(int x, int y)
{
    for ( int i = 0; i < 4; ++i )
    {
        int const px = CX + PX[i] * 100, py = CY + PY[i] * 100;
        if ( ((x - px) * (x - px) + (y - py) * (y - py)) < PAD * PAD ) { return i; }
    }
    return -1;
}

void app_main(void)
{
    kk_init();
    best = (int)kk_best_get("kleur");
    mode = TITLE;
    for ( ;; )
    {
        kk_begin();
        uint32_t const now = kk_ms();
        if ( (lit >= 0) && (now > lit_until) ) { lit = -1; kk_led(0, 0, 0); }
        if ( SHOW == mode )
        {
            if ( (lit < 0) && (now >= next_at) )
            {
                if ( pos < len )
                {
                    uint32_t const ms = (len < 6) ? 480 : (len < 12) ? 380 : 300;
                    light(seq[pos++], ms);
                    next_at = now + ms + 140;
                }
                else
                {
                    pos = 0;
                    mode = INPUT;
                }
            }
        }
        else if ( INPUT == mode )
        {
            if ( kk_pressed(KK_UP) ) { press(0); }
            if ( kk_pressed(KK_RIGHT) ) { press(1); }
            if ( kk_pressed(KK_DOWN) ) { press(2); }
            if ( kk_pressed(KK_LEFT) ) { press(3); }
            int tx, ty;
            if ( kk_tapped(&tx, &ty) ) { int const p = pad_at(tx, ty); if ( p >= 0 ) { press(p); } }
        }
        else if ( kk_pressed(KK_TOP) || kk_tapped(NULL, NULL) )
        {
            len = 0;
            kk_led(0, 0, 0);
            new_round();
        }

        kk_clear(KK_RGB(16, 16, 22));
        kk_disc(CX, CY, 190, KK_RGB(28, 28, 38));
        for ( int i = 0; i < 4; ++i )
        {
            int const px = CX + PX[i] * 100, py = CY + PY[i] * 100;
            bool const on = (lit == i);
            uint16_t const c = on ? kk_mix(COL[i], KK_WHITE, 90) : kk_mix(COL[i], KK_BLACK, 150);
            if ( on ) { kk_disc(px, py, PAD + 8, kk_mix(COL[i], KK_BLACK, 120)); }
            kk_disc(px, py, PAD, c);
            kk_disc(px - 18, py - 20, 16, kk_mix(c, KK_WHITE, 70));
        }
        kk_disc(CX, CY, 46, KK_RGB(16, 16, 22));
        char buf[24];
        (void)snprintf(buf, sizeof(buf), "%d", (TITLE == mode) ? 0 : (len - ((WRONG == mode) ? 1 : 0)));
        kk_text(CX - kk_text_w(buf, 4) / 2, CY - 14, buf, 4, KK_WHITE);
        if ( TITLE == mode )
        {
            kk_text_c(36, "COLOR MEMORY", 3, KK_WHITE);
            kk_text_c(476, "TOP: START", 2, KK_GREY);
        }
        else if ( SHOW == mode )
        {
            kk_text_c(40, "WATCH AND LISTEN", 2, KK_GREY);
        }
        else if ( INPUT == mode )
        {
            (void)snprintf(buf, sizeof(buf), "YOU: %d / %d", pos, len);
            kk_text_c(40, buf, 2, KK_WHITE);
        }
        else
        {
            kk_text_c(36, "WRONG!", 3, KK_RED);
            (void)snprintf(buf, sizeof(buf), "BEST %d   TOP: AGAIN", best);
            kk_text_c(480, buf, 1, KK_GREY);
        }
        kk_end();
    }
}
