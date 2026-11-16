/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Muurbreker: bounce the ball and break every brick. LEFT/RIGHT or a
 *        finger moves the bat, TOP launches.
 */
#include <math.h>
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define BCOLS 8
#define BROWS 6
#define BW 40
#define BH 16
#define BX0 ((KK_W - BCOLS * BW) / 2)
#define BY0 110
#define PADY 430
#define BALLR 6

static uint8_t bricks[BROWS][BCOLS];
static float px, pw, bx, by, vx, vy;
static int lives, score, level, best, left;
static bool stuck;
static enum { TITLE, PLAY, OVER, WON } mode;
static const uint16_t ROWC[BROWS] = {
    KK_RGB(230, 70, 80), KK_RGB(245, 140, 50), KK_RGB(250, 205, 50),
    KK_RGB(90, 200, 100), KK_RGB(60, 170, 230), KK_RGB(160, 100, 230),
};

static void build_level(void)
{
    left = 0;
    for ( int r = 0; r < BROWS; ++r )
    {
        for ( int c = 0; c < BCOLS; ++c )
        {
            /* Later levels leave gaps and make the top rows take two hits. */
            bool const gap = (level > 1) && (((r + c + level) % 5) == 0);
            bricks[r][c] = gap ? 0 : (uint8_t)(((level > 2) && (r < 2)) ? 2 : 1);
            if ( bricks[r][c] ) { ++left; }
        }
    }
}

static void serve(void)
{
    stuck = true;
    pw = 70;
    bx = px;
    by = PADY - BALLR - 1;
}

static void new_game(void)
{
    lives = 3;
    score = 0;
    level = 1;
    px = KK_W / 2;
    build_level();
    serve();
}

static void launch(void)
{
    float const sp = 5.0f + (float)level * 0.6f;
    float const a = -1.1f + (float)kk_rand(100) / 100.0f * 0.5f;
    vx = sp * sinf(a + 0.25f);
    vy = -sp * cosf(a + 0.25f);
    stuck = false;
}

static void hit_brick(int r, int c)
{
    if ( --bricks[r][c] == 0 )
    {
        --left;
        score += 10 * (BROWS - r);
        kk_tone((uint16_t)(400 + 80 * (BROWS - r)), 30);
    }
    else
    {
        kk_tone(300, 30);
    }
}

static void physics(void)
{
    float const steps = 4.0f;
    for ( int i = 0; i < (int)steps; ++i )
    {
        bx += vx / steps;
        by += vy / steps;
        if ( bx < 30 + BALLR ) { bx = 30 + BALLR; vx = -vx; kk_tone(200, 10); }
        if ( bx > KK_W - 30 - BALLR ) { bx = KK_W - 30 - BALLR; vx = -vx; kk_tone(200, 10); }
        if ( by < 70 + BALLR ) { by = 70 + BALLR; vy = -vy; kk_tone(200, 10); }
        /* the bat */
        if ( (vy > 0) && (by + BALLR >= PADY) && (by + BALLR <= PADY + 12) && (bx > px - pw / 2 - BALLR) && (bx < px + pw / 2 + BALLR) )
        {
            float const off = (bx - px) / (pw / 2);   /* -1 .. 1 */
            float const sp = sqrtf(vx * vx + vy * vy);
            float const a = off * 1.05f;
            vx = sp * sinf(a);
            vy = -sp * cosf(a);
            by = PADY - BALLR;
            kk_tone(520, 15);
        }
        /* the bricks */
        int const c = (int)((bx - BX0) / BW), r = (int)((by - BY0) / BH);
        if ( (r >= 0) && (r < BROWS) && (c >= 0) && (c < BCOLS) && (bx >= BX0) && (by >= BY0) && bricks[r][c] )
        {
            float const cx = BX0 + c * BW + BW / 2.0f, cy = BY0 + r * BH + BH / 2.0f;
            if ( fabsf(bx - cx) / BW > fabsf(by - cy) / BH ) { vx = -vx; } else { vy = -vy; }
            hit_brick(r, c);
            break;
        }
        if ( by > KK_H + 20 )
        {
            if ( --lives <= 0 )
            {
                mode = OVER;
                if ( score > best ) { best = score; kk_best_put("muur", best); }
                kk_tone(150, 400);
                kk_led(90, 0, 0);
            }
            else
            {
                kk_tone(180, 200);
                serve();
            }
            return;
        }
    }
    if ( 0 == left )
    {
        ++level;
        score += 100;
        kk_tone(1200, 120);
        kk_led(0, 80, 0);
        if ( level > 5 ) { mode = WON; if ( score > best ) { best = score; kk_best_put("muur", best); } return; }
        build_level();
        serve();
    }
}

static void draw(void)
{
    kk_vgrad(0, 0, KK_W, KK_H, KK_RGB(20, 22, 44), KK_RGB(6, 6, 14));
    char buf[40];
    (void)snprintf(buf, sizeof(buf), "%d", score);
    kk_text(60, 40, buf, 3, KK_WHITE);
    (void)snprintf(buf, sizeof(buf), "LVL %d", level);
    kk_text(KK_W - 60 - kk_text_w(buf, 2), 44, buf, 2, KK_GREY);
    for ( int i = 0; i < lives; ++i ) { kk_disc(190 + i * 16, 50, 5, KK_ORANGE); }
    for ( int r = 0; r < BROWS; ++r )
    {
        for ( int c = 0; c < BCOLS; ++c )
        {
            if ( !bricks[r][c] ) { continue; }
            int const x = BX0 + c * BW, y = BY0 + r * BH;
            uint16_t const col = (2 == bricks[r][c]) ? KK_RGB(200, 200, 210) : ROWC[r];
            kk_rect(x + 1, y + 1, BW - 2, BH - 2, col);
            kk_rect(x + 1, y + 1, BW - 2, 3, kk_mix(col, KK_WHITE, 110));
            kk_rect(x + 1, y + BH - 4, BW - 2, 3, kk_mix(col, KK_BLACK, 90));
        }
    }
    kk_round((int)(px - pw / 2), PADY, (int)pw, 12, 6, KK_CYAN);
    kk_rect((int)(px - pw / 2) + 6, PADY + 2, (int)pw - 12, 3, KK_WHITE);
    kk_disc((int)bx, (int)by, BALLR, KK_WHITE);
}

static void panel(const char *big, const char *big2, uint16_t col, const char *l1, const char *l2)
{
    int const h = big2 ? 190 : 150;
    kk_round(55, 190, 300, h, 18, KK_RGB(8, 8, 20));
    kk_box(55, 190, 300, h, 2, col);
    kk_text_c(210, big, 5, col);
    int y = 270;
    if ( big2 ) { kk_text_c(255, big2, 5, col); y = 310; }
    kk_text_c(y, l1, 2, KK_WHITE);
    kk_text_c(y + 30, l2, 2, KK_GREY);
}

void app_main(void)
{
    kk_init();
    best = (int)kk_best_get("muur");
    new_game();
    mode = TITLE;
    for ( ;; )
    {
        kk_begin();
        uint32_t const h = kk_held();
        int tx, ty;
        if ( kk_touch(&tx, &ty) && (PLAY == mode) ) { px += ((float)tx - px) * 0.35f; }
        if ( h & KK_LEFT ) { px -= 8; }
        if ( h & KK_RIGHT ) { px += 8; }
        if ( px < 30 + pw / 2 ) { px = 30 + pw / 2; }
        if ( px > KK_W - 30 - pw / 2 ) { px = KK_W - 30 - pw / 2; }
        bool const go = kk_pressed(KK_TOP) || kk_tapped(NULL, NULL);
        if ( PLAY == mode )
        {
            if ( stuck ) { bx = px; if ( go ) { launch(); } }
            else { physics(); }
        }
        else if ( go )
        {
            new_game();
            mode = PLAY;
            kk_led(0, 0, 0);
        }
        draw();
        char buf[40];
        if ( TITLE == mode )
        {
            (void)snprintf(buf, sizeof(buf), "BEST %d", best);
            panel("WALL", "BREAKER", KK_ORANGE, "TOP: START", buf);
        }
        else if ( OVER == mode )
        {
            (void)snprintf(buf, sizeof(buf), "SCORE %d  BEST %d", score, best);
            panel("OOPS", NULL, KK_RED, buf, "TOP: AGAIN");
        }
        else if ( WON == mode )
        {
            (void)snprintf(buf, sizeof(buf), "SCORE %d", score);
            panel("YOU WIN", NULL, KK_GREEN, buf, "TOP: PLAY AGAIN");
        }
        else if ( stuck )
        {
            kk_text_c(370, "TOP: LAUNCH", 2, KK_GREY);
        }
        kk_end();
    }
}
