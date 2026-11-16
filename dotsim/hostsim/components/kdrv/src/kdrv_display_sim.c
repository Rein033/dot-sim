/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_display_sim.c
 * @brief The CO5300 panel, simulated: two framebuffers, flips, and a glass
 *        that the bridge sends to the browser.
 *
 * A flush is "done" one panel frame (KDRV_DISPLAY_FRAME_US) after it starts,
 * as on the glass, so an app that paces itself on kdrv_display_flush_wait()
 * runs at the Dot's own frame rate.
 */
#include "kdrv_display.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "kdotsim.h"

#define W KBSP_LCD_H_RES
#define H KBSP_LCD_V_RES

static uint16_t *s_fb[2];
static uint8_t s_back;           /* index of the back buffer */
static uint16_t *s_glass;        /* what the glass shows */
static bool s_up;
static int64_t s_done_at;        /* when the flush in flight completes, esp_timer us */
static atomic_bool s_in_flight;
static atomic_int s_dirty_y1 = H, s_dirty_y2 = 0;
static uint8_t s_brightness = 100U;
static bool s_lit;
static bool s_sleep;

static void dirty(int32_t y1, int32_t y2)
{
    int cur = atomic_load(&s_dirty_y1);
    while ( (y1 < cur) && !atomic_compare_exchange_weak(&s_dirty_y1, &cur, (int)y1) ) {}
    cur = atomic_load(&s_dirty_y2);
    while ( (y2 > cur) && !atomic_compare_exchange_weak(&s_dirty_y2, &cur, (int)y2) ) {}
}

static void present(int32_t y1, int32_t y2)
{
    if ( !s_lit )
    {
        s_lit = true;
        kdotsim_brightness(s_brightness);
    }
    if ( !s_sleep )
    {
        kdotsim_frame(s_glass, W, y1, y2);
    }
    dirty(y1, y2);
}

kode_status_t kdrv_display_init(void)
{
    if ( s_up )
    {
        return KODE_OK;
    }
    for ( unsigned i = 0U; i < 2U; ++i )
    {
        if ( posix_memalign((void **)&s_fb[i], KDRV_DISPLAY_CACHE_LINE, KDRV_DISPLAY_FB_BYTES) != 0 )
        {
            return KODE_NO_RESOURCES;
        }
        memset(s_fb[i], 0, KDRV_DISPLAY_FB_BYTES);
    }
    s_back = 1U;
    s_glass = s_fb[0];
    s_up = true;
    return KODE_OK;
}

kode_status_t kdrv_display_keep_for_restart(void) { return s_up ? KODE_OK : KODE_DENIED; }
bool kdrv_display_video_expected(void) { return false; }
uint32_t kdrv_display_recoveries(void) { return 0U; }
void kdrv_display_recoveries_set(uint32_t n) { (void)n; }

bool kdrv_display_flush_wait(uint32_t ms)
{
    if ( !atomic_load(&s_in_flight) )
    {
        return true;
    }
    int64_t const left = s_done_at - esp_timer_get_time();
    if ( left > 0 )
    {
        if ( (uint64_t)left > (uint64_t)ms * 1000U )
        {
            vTaskDelay(pdMS_TO_TICKS(ms));
            return false;
        }
        TickType_t t = pdMS_TO_TICKS((left + 999) / 1000);
        vTaskDelay((0U == t) ? 1U : t);
    }
    atomic_store(&s_in_flight, false);
    return true;
}

void kdrv_display_flush_abort(void)
{
    atomic_store(&s_in_flight, false);
}

kode_status_t kdrv_display_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const void *pixels, bool last)
{
    if ( !s_up )
    {
        return KODE_DENIED;
    }
    if ( (NULL == pixels) || (x1 < 0) || (y1 < 0) || (x2 > (int32_t)W) || (y2 > (int32_t)H) || (x1 >= x2) || (y1 >= y2) )
    {
        return KODE_DENIED;
    }
    int64_t const now = esp_timer_get_time();
    if ( (pixels == s_fb[0]) || (pixels == s_fb[1]) )
    {
        /* Drawn in place: the panel flips to that buffer. */
        uint8_t const idx = (pixels == s_fb[0]) ? 0U : 1U;
        s_glass = s_fb[idx];
        s_back = (uint8_t)(1U - idx);
        present(y1, y2);
    }
    else
    {
        /* A chunk: copied into the glass at its rectangle. */
        const uint16_t *src = pixels;
        int32_t const w = x2 - x1;
        for ( int32_t y = y1; y < y2; ++y )
        {
            memcpy(&s_glass[(uint32_t)y * W + (uint32_t)x1], &src[(uint32_t)(y - y1) * (uint32_t)w], (size_t)w * 2U);
        }
        if ( last )
        {
            present(0, (int32_t)H);
        }
    }
    s_done_at = last ? (now + KDRV_DISPLAY_FRAME_US) : now;
    atomic_store(&s_in_flight, true);
    return KODE_OK;
}

void *kdrv_display_back_buffer(void)
{
    return s_up ? s_fb[s_back] : NULL;
}

void *kdrv_display_front_buffer(void)
{
    return s_up ? s_glass : NULL;
}

kode_status_t kdrv_display_brightness(uint8_t percent)
{
    if ( percent > 100U )
    {
        percent = 100U;
    }
    s_brightness = percent;
    if ( s_lit )
    {
        kdotsim_brightness(percent);
    }
    return KODE_OK;
}

kode_status_t kdrv_display_link_set(bool up) { (void)up; return KODE_OK; }
uint32_t kdrv_display_buffers_epoch(void) { return 0U; }

kode_status_t kdrv_display_panel_sleep(bool sleep)
{
    s_sleep = sleep;
    kdotsim_brightness(sleep ? 0U : s_brightness);
    return KODE_OK;
}

kode_status_t kdrv_display_reset_low(void) { return KODE_OK; }

kode_status_t kdrv_display_gram_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *rgb565)
{
    if ( (NULL == rgb565) || ((uint32_t)x + w > W) || ((uint32_t)y + h > H) )
    {
        return KODE_DENIED;
    }
    for ( uint32_t r = 0U; r < h; ++r )
    {
        memcpy(&s_glass[(y + r) * W + x], &rgb565[r * w], (size_t)w * 2U);
    }
    present(y, (int32_t)y + h);
    return KODE_OK;
}

kode_status_t kdrv_display_capture_begin(const uint8_t **image, size_t *len)
{
    if ( (NULL == image) || (NULL == len) || !s_up )
    {
        return KODE_DENIED;
    }
    *image = (const uint8_t *)s_glass;
    *len = (size_t)W * H * 2U;
    return KODE_OK;
}

void kdrv_display_capture_end(void) {}
bool kdrv_display_capture_painted(void) { return s_lit; }
kode_status_t kdrv_display_capture_selftest(void) { return KODE_OK; }

kode_status_t kdrv_display_band(uint32_t y, uint32_t h, const uint8_t **rows, size_t *len)
{
    if ( (NULL == rows) || (NULL == len) || (0U == h) || (y + h > H) || !s_up )
    {
        return KODE_DENIED;
    }
    *rows = (const uint8_t *)&s_glass[y * W];
    *len = (size_t)h * W * 2U;
    return KODE_OK;
}

void kdrv_display_dirty_take(int32_t *y1, int32_t *y2)
{
    int const a = atomic_exchange(&s_dirty_y1, (int)H);
    int const b = atomic_exchange(&s_dirty_y2, 0);
    if ( NULL != y1 ) { *y1 = a; }
    if ( NULL != y2 ) { *y2 = b; }
}

void kdrv_display_link(kdrv_display_link_t *out)
{
    if ( NULL != out )
    {
        memset(out, 0, sizeof(*out));
    }
}
