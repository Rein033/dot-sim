/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_sdmmc_sim.c
 * @brief The microSD in the sim: a 1 GB card whose volume is the folder <data>/sd.
 *
 * The page's "SD inserted" box (the bridge's `s <0|1>` line) puts the card in
 * or takes it out. With the card out a mount finds nothing (KODE_TIMEOUT, as an
 * empty slot answers) and a mounted volume is gone at once. The files are the
 * folder's own, so the server lists, uploads and downloads them without the
 * sim; sizes and free space come from the folder. The sectors under the volume
 * are not modelled: reads give zeros, writes are refused.
 */
#include "khal_sdmmc.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "kdotsim.h"

#define CARD_BYTES   (1024ULL * 1024ULL * 1024ULL)   /* the card's size */
#define SECTOR_SIZE  512U
#define WALK_DEPTH   4U

static atomic_bool s_in;
static atomic_bool s_mounted;
static bool s_hosted;
static char s_point[560];

void kdotsim_sd_set(bool in)
{
    atomic_store(&s_in, in);
    if ( !in )
    {
        atomic_store(&s_mounted, false);
    }
}

bool kdotsim_sd_in(void)
{
    return atomic_load(&s_in);
}

const char *khal_sdmmc_mount_point(void)
{
    if ( '\0' == s_point[0] )
    {
        (void)snprintf(s_point, sizeof(s_point), "%s/sd", kdotsim_data_dir());
    }
    return s_point;
}

bool khal_sdmmc_ready(void)
{
    return true;
}

bool khal_sdmmc_mounted(void)
{
    return atomic_load(&s_mounted) && atomic_load(&s_in);
}

kode_status_t khal_sdmmc_mount(uint8_t width, uint32_t max_khz, khal_sdmmc_card_t *out)
{
    if ( ((1U != width) && (4U != width)) || (0U == max_khz) || khal_sdmmc_mounted() )
    {
        return KODE_DENIED;
    }
    if ( !atomic_load(&s_in) )
    {
        return KODE_TIMEOUT;
    }
    (void)mkdir(khal_sdmmc_mount_point(), 0755);
    if ( NULL != out )
    {
        (void)memset(out, 0, sizeof(*out));
        out->mfg_id = 0x03U;
        out->oem_id = 0x5344U;
        (void)snprintf(out->name, sizeof(out->name), "SIMSD");
        out->revision = 0x10U;
        out->serial = 0x4B4F4445U;
        out->date = (uint16_t)(((2026U - 2000U) << 4) | 11U);
        out->csd_ver = 1U;
        out->sectors = (uint32_t)(CARD_BYTES / SECTOR_SIZE);
        out->sector_size = SECTOR_SIZE;
        out->read_block_len = SECTOR_SIZE;
        out->ccc = 0x5B5U;
        out->tr_speed_khz = 25000U;
        out->sd_spec = 3U;
        out->bus_widths = 0x1U;   /* one line: the write check on four lines has no card to ask */
        out->au_kb = 64U;
        out->high_capacity = true;
        out->bus_width = 1U;
        out->freq_khz = (max_khz < 20000U) ? max_khz : 20000U;
        out->max_freq_khz = 25000U;
        out->rca = 1U;
    }
    atomic_store(&s_mounted, true);
    return KODE_OK;
}

void khal_sdmmc_unmount(void)
{
    atomic_store(&s_mounted, false);
}

void khal_sdmmc_set_hosted(bool hosted)
{
    s_hosted = hosted;
}

bool khal_sdmmc_hosted(void)
{
    return s_hosted;
}

/** @brief Bytes of the regular files under @p path. */
static uint64_t walk(const char *path, unsigned depth)
{
    uint64_t sum = 0U;
    DIR *const d = opendir(path);
    if ( NULL == d ) { return 0U; }
    struct dirent *e;
    while ( NULL != (e = readdir(d)) )
    {
        char sub[1200];
        struct stat st;
        if ( (0 == strcmp(e->d_name, ".")) || (0 == strcmp(e->d_name, "..")) ) { continue; }
        if ( snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= (int)sizeof(sub) ) { continue; }
        if ( 0 != lstat(sub, &st) ) { continue; }
        if ( S_ISREG(st.st_mode) ) { sum += (uint64_t)st.st_size; }
        else if ( S_ISDIR(st.st_mode) && (depth < WALK_DEPTH) ) { sum += walk(sub, depth + 1U); }
    }
    closedir(d);
    return sum;
}

kode_status_t khal_sdmmc_info(uint64_t *total, uint64_t *free_bytes)
{
    if ( !khal_sdmmc_mounted() ) { return KODE_DENIED; }
    uint64_t used = walk(khal_sdmmc_mount_point(), 0U);
    if ( used > CARD_BYTES ) { used = CARD_BYTES; }
    if ( NULL != total ) { *total = CARD_BYTES; }
    if ( NULL != free_bytes ) { *free_bytes = CARD_BYTES - used; }
    return KODE_OK;
}

/** @brief Remove everything under @p path (the folder itself stays when @p keep). */
static void wipe(const char *path, bool keep, unsigned depth)
{
    DIR *const d = opendir(path);
    if ( NULL != d )
    {
        struct dirent *e;
        while ( NULL != (e = readdir(d)) )
        {
            char sub[1200];
            struct stat st;
            if ( (0 == strcmp(e->d_name, ".")) || (0 == strcmp(e->d_name, "..")) ) { continue; }
            if ( snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= (int)sizeof(sub) ) { continue; }
            if ( 0 != lstat(sub, &st) ) { continue; }
            if ( S_ISDIR(st.st_mode) && (depth < WALK_DEPTH) ) { wipe(sub, false, depth + 1U); }
            else { (void)remove(sub); }
        }
        closedir(d);
    }
    if ( !keep ) { (void)rmdir(path); }
}

kode_status_t khal_sdmmc_format(void)
{
    if ( !khal_sdmmc_mounted() ) { return KODE_DENIED; }
    wipe(khal_sdmmc_mount_point(), true, 0U);
    return KODE_OK;
}

kode_status_t khal_sdmmc_read_sectors(uint32_t first, uint32_t count, void *dst)
{
    if ( !khal_sdmmc_mounted() || (NULL == dst) || (0U == count) ) { return KODE_DENIED; }
    if ( ((uint64_t)first + count) > (CARD_BYTES / SECTOR_SIZE) ) { return KODE_ERROR; }
    (void)memset(dst, 0, (size_t)count * SECTOR_SIZE);
    return KODE_OK;
}

kode_status_t khal_sdmmc_write_sectors(uint32_t first, uint32_t count, const void *src)
{
    (void)first;
    if ( !khal_sdmmc_mounted() || (NULL == src) || (0U == count) ) { return KODE_DENIED; }
    return KODE_ERROR;   /* the sectors under the volume are not modelled */
}
