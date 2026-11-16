/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_es8311_sim.c
 * @brief The ES8311 codec, simulated: tones and PCM go to the browser, the
 *        microphone hears silence. kes_fields.c (the SDK's field table) runs
 *        on top of the register file here.
 */
#include "kdrv_es8311.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "kes_fields.h"
#include "kdotsim.h"

static uint8_t s_reg[256];
static bool s_init;
static bool s_open;
static atomic_bool s_busy;
static int16_t *s_buf;
static kdrv_es8311_result_t s_result;
static atomic_bool s_stream_end;
static atomic_bool s_record_end;
static atomic_bool s_hush;
static int64_t s_stream_until;   /* esp_timer us at which the queued audio ends */

kode_status_t kdrv_es8311_init(void)
{
    if ( !s_init )
    {
        s_reg[0xFD] = KDRV_ES8311_ID1;
        s_reg[0xFE] = KDRV_ES8311_ID2;
        s_reg[0xFF] = 0x00U;
        s_reg[0x32] = 0xBFU;   /* DAC volume 0 dB */
        s_buf = calloc(KDRV_ES8311_BUF_FRAMES, sizeof(int16_t));
        if ( NULL == s_buf )
        {
            return KODE_NO_RESOURCES;
        }
        s_init = true;
    }
    return KODE_OK;
}

kode_status_t kdrv_es8311_park(void)
{
    s_open = false;
    return KODE_OK;
}

static uint8_t volume(void)
{
    kes_settings_t s;
    kes_settings(&s);
    return s.vol;
}

void kdrv_es8311_info(kdrv_es8311_info_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->rate = KDRV_ES8311_RATE_HZ;
    out->present = s_init;
    out->open = s_open;
    out->busy = atomic_load(&s_busy);
    out->bits = KDRV_ES8311_BITS;
    kes_settings_t s;
    kes_settings(&s);
    out->volume = s.vol;
    out->mic_db = s.mic_db;
    out->mute = s.out_mute;
}

bool kdrv_es8311_present(void) { return s_init; }
bool kdrv_es8311_is_open(void) { return s_open; }

int16_t *kdrv_es8311_buf(uint32_t *frames)
{
    if ( NULL != frames )
    {
        *frames = (NULL != s_buf) ? KDRV_ES8311_BUF_FRAMES : 0U;
    }
    return s_buf;
}

uint8_t *kdrv_es8311_buf_file(uint32_t frames, uint32_t *bytes)
{
    /* The recording as a WAV image, in place: the header is the caller's business in kwav. */
    if ( NULL != bytes )
    {
        *bytes = frames * 2U;
    }
    return (uint8_t *)s_buf;
}

/** @brief Sleep @p ms in slices, stopping early on a hush. */
static void hold(uint32_t ms)
{
    int64_t const end = esp_timer_get_time() + (int64_t)ms * 1000;
    while ( !atomic_load(&s_hush) && (esp_timer_get_time() < end) )
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void pcm_out(const int16_t *buf, uint32_t frames)
{
    uint32_t const step = KDRV_ES8311_RATE_HZ / 10U;   /* 100 ms per record */
    for ( uint32_t i = 0U; (i < frames) && !atomic_load(&s_hush); i += step )
    {
        uint32_t const n = ((frames - i) < step) ? (frames - i) : step;
        kdotsim_pcm(&buf[i], n, KDRV_ES8311_RATE_HZ);
        hold((n * 1000U) / KDRV_ES8311_RATE_HZ);
    }
}

kode_status_t kdrv_es8311_run(const kdrv_es8311_req_t *req)
{
    if ( (NULL == req) || !s_init )
    {
        return KODE_DENIED;
    }
    atomic_store(&s_busy, true);
    atomic_store(&s_hush, false);
    kode_status_t rc = KODE_OK;
    memset(&s_result, 0, sizeof(s_result));
    s_result.bin = req->bin;
    switch ( req->type )
    {
    case KDRV_ES8311_JOB_OPEN:
        s_open = true;
        break;
    case KDRV_ES8311_JOB_CLOSE:
        s_open = false;
        break;
    case KDRV_ES8311_JOB_TONE:
        kdotsim_tone(ktone_bin_hz(req->bin), req->ms, volume());
        hold(req->ms);
        s_result.frames = (uint32_t)req->ms * (KDRV_ES8311_RATE_HZ / 1000U);
        break;
    case KDRV_ES8311_JOB_PLAY:
        if ( NULL != req->buf )
        {
            pcm_out(req->buf, req->frames);
        }
        s_result.frames = req->frames;
        break;
    case KDRV_ES8311_JOB_FLOOR:
    case KDRV_ES8311_JOB_PROBE:
        /* The microphone hears a quiet room. */
        for ( unsigned i = 0U; i < KTONE_BIN_N; ++i )
        {
            s_result.win.bin_x10[i] = -900;
        }
        s_result.win.rms_x10 = -800;
        s_result.win.peak_x10 = -700;
        s_result.win.peak_bin = req->bin;
        if ( KDRV_ES8311_JOB_PROBE == req->type )
        {
            s_result.win.bin_x10[req->bin % KTONE_BIN_N] = -150;   /* the loopback hears its own tone */
        }
        hold(req->ms ? req->ms : 100U);
        break;
    case KDRV_ES8311_JOB_RECORD:
    {
        atomic_store(&s_record_end, false);
        uint32_t frames = 0U;
        uint32_t const cap = (NULL != req->buf) ? req->frames : 0U;
        int64_t const t0 = esp_timer_get_time();
        while ( !atomic_load(&s_record_end) && !atomic_load(&s_hush) && (frames < cap) )
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            uint64_t const ms = (uint64_t)(esp_timer_get_time() - t0) / 1000U;
            uint32_t const want = (uint32_t)(ms * (KDRV_ES8311_RATE_HZ / 1000U));
            uint32_t const upto = (want < cap) ? want : cap;
            if ( upto > frames )
            {
                memset(&req->buf[frames], 0, (size_t)(upto - frames) * sizeof(int16_t));
                frames = upto;
            }
            if ( (0U != req->ms) && (ms >= req->ms) )
            {
                break;
            }
        }
        s_result.frames = frames;
        break;
    }
    case KDRV_ES8311_JOB_STREAM:
        atomic_store(&s_stream_end, false);
        s_stream_until = esp_timer_get_time();
        while ( !atomic_load(&s_stream_end) && !atomic_load(&s_hush) )
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        /* Let what was queued play out. */
        while ( !atomic_load(&s_hush) && (esp_timer_get_time() < s_stream_until) )
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        break;
    default:
        rc = KODE_DENIED;
        break;
    }
    atomic_store(&s_busy, false);
    return rc;
}

void kdrv_es8311_result(kdrv_es8311_result_t *out)
{
    if ( NULL != out )
    {
        *out = s_result;
    }
}

kode_status_t kdrv_es8311_stream_reset(void)
{
    atomic_store(&s_stream_end, false);
    s_stream_until = esp_timer_get_time();
    return KODE_OK;
}

kode_status_t kdrv_es8311_stream_write(const int16_t *frames, size_t n, uint8_t channels, uint32_t ms, size_t *taken)
{
    if ( (NULL == frames) || (0U == channels) || (channels > 2U) )
    {
        return KODE_DENIED;
    }
    /* Keep at most ~300 ms queued ahead, as the codec's ring would. */
    int64_t const deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    while ( (s_stream_until - esp_timer_get_time()) > 300000 )
    {
        if ( esp_timer_get_time() >= deadline )
        {
            if ( NULL != taken ) { *taken = 0U; }
            return KODE_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    static int16_t mono[KDRV_ES8311_STREAM_FRAMES];
    size_t done = 0U;
    while ( done < n )
    {
        size_t const chunk = ((n - done) < KDRV_ES8311_STREAM_FRAMES) ? (n - done) : KDRV_ES8311_STREAM_FRAMES;
        for ( size_t i = 0U; i < chunk; ++i )
        {
            if ( 2U == channels )
            {
                mono[i] = (int16_t)(((int32_t)frames[(done + i) * 2U] + frames[(done + i) * 2U + 1U]) / 2);
            }
            else
            {
                mono[i] = frames[done + i];
            }
        }
        kdotsim_pcm(mono, chunk, KDRV_ES8311_RATE_HZ);
        done += chunk;
    }
    int64_t const now = esp_timer_get_time();
    if ( s_stream_until < now )
    {
        s_stream_until = now;
    }
    s_stream_until += (int64_t)n * 1000000 / KDRV_ES8311_RATE_HZ;
    if ( NULL != taken )
    {
        *taken = n;
    }
    return KODE_OK;
}

void kdrv_es8311_stream_end(void) { atomic_store(&s_stream_end, true); }
void kdrv_es8311_record_reset(void) { atomic_store(&s_record_end, false); }
void kdrv_es8311_record_end(void) { atomic_store(&s_record_end, true); }

kode_status_t kdrv_es8311_reg_read(uint8_t reg, uint8_t *val)
{
    if ( NULL == val )
    {
        return KODE_DENIED;
    }
    *val = s_reg[reg];
    return KODE_OK;
}

kode_status_t kdrv_es8311_reg_write(uint8_t reg, uint8_t val)
{
    s_reg[reg] = val;
    return KODE_OK;
}

void kdrv_es8311_apply_settings(void) {}

void kdrv_es8311_hush(void)
{
    atomic_store(&s_hush, true);
    atomic_store(&s_stream_end, true);
}
