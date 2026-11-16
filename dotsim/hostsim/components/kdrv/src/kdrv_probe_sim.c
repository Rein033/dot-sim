/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_probe_sim.c
 * @brief What the SDK's own drivers read back from the sim's chips, for tests.
 *
 * The bridge's `? bat`, `? imu` and `? sd` lines are answered here through the public
 * read functions of kdrv_cw2217, kdrv_bq25896, kdrv_qmi8658 and kdrv_qmc5883p,
 * so a wrong register encoding in khal_i2c_sim.c shows up in the answer.
 * The AOs (kaos.power, kaos.imu) own the chips, but these calls only read
 * registers, which the I2C model serialises under its own lock; no call here
 * waits (the model's QMC status always says data ready); `? sd` mounts the card
 * with the rail already driven, so the driver has nothing to wait for. A driver that its AO
 * has not brought up yet refuses: no answer is made and the asker retries.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "kbsp.h"
#include "kdotsim.h"
#include "khal_gpio.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_sd.h"
#include "kdrv_qmi8658.h"

/** @brief The card as the SDK's driver sees it: mounted through kdrv_sd (a pulled card is dropped),
 *         then the files in the volume counted. Answers "sd <mounted> <files>". */
static int probe_sd(char *out, size_t cap)
{
    /* The driver waits for the card's rail the first time it finds SD_PWR not driven low, and a wait
     * (vTaskDelay) is for FreeRTOS tasks, not for the bridge's thread: drive the pin first. */
    (void)khal_gpio_output((uint8_t)KBSP_PIN_SD_PWR, false);
    if ( kdotsim_sd_in() ) { (void)kdrv_sd_mount(); } else { kdrv_sd_unmount(); }
    int32_t mounted = 0;
    if ( kdrv_sd_get(KDRV_SD_MOUNTED, &mounted) != KODE_OK )
    {
        return 0;
    }
    int files = 0;
    DIR *const d = (0 != mounted) ? opendir(kdrv_sd_mount_point()) : NULL;
    if ( NULL != d )
    {
        struct dirent *e;
        while ( NULL != (e = readdir(d)) )
        {
            char path[1200];
            struct stat st;
            (void)snprintf(path, sizeof(path), "%s/%s", kdrv_sd_mount_point(), e->d_name);
            if ( (0 == stat(path, &st)) && S_ISREG(st.st_mode) ) { ++files; }
        }
        closedir(d);
    }
    return snprintf(out, cap, "sd %d %d\n", (0 != mounted) ? 1 : 0, files);
}

int kdotsim_probe(const char *what, char *out, size_t cap)
{
    if ( (NULL == what) || (NULL == out) )
    {
        return 0;
    }
    if ( 0 == strncmp(what, "bat", 3U) )
    {
        int32_t soc = 0, vbus = 0;
        if ( (kdrv_cw2217_get(KDRV_CW_SOC_X256, &soc) != KODE_OK)
            || (kdrv_bq25896_get(KDRV_BQ_VBUS_STAT, &vbus) != KODE_OK) )
        {
            return 0;
        }
        return snprintf(out, cap, "bat %d %d\n", (int)((soc + 128) / 256), (0 != vbus) ? 1 : 0);
    }
    if ( 0 == strncmp(what, "sd", 2U) )
    {
        return probe_sd(out, cap);
    }
    if ( 0 == strncmp(what, "imu", 3U) )
    {
        kdrv_qmi8658_sample_t s;
        int32_t mg[KDRV_QMC5883P_AXES];
        if ( (kdrv_qmi8658_read(&s) != KODE_OK) || (kdrv_qmc5883p_read(mg) != KODE_OK) )
        {
            return 0;
        }
        return snprintf(out, cap, "imu %d %d %d %d %d %d %d %d %d %d\n",
                        (int)s.a_mg[0], (int)s.a_mg[1], (int)s.a_mg[2],
                        (int)s.w_mdps[0], (int)s.w_mdps[1], (int)s.w_mdps[2],
                        (int)mg[0], (int)mg[1], (int)mg[2], (int)s.temp_dc);
    }
    return 0;
}
