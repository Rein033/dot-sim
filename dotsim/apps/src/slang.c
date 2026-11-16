/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Slang: eat apples, grow, never bite yourself. D-pad steers, TOP starts.
 */
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define COLS 16
#define ROWS 19
#define CELL 20
#define OX ((KK_W - COLS * CELL) / 2)
#define OY 80
#define MAXLEN (COLS * ROWS)

typedef struct { int8_t x, y; } pt_t;

static pt_t body[MAXLEN];
static int len, dir, next_dir, score, best;
static pt_t apple;
static uint32_t step_ms, last_step;
static enum { TITLE, PLAY, OVER } mode;

static bool on_snake(int x, int y)
{
    for ( int i = 0; i < len; ++i )
    {
        if ( (body[i].x == x) && (body[i].y == y) ) { return true; }
    }
    return false;
}

static void place_apple(void)
{
    do
    {
        apple.x = (int8_t)kk_rand(COLS);
        apple.y = (int8_t)kk_rand(ROWS);
    } while ( on_snake(apple.x, apple.y) );
}

static void reset(void)
{
    len = 4;
    for ( int i = 0; i < len; ++i ) { body[i].x = (int8_t)(5 - i); body[i].y = ROWS / 2; }
    dir = next_dir = 3;
    score = 0;
    step_ms = 160;
    last_step = kk_ms();
    place_apple();
}

static const int DX[4] = { 0, 0, -1, 1 }, DY[4] = { -1, 1, 0, 0 };   /* up down left right */

static void step(void)
{
    dir = next_dir;
    int const nx = body[0].x + DX[dir], ny = body[0].y + DY[dir];
    if ( (nx < 0) || (ny < 0) || (nx >= COLS) || (ny >= ROWS) || on_snake(nx, ny) )
    {
        mode = OVER;
        if ( score > best ) { best = score; kk_best_put("slang", best); }
        kk_tone(220, 200);
        kk_led(80, 0, 0);
        return;
    }
    bool const eat = (nx == apple.x) && (ny == apple.y);
    if ( eat && (len < MAXLEN) ) { ++len; }
    memmove(&body[1], &body[0], sizeof(pt_t) * (size_t)(len - 1));
    body[0].x = (int8_t)nx;
    body[0].y = (int8_t)ny;
    if ( eat )
    {
        ++score;
        kk_tone(880, 40);
        kk_led(0, 60, 0);
        if ( step_ms > 70 ) { step_ms -= 5; }
        place_apple();
    }
    else
    {
        kk_led(0, 0, 0);
    }
}

static void draw_field(void)
{
    kk_round(OX - 8, OY - 8, COLS * CELL + 16, ROWS * CELL + 16, 14, KK_RGB(30, 40, 30));
    for ( int y = 0; y < ROWS; ++y )
    {
        for ( int x = 0; x < COLS; ++x )
        {
            uint16_t const c = ((x + y) & 1) ? KK_RGB(42, 58, 40) : KK_RGB(48, 66, 46);
            kk_rect(OX + x * CELL, OY + y * CELL, CELL, CELL, c);
        }
    }
    int const ax = OX + apple.x * CELL + CELL / 2, ay = OY + apple.y * CELL + CELL / 2;
    kk_disc(ax, ay + 1, 8, KK_RED);
    kk_rect(ax, ay - 10, 2, 4, KK_RGB(120, 80, 40));
    kk_disc(ax + 4, ay - 8, 3, KK_GREEN);
    for ( int i = len - 1; i >= 0; --i )
    {
        int const t = (i * 160) / (len > 1 ? len : 1);
        uint16_t const c = kk_mix(KK_RGB(120, 230, 90), KK_RGB(40, 120, 60), t);
        kk_round(OX + body[i].x * CELL + 1, OY + body[i].y * CELL + 1, CELL - 2, CELL - 2, 6, c);
    }
    /* eyes */
    int const hx = OX + body[0].x * CELL + CELL / 2, hy = OY + body[0].y * CELL + CELL / 2;
    int const ex = (dir >= 2) ? 0 : 5, ey = (dir >= 2) ? 5 : 0;
    kk_disc(hx - ex + DX[dir] * 3, hy - ey + DY[dir] * 3, 3, KK_WHITE);
    kk_disc(hx + ex + DX[dir] * 3, hy + ey + DY[dir] * 3, 3, KK_WHITE);
    kk_disc(hx - ex + DX[dir] * 4, hy - ey + DY[dir] * 4, 1, KK_BLACK);
    kk_disc(hx + ex + DX[dir] * 4, hy + ey + DY[dir] * 4, 1, KK_BLACK);
}

void app_main(void)
{
    kk_init();
    best = (int)kk_best_get("slang");
    reset();
    mode = TITLE;
    for ( ;; )
    {
        kk_begin();
        uint32_t const now = kk_ms();
        if ( kk_pressed(KK_UP) && (dir != 1) ) { next_dir = 0; }
        if ( kk_pressed(KK_DOWN) && (dir != 0) ) { next_dir = 1; }
        if ( kk_pressed(KK_LEFT) && (dir != 3) ) { next_dir = 2; }
        if ( kk_pressed(KK_RIGHT) && (dir != 2) ) { next_dir = 3; }
        int tx, ty;
        bool const tap = kk_tapped(&tx, &ty);
        if ( tap && (PLAY == mode) )
        {
            /* Tap beside the head to turn that way. */
            int const hx = OX + body[0].x * CELL, hy = OY + body[0].y * CELL;
            int const ddx = tx - hx, ddy = ty - hy;
            if ( (dir <= 1) && (ddx < -10) ) { next_dir = 2; }
            else if ( (dir <= 1) && (ddx > 10) ) { next_dir = 3; }
            else if ( (dir >= 2) && (ddy < -10) ) { next_dir = 0; }
            else if ( (dir >= 2) && (ddy > 10) ) { next_dir = 1; }
        }
        if ( (TITLE == mode) || (OVER == mode) )
        {
            if ( kk_pressed(KK_TOP) || tap )
            {
                reset();
                mode = PLAY;
                kk_led(0, 0, 0);
            }
        }
        else if ( (now - last_step) >= step_ms )
        {
            last_step = now;
            step();
        }

        kk_vgrad(0, 0, KK_W, KK_H, KK_RGB(18, 24, 18), KK_RGB(8, 10, 8));
        char buf[32];
        (void)snprintf(buf, sizeof(buf), "%d", score);
        kk_text_c(36, buf, 4, KK_WHITE);
        draw_field();
        if ( TITLE == mode )
        {
            kk_round(60, 170, 290, 160, 18, KK_RGB(10, 14, 10));
            kk_text_c(190, "SNAKE", 7, KK_RGB(130, 230, 100));
            kk_text_c(260, "TOP: START", 3, KK_WHITE);
            (void)snprintf(buf, sizeof(buf), "BEST %d", best);
            kk_text_c(295, buf, 2, KK_GREY);
        }
        else if ( OVER == mode )
        {
            kk_round(60, 170, 290, 160, 18, KK_RGB(10, 14, 10));
            kk_text_c(190, "OUCH!", 7, KK_RED);
            (void)snprintf(buf, sizeof(buf), "SCORE %d  BEST %d", score, best);
            kk_text_c(260, buf, 2, KK_WHITE);
            kk_text_c(295, "TOP: AGAIN", 2, KK_GREY);
        }
        kk_text_c(480, "TOP+BOT 1S: BACK", 1, KK_GREY);
        kk_end();
    }
}
