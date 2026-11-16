/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_absent_sim.c
 * @brief The parts the sim has no model for: the 125 kHz reader, the
 *        expansion buses, the app slot, and the internal FAT (a directory).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "kdrv_exp_bus.h"
#include "kdrv_fs.h"
#include "kdrv_rfid_lf.h"
#include "kdrv_slot.h"

#include "kdotsim.h"

/* ---- RFID: no front end, so no tag is ever in the field ---- */
static bool (*s_rfid_worker)(void);
kode_status_t kdrv_rfid_lf_init(void) { return KODE_OK; }
kode_status_t kdrv_rfid_lf_field_on(bool boost) { (void)boost; return KODE_OK; }
kode_status_t kdrv_rfid_lf_field_off(void) { return KODE_OK; }
kode_status_t kdrv_rfid_lf_rest(void) { return KODE_OK; }
kode_status_t kdrv_rfid_lf_read_window(uint8_t id[5], krfid_em4100_diag_t *d)
{
    (void)id;
    if ( NULL != d ) { memset(d, 0, sizeof(*d)); }
    return KODE_TIMEOUT;
}
kode_status_t kdrv_rfid_lf_clone(const uint8_t id[5]) { (void)id; return KODE_TIMEOUT; }
kode_status_t kdrv_rfid_lf_cert_carrier(bool modulated) { (void)modulated; return KODE_DENIED; }
void kdrv_rfid_lf_set_worker_check(bool (*is_worker)(void)) { s_rfid_worker = is_worker; }
kode_status_t kdrv_rfid_lf_run(kdrv_rfid_lf_job_t const *job, uint8_t id[5], uint8_t *rf_div)
{
    (void)id;
    if ( NULL != rf_div ) { *rf_div = 0U; }
    if ( NULL == job ) { return KODE_DENIED; }
    if ( (KDRV_RFID_JOB_CAPTURE == job->type) && (NULL != job->n) ) { *job->n = 0U; }
    return (KDRV_RFID_JOB_EMU_STOP == job->type) ? KODE_OK : KODE_TIMEOUT;
}
kode_status_t kdrv_rfid_lf_field_capture(uint32_t ms, krfid_field_t *out, uint16_t *wave, size_t cap, size_t *n)
{
    (void)ms; (void)wave; (void)cap;
    if ( NULL != out ) { memset(out, 0, sizeof(*out)); }
    if ( NULL != n ) { *n = 0U; }
    return KODE_OK;
}

/* ---- J3's buses: nothing is plugged in ---- */
kode_status_t kdrv_exp_i2c_open(uint8_t ctrl, uint32_t sda_gpio, uint32_t scl_gpio, uint32_t hz)
{ (void)ctrl; (void)sda_gpio; (void)scl_gpio; (void)hz; return KODE_OK; }
kode_status_t kdrv_exp_i2c_xfer(uint8_t addr, const uint8_t *out, size_t out_len, uint8_t *in, size_t in_len, uint32_t ms)
{ (void)addr; (void)out; (void)out_len; (void)in; (void)in_len; (void)ms; return KODE_IO_ERROR; }
kode_status_t kdrv_exp_i2c_probe(uint8_t addr) { (void)addr; return KODE_IO_ERROR; }
kode_status_t kdrv_exp_i2c_close(void) { return KODE_OK; }
kode_status_t kdrv_exp_spi_open(uint8_t host, uint32_t sck, uint32_t mosi, uint32_t miso, uint32_t cs, uint32_t hz, uint8_t mode)
{ (void)host; (void)sck; (void)mosi; (void)miso; (void)cs; (void)hz; (void)mode; return KODE_OK; }
kode_status_t kdrv_exp_spi_xfer(const uint8_t *out, uint8_t *in, size_t len)
{
    (void)out;
    if ( NULL != in ) { memset(in, 0xFF, len); }
    return KODE_OK;
}
kode_status_t kdrv_exp_spi_close(void) { return KODE_OK; }
kode_status_t kdrv_exp_uart_open(uint8_t port, uint32_t tx, uint32_t rx, uint32_t baud)
{ (void)port; (void)tx; (void)rx; (void)baud; return KODE_OK; }
kode_status_t kdrv_exp_uart_write(const uint8_t *d, size_t len, size_t *sent)
{ (void)d; if ( NULL != sent ) { *sent = len; } return KODE_OK; }
kode_status_t kdrv_exp_uart_read(uint8_t *d, size_t cap, uint32_t ms, size_t *got)
{ (void)d; (void)cap; (void)ms; if ( NULL != got ) { *got = 0U; } return KODE_TIMEOUT; }
kode_status_t kdrv_exp_uart_close(void) { return KODE_OK; }

/* ---- the app slot: the sim runs one app, which is always confirmed ---- */
static kdrv_slot_desc_t sim_desc(void)
{
    kdrv_slot_desc_t d;
    memset(&d, 0, sizeof(d));
    (void)snprintf(d.project, sizeof(d.project), "%s", "sim-app");
    (void)snprintf(d.version, sizeof(d.version), "%s", "sim");
    (void)snprintf(d.date, sizeof(d.date), "%s", __DATE__);
    (void)snprintf(d.time, sizeof(d.time), "%s", __TIME__);
    (void)snprintf(d.idf_ver, sizeof(d.idf_ver), "%s", "v6.1");
    return d;
}
bool kdrv_slot_present(void) { return false; }
uint32_t kdrv_slot_size(void) { return 0U; }
uint32_t kdrv_slot_address(void) { return 0U; }
kode_status_t kdrv_slot_desc(kdrv_slot_desc_t *out) { (void)out; return KODE_DENIED; }
bool kdrv_slot_has_image(void) { return false; }
kode_status_t kdrv_slot_begin(void) { return KODE_DENIED; }
kode_status_t kdrv_slot_write(const void *data, size_t len) { (void)data; (void)len; return KODE_DENIED; }
kode_status_t kdrv_slot_end(void) { return KODE_DENIED; }
void kdrv_slot_abort(void) {}
const kfw_sink_t *kdrv_slot_sink(void) { return NULL; }
kode_status_t kdrv_slot_erase(void) { return KODE_DENIED; }
bool kdrv_slot_confirm_self(void) { return true; }
kode_status_t kdrv_slot_reject_self(void) { return KODE_OK; }
bool kdrv_slot_crashes(uint32_t *count) { if ( NULL != count ) { *count = 0U; } return false; }
void kdrv_slot_crashes_set(uint32_t count) { (void)count; }
bool kdrv_otadata_blank(void) { return false; }
kode_status_t kdrv_otadata_clear(void) { return KODE_OK; }
kode_status_t kdrv_otadata_select_slot(void) { return KODE_OK; }
kdrv_slot_state_t kdrv_slot_state(void) { return KDRV_SLOT_STATE_OTHER; }
kode_status_t kdrv_slot_info(kdrv_slot_id_t id, kdrv_slot_info_t *out)
{
    if ( (NULL == out) || (id >= KDRV_SLOT_ID_COUNT) ) { return KODE_DENIED; }
    memset(out, 0, sizeof(*out));
    static const char *const labels[] = { "factory", "ota_0", "ota_1" };
    out->label = labels[id];
    out->running = (KDRV_SLOT_ID_OTA_0 == id);
    out->has_image = out->running;
    if ( out->running ) { out->desc = sim_desc(); }
    return KODE_OK;
}
kode_status_t kdrv_slot_coredump(kdrv_coredump_t *out)
{
    if ( NULL != out ) { memset(out, 0, sizeof(*out)); }
    return KODE_OK;
}
kode_status_t kdrv_slot_coredump_reason(char *buf, size_t len) { if ( (NULL != buf) && len ) { buf[0] = '\0'; } return KODE_DENIED; }
void kdrv_running_info(const char **label, uint32_t *address, uint32_t *size)
{
    if ( NULL != label ) { *label = "ota_0"; }
    if ( NULL != address ) { *address = 0x420000U; }
    if ( NULL != size ) { *size = 0x400000U; }
}
kode_status_t kdrv_running_desc(kdrv_slot_desc_t *out)
{
    if ( NULL == out ) { return KODE_DENIED; }
    *out = sim_desc();
    return KODE_OK;
}
bool kdrv_running_is_factory(void) { return false; }
int kdrv_reset_reason(void) { return KDRV_RESET_POWERON; }

/* ---- the internal FAT: a directory under the sim's data dir ---- */
static char s_fs_mp[512];
static bool s_fs_mounted;
const char *kdrv_fs_mount(void)
{
    if ( !s_fs_mounted )
    {
        (void)snprintf(s_fs_mp, sizeof(s_fs_mp), "%s/storage", kdotsim_data_dir());
        (void)mkdir(s_fs_mp, 0755);
        s_fs_mounted = true;
    }
    return s_fs_mp;
}
void kdrv_fs_unmount(void) { s_fs_mounted = false; }
bool kdrv_fs_mounted(void) { return s_fs_mounted; }
const char *kdrv_fs_mount_point(void) { return s_fs_mounted ? s_fs_mp : NULL; }
kode_status_t kdrv_fs_usage(uint64_t *total, uint64_t *free_bytes)
{
    uint64_t const cap = 2ULL * 1024ULL * 1024ULL;   /* the 2 MB storage partition */
    if ( NULL != total ) { *total = cap; }
    if ( NULL != free_bytes ) { *free_bytes = cap / 2U; }
    return KODE_OK;
}
uint32_t kdrv_fs_cluster_bytes(void) { return 4096U; }
