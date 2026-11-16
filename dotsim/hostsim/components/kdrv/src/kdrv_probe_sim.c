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
 * waits (the model's QMC status always says data ready), except `? sd`, which
 * mounts the card and so runs on a task of its own. A driver that its AO
 * has not brought up yet refuses: no answer is made and the asker retries.
 */
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kdotsim.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_sd.h"
#include "kdrv_qmi8658.h"

/** @brief The card as the SDK's driver sees it: mounted through kdrv_sd (a pulled card is dropped),
 *         then the files in the volume counted. Answers "sd <mounted> <files>" and ends. */
static void sd_task(void *arg)
{
    if ( kdotsim_sd_in() ) { (void)kdrv_sd_mount(); } else { kdrv_sd_unmount(); }
    int32_t mounted = 0;
    if ( kdrv_sd_get(KDRV_SD_MOUNTED, &mounted) == KODE_OK )
    {
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
        char out[48];
        int const n = snprintf(out, sizeof(out), "sd %d %d\n", (0 != mounted) ? 1 : 0, files);
        kdotsim_log(out, (size_t)n);
    }
    atomic_flag_clear((atomic_flag *)arg);
    vTaskDelete(NULL);
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
        /* Mounting waits (the card's rail), which only a FreeRTOS task may do: the answer
         * comes from a task of its own, as an L record. One at a time; a probe sent while
         * one runs is dropped, and the asker asks again. */
        static atomic_flag s_busy = ATOMIC_FLAG_INIT;
        if ( !atomic_flag_test_and_set(&s_busy) )
        {
            if ( pdPASS != xTaskCreate(sd_task, "sdprobe", 4096, &s_busy, 5, NULL) )
            {
                atomic_flag_clear(&s_busy);
            }
        }
        return 0;
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
