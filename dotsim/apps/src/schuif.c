/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Schuif: slide the tiles, equal ones merge. Reach 2048.
 *        D-pad or a swipe slides, TOP starts over.
 */
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define N 4
#define TS 70
#define GAP 8
#define BOARD (N * TS + (N + 1) * GAP)
#define BXO ((KK_W - BOARD) / 2)
#define BYO 130

static int b[N][N];
static int pop[N][N];      /* frames left of a merge's pop */
static int score, best;
static bool won_shown, over;
static int sx0, sy0;
static bool swiping;

static void add_tile(void)
{
    int free_n = 0;
    for ( int y = 0; y < N; ++y ) { for ( int x = 0; x < N; ++x ) { if ( !b[y][x] ) { ++free_n; } } }
    if ( !free_n ) { return; }
    int k = (int)kk_rand((uint32_t)free_n);
    for ( int y = 0; y < N; ++y )
    {
        for ( int x = 0; x < N; ++x )
        {
            if ( !b[y][x] && (0 == k--) ) { b[y][x] = (kk_rand(10) == 0) ? 4 : 2; pop[y][x] = 4; return; }
        }
    }
}

static void new_game(void)
{
    memset(b, 0, sizeof(b));
    score = 0;
    over = won_shown = false;
    add_tile();
    add_tile();
}

/* Slide one line toward index 0. Returns true when something moved. */
static bool slide_line(int *v[N], int pops[N])
{
    int out[N] = { 0 }, n = 0;
    bool moved = false;
    int last = 0;
    for ( int i = 0; i < N; ++i )
    {
        int const t = *v[i];
        if ( !t ) { continue; }
        if ( last && (last == t) )
        {
            out[n - 1] = t * 2;
            score += t * 2;
            pops[n - 1] = 6;
            last = 0;
        }
        else
        {
            out[n++] = t;
            last = t;
        }
    }
    for ( int i = 0; i < N; ++i )
    {
        if ( *v[i] != out[i] ) { moved = true; }
        *v[i] = out[i];
    }
    return moved;
}

static bool move(int d)   /* 0 up 1 down 2 left 3 right */
{
    bool moved = false;
    for ( int k = 0; k < N; ++k )
    {
        int *v[N];
        int *p[N];
        int pops[N] = { 0 };
        for ( int i = 0; i < N; ++i )
        {
            int x, y;
            switch ( d )
            {
            case 0: x = k; y = i; break;
            case 1: x = k; y = N - 1 - i; break;
            case 2: x = i; y = k; break;
            default: x = N - 1 - i; y = k; break;
            }
            v[i] = &b[y][x];
            p[i] = &pop[y][x];
        }
        if ( slide_line(v, pops) ) { moved = true; }
        for ( int i = 0; i < N; ++i ) { if ( pops[i] ) { *p[i] = pops[i]; } }
    }
    return moved;
}

static bool can_move(void)
{
    for ( int y = 0; y < N; ++y )
    {
        for ( int x = 0; x < N; ++x )
        {
            if ( !b[y][x] ) { return true; }
            if ( (x + 1 < N) && (b[y][x] == b[y][x + 1]) ) { return true; }
            if ( (y + 1 < N) && (b[y][x] == b[y + 1][x]) ) { return true; }
        }
    }
    return false;
}

static uint16_t tile_col(int v)
{
    switch ( v )
    {
    case 2: return KK_RGB(238, 228, 218);
    case 4: return KK_RGB(237, 224, 200);
    case 8: return KK_RGB(242, 177, 121);
    case 16: return KK_RGB(245, 149, 99);
    case 32: return KK_RGB(246, 124, 95);
    case 64: return KK_RGB(246, 94, 59);
    case 128: return KK_RGB(237, 207, 114);
    case 256: return KK_RGB(237, 204, 97);
    case 512: return KK_RGB(237, 200, 80);
    case 1024: return KK_RGB(237, 197, 63);
    case 2048: return KK_RGB(237, 194, 46);
    default: return KK_RGB(60, 58, 50);
    }
}

static void do_move(int d)
{
    if ( over ) { return; }
    if ( move(d) )
    {
        add_tile();
        kk_tone(330, 15);
        if ( score > best ) { best = score; kk_best_put("schuif", best); }
        if ( !can_move() ) { over = true; kk_tone(150, 300); kk_led(80, 0, 0); }
    }
}

void app_main(void)
{
    kk_init();
    best = (int)kk_best_get("schuif");
    new_game();
    for ( ;; )
    {
        kk_begin();
        if ( kk_pressed(KK_UP) ) { do_move(0); }
        if ( kk_pressed(KK_DOWN) ) { do_move(1); }
        if ( kk_pressed(KK_LEFT) ) { do_move(2); }
        if ( kk_pressed(KK_RIGHT) ) { do_move(3); }
        if ( kk_pressed(KK_TOP) ) { new_game(); kk_led(0, 0, 0); }
        int tx, ty;
        bool const t = kk_touch(&tx, &ty);
        if ( t && !swiping ) { swiping = true; sx0 = tx; sy0 = ty; }
        if ( swiping && t )
        {
            int const dx = tx - sx0, dy = ty - sy0;
            if ( (dx * dx + dy * dy) > 40 * 40 )
            {
                if ( (dx > 0 ? dx : -dx) > (dy > 0 ? dy : -dy) ) { do_move(dx > 0 ? 3 : 2); }
                else { do_move(dy > 0 ? 1 : 0); }
                sx0 = tx; sy0 = ty;
                swiping = false;   /* one move per swipe; lift to swipe again */
                sx0 = -9999;
            }
        }
        if ( !t ) { swiping = false; }

        kk_clear(KK_RGB(250, 248, 239));
        kk_rect(0, 0, KK_W, KK_H, KK_RGB(250, 248, 239));
        kk_text(BXO, 50, "SLIDE", 4, KK_RGB(119, 110, 101));
        char buf[24];
        kk_round(KK_W - BXO - 140, 40, 66, 50, 8, KK_RGB(187, 173, 160));
        kk_round(KK_W - BXO - 66, 40, 66, 50, 8, KK_RGB(187, 173, 160));
        kk_text(KK_W - BXO - 132, 48, "SCORE", 1, KK_RGB(238, 228, 218));
        (void)snprintf(buf, sizeof(buf), "%d", score);
        kk_text(KK_W - BXO - 132, 64, buf, 2, KK_WHITE);
        kk_text(KK_W - BXO - 58, 48, "BEST", 1, KK_RGB(238, 228, 218));
        (void)snprintf(buf, sizeof(buf), "%d", best);
        kk_text(KK_W - BXO - 58, 64, buf, 2, KK_WHITE);

        kk_round(BXO, BYO, BOARD, BOARD, 10, KK_RGB(187, 173, 160));
        bool has2048 = false;
        for ( int y = 0; y < N; ++y )
        {
            for ( int x = 0; x < N; ++x )
            {
                int const v = b[y][x];
                int const px = BXO + GAP + x * (TS + GAP), py = BYO + GAP + y * (TS + GAP);
                int grow = 0;
                if ( pop[y][x] ) { grow = pop[y][x]; --pop[y][x]; }
                if ( !v ) { kk_round(px, py, TS, TS, 6, KK_RGB(205, 193, 180)); continue; }
                if ( 2048 == v ) { has2048 = true; }
                kk_round(px - grow / 2, py - grow / 2, TS + grow, TS + grow, 6, tile_col(v));
                (void)snprintf(buf, sizeof(buf), "%d", v);
                int const s = (v < 100) ? 5 : (v < 1000) ? 4 : 3;
                uint16_t const tc = (v <= 4) ? KK_RGB(119, 110, 101) : KK_WHITE;
                kk_text(px + (TS - kk_text_w(buf, s)) / 2, py + (TS - 7 * s) / 2, buf, s, tc);
            }
        }
        if ( has2048 && !won_shown ) { won_shown = true; kk_tone(1320, 300); kk_led(80, 70, 0); }
        if ( over )
        {
            kk_round(BXO + 30, BYO + 110, BOARD - 60, 120, 14, KK_RGB(238, 228, 218));
            kk_text_c(BYO + 130, "STUCK!", 5, KK_RGB(119, 110, 101));
            kk_text_c(BYO + 190, "TOP: AGAIN", 2, KK_RGB(119, 110, 101));
        }
        kk_text_c(BYO + BOARD + 20, "SLIDE WITH D-PAD OR FINGER", 1, KK_RGB(150, 140, 130));
        kk_text_c(BYO + BOARD + 36, "TOP: NEW GAME", 1, KK_RGB(150, 140, 130));
        kk_end();
    }
}
