/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Reflex: wait for green, then press TOP (or tap) as fast as you can.
 *        Five tries, then your average. Too early counts as a miss.
 */
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define TRIES 5

static enum { TITLE, WAIT, GO, SHOWN, EARLY, DONE } mode;
static uint32_t go_at, shown_at, t_go;
static int tries, results[TRIES], best;
static int last;

static void arm(void)
{
    mode = WAIT;
    go_at = kk_ms() + 1200 + kk_rand(2800);
    kk_led(60, 0, 0);
}

void app_main(void)
{
    kk_init();
    best = (int)kk_best_get("reflex");
    mode = TITLE;
    for ( ;; )
    {
        kk_begin();
        uint32_t const now = kk_ms();
        bool const hit = kk_pressed(KK_TOP) || kk_pressed(KK_BOT) || kk_tapped(NULL, NULL);
        switch ( mode )
        {
        case TITLE:
        case DONE:
            if ( hit ) { tries = 0; arm(); }
            break;
        case WAIT:
            if ( hit ) { mode = EARLY; shown_at = now; kk_tone(150, 250); kk_led(80, 40, 0); }
            else if ( now >= go_at ) { mode = GO; t_go = now; kk_tone(1000, 60); kk_led(0, 90, 0); }
            break;
        case GO:
            if ( hit )
            {
                last = (int)(now - t_go);
                results[tries++] = last;
                mode = SHOWN;
                shown_at = now;
                kk_led(0, 0, 0);
            }
            else if ( (now - t_go) > 2000U ) { last = 2000; results[tries++] = last; mode = SHOWN; shown_at = now; }
            break;
        case SHOWN:
        case EARLY:
            if ( (now - shown_at) > 1200U )
            {
                if ( tries >= TRIES )
                {
                    int sum = 0;
                    for ( int i = 0; i < TRIES; ++i ) { sum += results[i]; }
                    last = sum / TRIES;
                    if ( (0 == best) || (last < best) ) { best = last; kk_best_put("reflex", best); kk_tone(1320, 200); }
                    mode = DONE;
                }
                else { arm(); }
            }
            break;
        }

        char buf[40];
        if ( WAIT == mode )
        {
            kk_clear(KK_RGB(150, 30, 40));
            kk_text_c(200, "WAIT...", 5, KK_WHITE);
        }
        else if ( GO == mode )
        {
            kk_clear(KK_RGB(40, 180, 80));
            kk_text_c(200, "NOW!", 9, KK_WHITE);
        }
        else if ( EARLY == mode )
        {
            kk_clear(KK_RGB(230, 140, 40));
            kk_text_c(200, "TOO EARLY", 5, KK_WHITE);
            kk_text_c(260, "TRY AGAIN", 2, KK_WHITE);
        }
        else if ( SHOWN == mode )
        {
            kk_clear(KK_RGB(30, 60, 110));
            (void)snprintf(buf, sizeof(buf), "%d MS", last);
            kk_text_c(190, buf, 6, KK_WHITE);
            const char *verdict = (last < 200) ? "LIGHTNING" : (last < 260) ? "FAST" : (last < 350) ? "GOOD" : "SLOW";
            kk_text_c(260, verdict, 3, KK_YELLOW);
        }
        else
        {
            kk_vgrad(0, 0, KK_W, KK_H, KK_RGB(30, 34, 50), KK_RGB(12, 14, 20));
            kk_text_c(80, "REFLEX", 6, KK_WHITE);
            if ( DONE == mode )
            {
                (void)snprintf(buf, sizeof(buf), "AVERAGE %d MS", last);
                kk_text_c(170, buf, 3, KK_YELLOW);
                for ( int i = 0; i < TRIES; ++i )
                {
                    (void)snprintf(buf, sizeof(buf), "%d. %d MS", i + 1, results[i]);
                    kk_text_c(220 + i * 24, buf, 2, KK_GREY);
                }
            }
            else
            {
                kk_text_c(170, "WAIT FOR GREEN,", 2, KK_GREY);
                kk_text_c(196, "THEN PRESS TOP.", 2, KK_GREY);
            }
            if ( best ) { (void)snprintf(buf, sizeof(buf), "RECORD %d MS", best); kk_text_c(370, buf, 2, KK_GREEN); }
            kk_text_c(410, "TOP: START", 3, KK_WHITE);
        }
        if ( (WAIT == mode) || (GO == mode) || (SHOWN == mode) || (EARLY == mode) )
        {
            (void)snprintf(buf, sizeof(buf), "%d / %d", tries + ((SHOWN == mode) ? 0 : 1) > TRIES ? TRIES : tries + ((SHOWN == mode) ? 0 : 1), TRIES);
            kk_text_c(420, buf, 2, KK_WHITE);
        }
        kk_end();
    }
}
