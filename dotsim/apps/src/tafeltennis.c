/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Tafeltennis: you below, the Dot above, first to 7.
 *        LEFT/RIGHT or a finger moves your bat, TOP serves.
 */
#include <math.h>
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define TBL_X 50
#define TBL_W (KK_W - 2 * TBL_X)
#define TBL_Y 70
#define TBL_H 362
#define BATW 64
#define BATH 10
#define YOU_Y (TBL_Y + TBL_H - 22)
#define CPU_Y (TBL_Y + 12)
#define R 7
#define WIN 7

static float you_x, cpu_x, bx, by, vx, vy;
static int you, cpu, rally, level;
static bool serving, you_serve;
static uint32_t pause_until;
static enum { TITLE, PLAY, OVER } mode;

static void serve_pos(void)
{
    serving = true;
    bx = you_serve ? you_x : cpu_x;
    by = you_serve ? (YOU_Y - R - 1) : (CPU_Y + BATH + R + 1);
    rally = 0;
}

static void launch(void)
{
    float const sp = 4.5f;
    vx = (float)((int)kk_rand(5) - 2) * 0.8f;
    vy = you_serve ? -sp : sp;
    serving = false;
    kk_tone(500, 20);
}

static void point(bool for_you)
{
    if ( for_you ) { ++you; kk_tone(990, 120); kk_led(0, 60, 0); }
    else { ++cpu; kk_tone(220, 160); kk_led(60, 0, 0); }
    you_serve = !for_you;
    pause_until = kk_ms() + 700;
    if ( (you >= WIN) || (cpu >= WIN) ) { mode = OVER; }
    serve_pos();
}

static void bounce_bat(float bat_x, bool from_below)
{
    float const off = (bx - bat_x) / (BATW / 2.0f);
    float sp = sqrtf(vx * vx + vy * vy) + 0.25f;
    if ( sp > 11 ) { sp = 11; }
    float const a = off * 0.9f;
    vx = sp * sinf(a);
    vy = (from_below ? -1.0f : 1.0f) * sp * cosf(a);
    ++rally;
    kk_tone(from_below ? 620 : 470, 15);
}

static void update(void)
{
    /* the Dot follows the ball, a little late, a little short of perfect */
    float const target = (vy < 0) ? bx : (KK_W / 2.0f);
    float const cpu_speed = 3.2f + (float)level * 0.6f;
    float const d = target - cpu_x;
    cpu_x += (d > cpu_speed) ? cpu_speed : ((d < -cpu_speed) ? -cpu_speed : d);
    if ( serving )
    {
        if ( !you_serve ) { bx = cpu_x; if ( kk_ms() > pause_until ) { launch(); } }
        else { bx = you_x; }
        return;
    }
    for ( int s = 0; s < 3; ++s )
    {
        bx += vx / 3;
        by += vy / 3;
        if ( bx < TBL_X + R ) { bx = TBL_X + R; vx = -vx; }
        if ( bx > TBL_X + TBL_W - R ) { bx = TBL_X + TBL_W - R; vx = -vx; }
        if ( (vy > 0) && (by + R >= YOU_Y) && (by + R <= YOU_Y + BATH + 6) && (fabsf(bx - you_x) < BATW / 2.0f + R) )
        {
            by = YOU_Y - R;
            bounce_bat(you_x, true);
        }
        if ( (vy < 0) && (by - R <= CPU_Y + BATH) && (by - R >= CPU_Y - 6) && (fabsf(bx - cpu_x) < BATW / 2.0f + R) )
        {
            by = CPU_Y + BATH + R;
            bounce_bat(cpu_x, false);
        }
        if ( by > TBL_Y + TBL_H + 20 ) { point(false); return; }
        if ( by < TBL_Y - 20 ) { point(true); return; }
    }
}

static void bat(float x, int y, uint16_t c)
{
    kk_round((int)x - BATW / 2, y, BATW, BATH, 5, c);
    kk_rect((int)x - BATW / 2 + 5, y + 2, BATW - 10, 2, kk_mix(c, KK_WHITE, 120));
}

static void draw(void)
{
    kk_clear(KK_RGB(26, 30, 38));
    kk_round(TBL_X - 6, TBL_Y - 6, TBL_W + 12, TBL_H + 12, 10, KK_RGB(200, 200, 205));
    kk_rect(TBL_X, TBL_Y, TBL_W, TBL_H, KK_RGB(20, 90, 150));
    kk_rect(TBL_X + TBL_W / 2 - 1, TBL_Y, 2, TBL_H, KK_RGB(200, 210, 220));
    for ( int x = TBL_X; x < TBL_X + TBL_W; x += 14 ) { kk_rect(x, TBL_Y + TBL_H / 2 - 2, 8, 4, KK_WHITE); }
    char buf[8];
    (void)snprintf(buf, sizeof(buf), "%d", cpu);
    kk_text(TBL_X + 12, TBL_Y + TBL_H / 2 - 50, buf, 5, KK_RGB(120, 170, 210));
    (void)snprintf(buf, sizeof(buf), "%d", you);
    kk_text(TBL_X + 12, TBL_Y + TBL_H / 2 + 16, buf, 5, KK_RGB(230, 240, 250));
    bat(cpu_x, CPU_Y, KK_RED);
    bat(you_x, YOU_Y, KK_BLUE);
    kk_disc((int)bx + 2, (int)by + 3, R, KK_RGB(10, 50, 90));
    kk_disc((int)bx, (int)by, R, KK_RGB(250, 245, 230));
}

void app_main(void)
{
    kk_init();
    level = 1;
    you_x = cpu_x = KK_W / 2;
    mode = TITLE;
    you_serve = true;
    serve_pos();
    for ( ;; )
    {
        kk_begin();
        uint32_t const h = kk_held();
        if ( h & KK_LEFT ) { you_x -= 7; }
        if ( h & KK_RIGHT ) { you_x += 7; }
        int tx, ty;
        if ( kk_touch(&tx, &ty) ) { you_x += ((float)tx - you_x) * 0.4f; }
        if ( you_x < TBL_X + BATW / 2 ) { you_x = TBL_X + BATW / 2; }
        if ( you_x > TBL_X + TBL_W - BATW / 2 ) { you_x = TBL_X + TBL_W - BATW / 2; }
        bool const go = kk_pressed(KK_TOP) || kk_tapped(NULL, NULL);
        if ( PLAY == mode )
        {
            if ( serving && you_serve && go && (kk_ms() > pause_until) ) { launch(); }
            update();
            if ( kk_ms() > pause_until ) { kk_led(0, 0, 0); }
        }
        else if ( go )
        {
            if ( OVER == mode ) { level = (you > cpu) ? (level + 1) : level; }
            you = cpu = 0;
            you_serve = true;
            serve_pos();
            mode = PLAY;
            kk_led(0, 0, 0);
        }
        draw();
        if ( TITLE == mode )
        {
            kk_round(55, 170, 300, 150, 16, KK_RGB(12, 16, 22));
            kk_text_c(190, "TABLE", 5, KK_BLUE);
            kk_text_c(232, "TENNIS", 5, KK_BLUE);
            kk_text_c(290, "TOP: SERVE", 2, KK_WHITE);
        }
        else if ( OVER == mode )
        {
            char buf[32];
            kk_round(55, 170, 300, 150, 16, KK_RGB(12, 16, 22));
            kk_text_c(190, (you > cpu) ? "YOU WIN" : "YOU LOSE", 4, (you > cpu) ? KK_GREEN : KK_RED);
            (void)snprintf(buf, sizeof(buf), "%d - %d", you, cpu);
            kk_text_c(240, buf, 3, KK_WHITE);
            kk_text_c(285, (you > cpu) ? "TOP: NEXT LEVEL" : "TOP: REMATCH", 2, KK_GREY);
        }
        else if ( serving && you_serve && (kk_ms() > pause_until) )
        {
            kk_text_c(YOU_Y - 40, "TOP: SERVE", 2, KK_WHITE);
        }
        kk_end();
    }
}
