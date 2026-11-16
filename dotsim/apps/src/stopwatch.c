/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Stopwatch: TOP starts and stops, BOT takes a lap while running and
 *        resets while stopped. A tap on the glass does what TOP does.
 */
#include <math.h>
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define MAXLAP 99
#define CX (KK_W / 2)
#define CY 190
#define RR 118

static bool running;
static uint32_t start_ms, acc_ms;
static uint32_t laps[MAXLAP];
static int nlap;

static uint32_t elapsed(void) { return acc_ms + (running ? (kk_ms() - start_ms) : 0U); }

static void fmt(char *buf, size_t cap, uint32_t ms, bool cs)
{
    uint32_t const m = ms / 60000U, s = (ms / 1000U) % 60U, c = (ms / 10U) % 100U;
    if ( cs ) { (void)snprintf(buf, cap, "%02lu:%02lu.%02lu", (unsigned long)m, (unsigned long)s, (unsigned long)c); }
    else { (void)snprintf(buf, cap, "%02lu:%02lu", (unsigned long)m, (unsigned long)s); }
}

static void toggle(void)
{
    if ( running ) { acc_ms += kk_ms() - start_ms; running = false; kk_tone(440, 40); kk_led(60, 0, 0); }
    else { start_ms = kk_ms(); running = true; kk_tone(880, 40); kk_led(0, 60, 0); }
}

void app_main(void)
{
    kk_init();
    for ( ;; )
    {
        kk_begin();
        if ( kk_pressed(KK_TOP) || kk_tapped(NULL, NULL) ) { toggle(); }
        if ( kk_pressed(KK_BOT) )
        {
            if ( running ) { if ( nlap < MAXLAP ) { laps[nlap++] = elapsed(); } kk_tone(660, 30); }
            else { acc_ms = 0; nlap = 0; kk_tone(330, 30); kk_led(0, 0, 0); }
        }
        uint32_t const t = elapsed();

        kk_vgrad(0, 0, KK_W, KK_H, KK_RGB(24, 28, 36), KK_RGB(10, 12, 16));
        /* the dial: 60 ticks, a sweep for the seconds */
        kk_disc(CX, CY, RR + 10, KK_RGB(34, 40, 52));
        kk_disc(CX, CY, RR, KK_RGB(16, 18, 24));
        for ( int i = 0; i < 60; ++i )
        {
            float const a = (float)i * 6.2831853f / 60.0f;
            int const in = (i % 5) ? (RR - 8) : (RR - 16);
            kk_line(CX + (int)(sinf(a) * in), CY - (int)(cosf(a) * in), CX + (int)(sinf(a) * (RR - 3)), CY - (int)(cosf(a) * (RR - 3)), (i % 5) ? 1 : 3,
                    (i % 5) ? KK_GREY : KK_WHITE);
        }
        float const sec = (float)(t % 60000U) / 1000.0f;
        float const a = sec * 6.2831853f / 60.0f;
        kk_line(CX, CY, CX + (int)(sinf(a) * (RR - 20)), CY - (int)(cosf(a) * (RR - 20)), 3, KK_ORANGE);
        kk_disc(CX, CY, 7, KK_ORANGE);
        char buf[24];
        fmt(buf, sizeof(buf), t, true);
        kk_text_c(CY + 40, buf, 3, running ? KK_WHITE : KK_GREY);

        /* laps: newest first, with the split since the one before */
        kk_text(60, 340, running ? "TOP STOP   BOT LAP" : "TOP START  BOT RESET", 1, KK_GREY);
        int y = 360;
        for ( int i = nlap - 1; (i >= 0) && (y < 470); --i, y += 20 )
        {
            char a1[24], a2[24], line[64];
            fmt(a1, sizeof(a1), laps[i], true);
            fmt(a2, sizeof(a2), laps[i] - ((i > 0) ? laps[i - 1] : 0U), true);
            (void)snprintf(line, sizeof(line), "%2d  %s  +%s", i + 1, a1, a2);
            kk_text(60, y, line, 2, (i == nlap - 1) ? KK_WHITE : KK_GREY);
        }
        kk_end();
    }
}
