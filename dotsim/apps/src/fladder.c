/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Fladder: keep the little balloon in the air and through the gaps.
 *        TOP or a tap flaps.
 */
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define NCOL 3
#define COLW 58
#define GAPH 150
#define SPACING 190
#define BIRDX 130
#define GROUND 440

typedef struct { float x; int gap_y; bool scored; } col_t;

static col_t cols[NCOL];
static float by, vy, scroll;
static int score, best;
static uint32_t dead_at;
static enum { TITLE, PLAY, DEAD } mode;

static void reset(void)
{
    by = 230;
    vy = 0;
    score = 0;
    for ( int i = 0; i < NCOL; ++i )
    {
        cols[i].x = KK_W + 40 + i * SPACING;
        cols[i].gap_y = 130 + (int)kk_rand(200);
        cols[i].scored = false;
    }
}

static void flap(void)
{
    vy = -6.2f;
    kk_tone(660, 25);
}

static void die(void)
{
    mode = DEAD;
    dead_at = kk_ms();
    if ( score > best ) { best = score; kk_best_put("fladder", best); }
    kk_tone(160, 300);
    kk_led(90, 0, 0);
}

static void update(void)
{
    vy += 0.38f;
    if ( vy > 9 ) { vy = 9; }
    by += vy;
    if ( by < 40 ) { by = 40; vy = 0; }
    if ( by > GROUND - 14 ) { by = GROUND - 14; die(); return; }
    float const speed = 2.6f + (float)score * 0.05f;
    scroll += speed;
    for ( int i = 0; i < NCOL; ++i )
    {
        col_t *c = &cols[i];
        c->x -= speed;
        if ( c->x < -COLW )
        {
            float maxx = 0;
            for ( int j = 0; j < NCOL; ++j ) { if ( cols[j].x > maxx ) { maxx = cols[j].x; } }
            c->x = maxx + SPACING;
            c->gap_y = 120 + (int)kk_rand(210);
            c->scored = false;
        }
        if ( !c->scored && (c->x + COLW < BIRDX - 14) )
        {
            c->scored = true;
            ++score;
            kk_tone(1100, 40);
            kk_led(0, 50, 60);
        }
        /* hit: the bird's circle against both pillars */
        if ( (BIRDX + 13 > c->x) && (BIRDX - 13 < c->x + COLW) )
        {
            if ( (by - 12 < c->gap_y - GAPH / 2) || (by + 12 > c->gap_y + GAPH / 2) ) { die(); return; }
        }
    }
}

static void pillar(int x, int y, int h, bool cap_bottom)
{
    if ( h <= 0 ) { return; }
    uint16_t const base = KK_RGB(90, 190, 90);
    kk_rect(x, y, COLW, h, base);
    kk_rect(x + 6, y, 8, h, KK_RGB(150, 230, 140));
    kk_rect(x + COLW - 10, y, 10, h, KK_RGB(60, 140, 70));
    int const capy = cap_bottom ? (y + h - 22) : y;
    kk_rect(x - 6, capy, COLW + 12, 22, KK_RGB(100, 205, 100));
    kk_box(x - 6, capy, COLW + 12, 22, 2, KK_RGB(40, 100, 50));
}

static void draw(void)
{
    kk_vgrad(0, 0, KK_W, GROUND, KK_RGB(110, 190, 240), KK_RGB(200, 235, 250));
    /* clouds drift slower than the pillars */
    for ( int i = 0; i < 4; ++i )
    {
        int const cx = (int)(((i * 150) - (int)(scroll * 0.3f)) % (KK_W + 160) + KK_W + 160) % (KK_W + 160) - 80;
        int const cy = 80 + (i * 53) % 120;
        kk_disc(cx, cy, 22, KK_WHITE);
        kk_disc(cx + 24, cy + 6, 18, KK_WHITE);
        kk_disc(cx - 22, cy + 8, 16, KK_WHITE);
    }
    for ( int i = 0; i < NCOL; ++i )
    {
        int const x = (int)cols[i].x;
        int const top = cols[i].gap_y - GAPH / 2, bot = cols[i].gap_y + GAPH / 2;
        pillar(x, 0, top, true);
        pillar(x, bot, GROUND - bot, false);
    }
    kk_rect(0, GROUND, KK_W, KK_H - GROUND, KK_RGB(222, 200, 140));
    kk_rect(0, GROUND, KK_W, 8, KK_RGB(120, 200, 90));
    for ( int x = -((int)scroll % 24); x < KK_W; x += 24 ) { kk_rect(x, GROUND + 8, 12, 4, KK_RGB(200, 178, 120)); }
    /* the balloon-bird: a round body, a beak, a wing that beats with vy */
    int const bx = BIRDX, byi = (int)by;
    kk_disc(bx, byi, 14, KK_YELLOW);
    kk_disc(bx + 5, byi - 4, 5, KK_WHITE);
    kk_disc(bx + 6, byi - 4, 2, KK_BLACK);
    kk_rect(bx + 12, byi + 1, 9, 5, KK_ORANGE);
    int const wing = (vy < 0) ? -5 : 3;
    kk_round(bx - 14, byi + wing, 14, 8, 4, KK_RGB(250, 170, 40));
    char buf[16];
    (void)snprintf(buf, sizeof(buf), "%d", score);
    kk_text_c(40, buf, 5, KK_WHITE);
}

void app_main(void)
{
    kk_init();
    best = (int)kk_best_get("fladder");
    reset();
    mode = TITLE;
    for ( ;; )
    {
        kk_begin();
        bool const tap = kk_pressed(KK_TOP) || kk_pressed(KK_UP) || kk_tapped(NULL, NULL);
        if ( PLAY == mode )
        {
            if ( tap ) { flap(); }
            update();
        }
        else if ( TITLE == mode )
        {
            by = 230 + 10 * ((kk_ms() / 300) % 2);
            scroll += 1.5f;
            if ( tap ) { reset(); mode = PLAY; flap(); kk_led(0, 0, 0); }
        }
        else if ( tap && ((kk_ms() - dead_at) > 600) )
        {
            reset();
            mode = TITLE;
            kk_led(0, 0, 0);
        }
        draw();
        if ( TITLE == mode )
        {
            kk_round(70, 140, 270, 90, 16, KK_RGB(255, 255, 255));
            kk_text_c(160, "FLUTTER", 5, KK_RGB(40, 120, 200));
            kk_text_c(300, "TOP OR TAP: FLAP", 2, KK_RGB(30, 60, 90));
            char buf[24];
            (void)snprintf(buf, sizeof(buf), "BEST %d", best);
            kk_text_c(330, buf, 2, KK_RGB(30, 60, 90));
        }
        else if ( DEAD == mode )
        {
            char buf[32];
            kk_round(65, 170, 280, 130, 16, KK_WHITE);
            kk_text_c(188, "SPLAT!", 5, KK_RED);
            (void)snprintf(buf, sizeof(buf), "SCORE %d  BEST %d", score, best);
            kk_text_c(240, buf, 2, KK_RGB(30, 40, 60));
            kk_text_c(270, "TOP: AGAIN", 2, KK_GREY);
        }
        kk_end();
    }
}
