/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_misc_sim.c
 * @brief The rest of the board's HAL, simulated: clocks, console, identity,
 *        the warm-boot keep, power, the die temperature, and the
 *        ports of khal_exp and khal_pwm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "khal_clk.h"
#include "khal_console.h"
#include "khal_identity.h"
#include "khal_keep.h"
#include "khal_power.h"
#include "khal_sdmmc.h"
#include "khal_temp.h"
#include "khal_system.h"
#include "khal_exp_port.h"
#include "khal_pwm_port.h"

#include "kdotsim.h"

/* ---- clocks ---- */
uint32_t khal_clk_main_hz(void) { return 40000000U; }
khal_clk_slow_t khal_clk_slow_src(void) { return KHAL_CLK_SLOW_XTAL32K; }
kode_status_t khal_clk_measure(khal_clk_src_t src, uint32_t cycles, uint32_t *milli_hz)
{
    (void)src; (void)cycles;
    if ( NULL != milli_hz ) { *milli_hz = 32768000U; }
    return KODE_OK;
}
kode_status_t khal_clk_tree_hz(khal_clk_tree_t clk, uint32_t *hz)
{
    if ( NULL == hz ) { return KODE_DENIED; }
    switch ( clk )
    {
    case KHAL_CLK_TREE_CPU: *hz = 400000000U; break;
    case KHAL_CLK_TREE_APB: *hz = 100000000U; break;
    case KHAL_CLK_TREE_RTC_SLOW: *hz = 32768U; break;
    default: *hz = 20000000U; break;
    }
    return KODE_OK;
}

/* ---- console: out to stdout and the browser's log; no input yet ---- */
void khal_console_init(void) {}
void khal_console_rest(bool rest) { (void)rest; }
kode_status_t khal_console_start_reader(void (*on_byte)(uint8_t byte), uint32_t prio)
{
    (void)on_byte; (void)prio;
    return KODE_OK;
}
kode_status_t khal_console_raw_begin(void (*pump)(void), void (*on_armed)(void))
{
    (void)pump; (void)on_armed;
    return KODE_DENIED;
}
void khal_console_raw_end(void) {}
int32_t khal_console_read_raw(void *buf, size_t want, uint32_t timeout_ms)
{
    (void)buf; (void)want; (void)timeout_ms;
    return 0;
}
void khal_console_write(const char *buf, size_t len)
{
    if ( (NULL == buf) || (0U == len) ) { return; }
    (void)fwrite(buf, 1U, len, stdout);
    (void)fflush(stdout);
    kdotsim_log(buf, len);
}
kode_status_t khal_console_dock_release(void) { return KODE_OK; }
kode_status_t khal_console_dock_take(void) { return KODE_OK; }

/* ---- identity: a fixed simulated MAC ---- */
static const uint8_t SIM_MAC[KHAL_ID_MAC_LEN] = { 0x02, 0x4B, 0x4F, 0x44, 0x45, 0x01 };
static char s_serial[KHAL_ID_LEN + 1U] = "KODESIM000001";
static char s_pretty[KHAL_ID_PRETTY_LEN + 1U] = "KODE-SIM-0001";
kode_status_t khal_identity_init(void) { return KODE_OK; }
kode_status_t khal_identity_set_mac(const uint8_t mac[KHAL_ID_MAC_LEN]) { (void)mac; return KODE_OK; }
const char *khal_identity_serial(void) { return s_serial; }
const char *khal_identity_pretty(void) { return s_pretty; }
bool khal_identity_valid(void) { return true; }
kode_status_t khal_identity_mac(uint8_t mac[KHAL_ID_MAC_LEN])
{
    memcpy(mac, SIM_MAC, KHAL_ID_MAC_LEN);
    return KODE_OK;
}
kode_status_t khal_identity_encode(const uint8_t mac[KHAL_ID_MAC_LEN], char *out, size_t cap)
{
    if ( (NULL == out) || (cap < 13U) ) { return KODE_DENIED; }
    (void)snprintf(out, cap, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return KODE_OK;
}

/* ---- keep: RAM that survives a warm boot; here a process lifetime ---- */
static uint8_t *s_keep;
void khal_keep_init(void)
{
    if ( NULL == s_keep ) { s_keep = calloc(1U, KHAL_KEEP_BYTES); }
}
void *khal_keep(void) { khal_keep_init(); return s_keep; }
void khal_keep_sync(size_t off, size_t len) { (void)off; (void)len; }
void khal_keep_forget_warm(void) {}

/* ---- power: always on USB, nothing sleeps ---- */
kode_status_t khal_power_init(void) { return KODE_OK; }
void khal_power_hold(bool hold) { (void)hold; }
kode_status_t khal_power_lock_new(const char *name, khal_power_lock_t *out)
{
    static uint8_t n;
    (void)name;
    if ( NULL == out ) { return KODE_DENIED; }
    if ( n >= KHAL_POWER_LOCKS ) { return KODE_NO_RESOURCES; }
    *out = n++;
    return KODE_OK;
}
void khal_power_lock(khal_power_lock_t l, bool hold) { (void)l; (void)hold; }
kode_status_t khal_power_wake_arm(uint32_t gpio, bool level_high, khal_power_wake_cb_t cb, void *arg)
{
    (void)gpio; (void)level_high; (void)cb; (void)arg;
    return KODE_OK;
}
void khal_power_wake_rearm(uint32_t gpio) { (void)gpio; }
void khal_power_stats(khal_power_stats_t *out) { if ( NULL != out ) { memset(out, 0, sizeof(*out)); } }
bool khal_power_woke_from_off(void) { return false; }
void khal_power_holds_release(void) {}
void khal_power_off_enter(void) { kdotsim_exit("power off"); }
static uint32_t s_off_note;
void khal_power_off_note_set(uint32_t note) { s_off_note = note; }
uint32_t khal_power_off_note(void) { return s_off_note; }

/* ---- die temperature ---- */
kode_status_t khal_temp_read(int32_t *deci_c)
{
    if ( NULL == deci_c ) { return KODE_DENIED; }
    *deci_c = 385;
    return KODE_OK;
}

/* ---- the ports under khal_exp and khal_pwm: J3 is unconnected ---- */
kode_status_t khal_exp_port_adc_open(uint8_t chan) { (void)chan; return KODE_OK; }
kode_status_t khal_exp_port_adc_read(uint8_t chan, uint16_t *raw, int32_t *mv, bool *calibrated)
{
    (void)chan;
    if ( NULL != raw ) { *raw = 0U; }
    if ( NULL != mv ) { *mv = 0; }
    if ( NULL != calibrated ) { *calibrated = false; }
    return KODE_OK;
}
void khal_exp_port_adc_close(uint8_t chan) { (void)chan; }
kode_status_t khal_pwm_port_timer_set(uint8_t timer, uint32_t hz, uint8_t bits) { (void)timer; (void)hz; (void)bits; return KODE_OK; }
void khal_pwm_port_timer_stop(uint8_t timer) { (void)timer; }
kode_status_t khal_pwm_port_timer_pause(uint8_t timer) { (void)timer; return KODE_OK; }
kode_status_t khal_pwm_port_timer_resume(uint8_t timer) { (void)timer; return KODE_OK; }
kode_status_t khal_pwm_port_chan_start(uint8_t chan, uint8_t gpio, uint8_t timer, uint32_t duty) { (void)chan; (void)gpio; (void)timer; (void)duty; return KODE_OK; }
kode_status_t khal_pwm_port_chan_bind(uint8_t chan, uint8_t timer, uint32_t duty) { (void)chan; (void)timer; (void)duty; return KODE_OK; }
kode_status_t khal_pwm_port_chan_duty(uint8_t chan, uint32_t duty) { (void)chan; (void)duty; return KODE_OK; }
void khal_pwm_port_chan_stop(uint8_t chan, uint8_t gpio) { (void)chan; (void)gpio; }
