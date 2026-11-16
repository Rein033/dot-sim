/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_main.c
 * @brief Krabbel: draw on the glass with a finger. LEFT/RIGHT pick a colour,
 *        UP/DOWN the brush, BOT erases, TOP saves to the storage, hold TOP to clear.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("@NAME@", "@CAT@");

#define PAL 10
#define BAR_Y 420

static const uint16_t PALETTE[PAL] = {
    KK_RGB(20, 20, 24), KK_WHITE, KK_RGB(230, 60, 70), KK_RGB(245, 140, 50), KK_RGB(250, 205, 50),
    KK_RGB(80, 200, 90), KK_RGB(60, 190, 220), KK_RGB(50, 110, 230), KK_RGB(150, 90, 220), KK_RGB(240, 110, 170),
};

static uint16_t *canvas;     /* the drawing, separate from the panel's two buffers */
static int color = 0, brush = 3;
static bool eraser;
static int lx = -1, ly = -1;
static uint32_t top_since;
static uint32_t toast_until;
static char toast[40];

static void clear_canvas(void)
{
    for ( int i = 0; i < KK_W * KK_H; ++i ) { canvas[i] = KK_RGB(246, 242, 230); }
}

static void dab(int x, int y, int r, uint16_t c)
{
    for ( int dy = -r; dy <= r; ++dy )
    {
        for ( int dx = -r; dx <= r; ++dx )
        {
            int const px = x + dx, py = y + dy;
            if ( (dx * dx + dy * dy <= r * r) && ((unsigned)px < KK_W) && ((unsigned)py < BAR_Y - 6) )
            {
                canvas[py * KK_W + px] = c;
            }
        }
    }
}

static void stroke(int x0, int y0, int x1, int y1)
{
    int const r = eraser ? brush * 3 : brush;
    uint16_t const c = eraser ? KK_RGB(246, 242, 230) : PALETTE[color];
    int const dx = x1 - x0, dy = y1 - y0;
    int const n = 1 + (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 2;
    for ( int i = 0; i <= n; ++i ) { dab(x0 + dx * i / n, y0 + dy * i / n, r, c); }
}

static void save(void)
{
    /* A plain PPM in the app's data folder on the internal storage. */
    (void)kode_file_create_folder("flash/krabbel");
    char path[64];
    (void)snprintf(path, sizeof(path), "flash/krabbel/k%lu.ppm", (unsigned long)(kk_ms() / 1000));
    static uint8_t row[KK_W * 3];
    char head[32];
    int const hl = snprintf(head, sizeof(head), "P6\n%d %d\n255\n", KK_W, BAR_Y - 6);
    bool ok = kode_file_write(path, head);
    for ( int y = 0; ok && (y < BAR_Y - 6); ++y )
    {
        for ( int x = 0; x < KK_W; ++x )
        {
            uint16_t const c = canvas[y * KK_W + x];
            row[x * 3] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            row[x * 3 + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            row[x * 3 + 2] = (uint8_t)((c & 31) * 255 / 31);
        }
        ok = kode_file_append_bytes(path, row, sizeof(row));
    }
    (void)hl;
    (void)snprintf(toast, sizeof(toast), ok ? "SAVED" : "SAVE FAILED");
    toast_until = kk_ms() + 1500;
    kk_tone(ok ? 880 : 200, 80);
}

void app_main(void)
{
    kk_init();
    canvas = malloc(sizeof(uint16_t) * KK_W * KK_H);
    if ( NULL == canvas ) { for ( ;; ) { kk_begin(); kk_clear(KK_RED); kk_end(); } }
    clear_canvas();
    for ( ;; )
    {
        kk_begin();
        if ( kk_pressed(KK_LEFT) ) { color = (color + PAL - 1) % PAL; eraser = false; kk_tone(500, 10); }
        if ( kk_pressed(KK_RIGHT) ) { color = (color + 1) % PAL; eraser = false; kk_tone(500, 10); }
        if ( kk_pressed(KK_UP) && (brush < 14) ) { brush += 1; }
        if ( kk_pressed(KK_DOWN) && (brush > 1) ) { brush -= 1; }
        if ( kk_pressed(KK_BOT) ) { eraser = !eraser; kk_tone(300, 10); }
        if ( kk_held() & KK_TOP )
        {
            if ( 0U == top_since ) { top_since = kk_ms(); }
            else if ( (kk_ms() - top_since) > 900 && (top_since != 1U) )
            {
                clear_canvas();
                top_since = 1U;   /* cleared; wait for the release */
                kk_tone(200, 120);
            }
        }
        else
        {
            if ( (top_since > 1U) && ((kk_ms() - top_since) <= 900) ) { save(); }
            top_since = 0U;
        }
        int tx, ty;
        if ( kk_touch(&tx, &ty) )
        {
            if ( ty >= BAR_Y )
            {
                /* the palette */
                int const i = (tx - 30) / 35;
                if ( (i >= 0) && (i < PAL) && (lx < 0) ) { color = i; eraser = false; }
            }
            else
            {
                if ( lx < 0 ) { stroke(tx, ty, tx, ty); } else { stroke(lx, ly, tx, ty); }
                lx = tx;
                ly = ty;
            }
        }
        else
        {
            lx = -1;
        }

        /* The canvas, copied into the frame, then the bar. */
        kk_blit(canvas);
        kk_rect(0, BAR_Y - 6, KK_W, KK_H - BAR_Y + 6, KK_RGB(36, 36, 44));
        for ( int i = 0; i < PAL; ++i )
        {
            int const x = 30 + i * 35 + 16, y = BAR_Y + 20;
            if ( (i == color) && !eraser ) { kk_disc(x, y, 16, KK_WHITE); }
            kk_disc(x, y, 13, PALETTE[i]);
        }
        char buf[40];
        (void)snprintf(buf, sizeof(buf), eraser ? "ERASER %d" : "BRUSH %d", brush);
        kk_text(110, BAR_Y + 46, buf, 1, KK_GREY);
        kk_disc(270, BAR_Y + 49, eraser ? brush * 3 : brush, eraser ? KK_WHITE : PALETTE[color]);
        if ( kk_ms() < toast_until )
        {
            kk_round(120, 40, 170, 36, 10, KK_RGB(20, 20, 24));
            kk_text_c(51, toast, 2, KK_WHITE);
        }
        kk_end();
    }
}
