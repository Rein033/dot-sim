/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Blokkenregen: stack the falling blocks, fill a row to clear it.
 *        LEFT/RIGHT move, DOWN drops faster, TOP turns, BOT drops at once.
 */
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define GW 10
#define GH 20
#define CS 19
#define GX 62
#define GY 78

/* Seven pieces, each four cells in a 4x4 box, as bit masks per rotation. */
static const uint16_t PIECE[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },   /* long */
    { 0x0660, 0x0660, 0x0660, 0x0660 },   /* square */
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 },   /* tee */
    { 0x06C0, 0x4620, 0x06C0, 0x4620 },   /* step right */
    { 0x0C60, 0x2640, 0x0C60, 0x2640 },   /* step left */
    { 0x0E20, 0x44C0, 0x8E00, 0x6440 },   /* hook */
    { 0x0E80, 0xC440, 0x2E00, 0x4460 },   /* hook, mirrored */
};
static const uint16_t PCOL[8] = {
    0, KK_RGB(60, 200, 230), KK_RGB(250, 205, 50), KK_RGB(170, 90, 230),
    KK_RGB(90, 210, 100), KK_RGB(235, 70, 80), KK_RGB(60, 120, 240), KK_RGB(245, 140, 50),
};

static uint8_t grid[GH][GW];
static int cur, rot, cx, cy, nxt;
static int score, lines, level, best;
static uint32_t last_fall, last_move;
static int clearing[4], nclear;
static uint32_t clear_at;
static enum { TITLE, PLAY, OVER } mode;

static bool cell(int p, int r, int x, int y) { return (PIECE[p][r] >> (15 - (y * 4 + x))) & 1; }

static bool fits(int p, int r, int ox, int oy)
{
    for ( int y = 0; y < 4; ++y )
    {
        for ( int x = 0; x < 4; ++x )
        {
            if ( !cell(p, r, x, y) ) { continue; }
            int const gx = ox + x, gy = oy + y;
            if ( (gx < 0) || (gx >= GW) || (gy >= GH) ) { return false; }
            if ( (gy >= 0) && grid[gy][gx] ) { return false; }
        }
    }
    return true;
}

static void spawn(void)
{
    cur = nxt;
    nxt = (int)kk_rand(7);
    rot = 0;
    cx = 3;
    cy = -1;
    if ( !fits(cur, rot, cx, cy) )
    {
        mode = OVER;
        if ( score > best ) { best = score; kk_best_put("blok", best); }
        kk_tone(140, 500);
        kk_led(90, 0, 0);
    }
}

static void new_game(void)
{
    memset(grid, 0, sizeof(grid));
    score = lines = 0;
    level = 1;
    nclear = 0;
    nxt = (int)kk_rand(7);
    spawn();
    last_fall = kk_ms();
}

static uint32_t fall_ms(void)
{
    int const ms = 700 - (level - 1) * 60;
    return (uint32_t)((ms < 90) ? 90 : ms);
}

static void lock_piece(void)
{
    for ( int y = 0; y < 4; ++y )
    {
        for ( int x = 0; x < 4; ++x )
        {
            if ( cell(cur, rot, x, y) && (cy + y >= 0) ) { grid[cy + y][cx + x] = (uint8_t)(cur + 1); }
        }
    }
    nclear = 0;
    for ( int y = 0; y < GH; ++y )
    {
        bool full = true;
        for ( int x = 0; x < GW; ++x ) { if ( !grid[y][x] ) { full = false; break; } }
        if ( full ) { clearing[nclear++] = y; }
    }
    if ( nclear )
    {
        static const int PTS[5] = { 0, 100, 300, 500, 800 };
        score += PTS[nclear] * level;
        lines += nclear;
        level = 1 + lines / 10;
        clear_at = kk_ms();
        kk_tone((uint16_t)(600 + nclear * 200), 120);
        kk_led(0, (uint8_t)(30 * nclear), (uint8_t)(20 * nclear));
    }
    else
    {
        kk_tone(180, 25);
        spawn();
    }
}

static void finish_clear(void)
{
    for ( int i = 0; i < nclear; ++i )
    {
        int const row = clearing[i];
        memmove(&grid[1][0], &grid[0][0], (size_t)row * GW);
        memset(&grid[0][0], 0, GW);
    }
    nclear = 0;
    kk_led(0, 0, 0);
    spawn();
}

static void block(int px, int py, int s, uint16_t c)
{
    kk_rect(px, py, s - 1, s - 1, c);
    kk_rect(px, py, s - 1, 3, kk_mix(c, KK_WHITE, 100));
    kk_rect(px, py, 3, s - 1, kk_mix(c, KK_WHITE, 60));
    kk_rect(px, py + s - 4, s - 1, 3, kk_mix(c, KK_BLACK, 90));
}

static void draw(void)
{
    kk_vgrad(0, 0, KK_W, KK_H, KK_RGB(30, 18, 50), KK_RGB(8, 6, 16));
    kk_rect(GX - 4, GY - 4, GW * CS + 8, GH * CS + 8, KK_RGB(70, 60, 100));
    kk_rect(GX, GY, GW * CS, GH * CS, KK_RGB(12, 10, 22));
    for ( int x = 1; x < GW; ++x ) { kk_rect(GX + x * CS - 1, GY, 1, GH * CS, KK_RGB(22, 20, 36)); }
    uint32_t const now = kk_ms();
    for ( int y = 0; y < GH; ++y )
    {
        bool flash = false;
        for ( int i = 0; i < nclear; ++i ) { if ( clearing[i] == y ) { flash = true; } }
        for ( int x = 0; x < GW; ++x )
        {
            if ( grid[y][x] )
            {
                uint16_t c = PCOL[grid[y][x]];
                if ( flash && (((now - clear_at) / 60) & 1) ) { c = KK_WHITE; }
                block(GX + x * CS, GY + y * CS, CS, c);
            }
        }
    }
    if ( (PLAY == mode) && !nclear )
    {
        int gy = cy;
        while ( fits(cur, rot, cx, gy + 1) ) { ++gy; }
        for ( int y = 0; y < 4; ++y )
        {
            for ( int x = 0; x < 4; ++x )
            {
                if ( !cell(cur, rot, x, y) ) { continue; }
                if ( gy + y >= 0 ) { kk_box(GX + (cx + x) * CS, GY + (gy + y) * CS, CS - 1, CS - 1, 1, kk_mix(PCOL[cur + 1], KK_BLACK, 120)); }
                if ( cy + y >= 0 ) { block(GX + (cx + x) * CS, GY + (cy + y) * CS, CS, PCOL[cur + 1]); }
            }
        }
    }
    /* side panel */
    int const sx = GX + GW * CS + 18;
    char buf[24];
    kk_text(sx, 90, "NEXT", 1, KK_GREY);
    kk_round(sx - 4, 104, 84, 64, 8, KK_RGB(18, 14, 30));
    for ( int y = 0; y < 4; ++y )
    {
        for ( int x = 0; x < 4; ++x )
        {
            if ( cell(nxt, 0, x, y) ) { block(sx + 4 + x * 16, 108 + y * 16, 16, PCOL[nxt + 1]); }
        }
    }
    kk_text(sx, 190, "SCORE", 1, KK_GREY);
    (void)snprintf(buf, sizeof(buf), "%d", score);
    kk_text(sx, 204, buf, 2, KK_WHITE);
    kk_text(sx, 240, "LINES", 1, KK_GREY);
    (void)snprintf(buf, sizeof(buf), "%d", lines);
    kk_text(sx, 254, buf, 2, KK_WHITE);
    kk_text(sx, 290, "LEVEL", 1, KK_GREY);
    (void)snprintf(buf, sizeof(buf), "%d", level);
    kk_text(sx, 304, buf, 2, KK_YELLOW);
    kk_text(sx, 340, "BEST", 1, KK_GREY);
    (void)snprintf(buf, sizeof(buf), "%d", best);
    kk_text(sx, 354, buf, 2, KK_GREY);
}

void app_main(void)
{
    kk_init();
    best = (int)kk_best_get("blok");
    new_game();
    mode = TITLE;
    for ( ;; )
    {
        kk_begin();
        uint32_t const now = kk_ms();
        if ( PLAY == mode )
        {
            if ( nclear )
            {
                if ( (now - clear_at) > 300 ) { finish_clear(); }
            }
            else
            {
                uint32_t const h = kk_held();
                bool const rep = (now - last_move) > 110;
                if ( (kk_pressed(KK_LEFT) || ((h & KK_LEFT) && rep)) && fits(cur, rot, cx - 1, cy) ) { --cx; last_move = now; }
                if ( (kk_pressed(KK_RIGHT) || ((h & KK_RIGHT) && rep)) && fits(cur, rot, cx + 1, cy) ) { ++cx; last_move = now; }
                if ( kk_pressed(KK_TOP) || kk_pressed(KK_UP) )
                {
                    int const nr = (rot + 1) & 3;
                    static const int KICK[] = { 0, -1, 1, -2, 2 };
                    for ( int k = 0; k < 5; ++k )
                    {
                        if ( fits(cur, nr, cx + KICK[k], cy) ) { rot = nr; cx += KICK[k]; kk_tone(700, 15); break; }
                    }
                }
                if ( kk_pressed(KK_BOT) )
                {
                    while ( fits(cur, rot, cx, cy + 1) ) { ++cy; score += 2; }
                    lock_piece();
                    last_fall = now;
                }
                else
                {
                    uint32_t const fall = (h & KK_DOWN) ? 40U : fall_ms();
                    if ( (now - last_fall) >= fall )
                    {
                        last_fall = now;
                        if ( fits(cur, rot, cx, cy + 1) ) { ++cy; if ( h & KK_DOWN ) { score += 1; } }
                        else { lock_piece(); }
                    }
                }
            }
        }
        else if ( kk_pressed(KK_TOP) || kk_tapped(NULL, NULL) )
        {
            new_game();
            mode = PLAY;
            kk_led(0, 0, 0);
        }
        draw();
        if ( TITLE == mode )
        {
            kk_round(50, 180, 310, 160, 18, KK_RGB(14, 10, 26));
            kk_text_c(200, "BLOCK", 4, KK_PURPLE);
            kk_text_c(240, "RAIN", 4, KK_PURPLE);
            kk_text_c(290, "TOP: START", 2, KK_WHITE);
            kk_text_c(315, "TOP SPIN  BOT DROP", 1, KK_GREY);
        }
        else if ( OVER == mode )
        {
            char buf[40];
            kk_round(50, 180, 310, 140, 18, KK_RGB(14, 10, 26));
            kk_text_c(200, "FULL!", 5, KK_RED);
            (void)snprintf(buf, sizeof(buf), "SCORE %d", score);
            kk_text_c(255, buf, 2, KK_WHITE);
            kk_text_c(285, "TOP: AGAIN", 2, KK_GREY);
        }
        kk_end();
    }
}
