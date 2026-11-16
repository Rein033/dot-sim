/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_probe_sim.c
 * @brief What the SDK's own drivers read back from the sim's chips, for tests.
 *
 * The bridge's `? bat` and `? imu` lines are answered here through the public
 * read functions of kdrv_cw2217, kdrv_bq25896, kdrv_qmi8658 and kdrv_qmc5883p,
 * so a wrong register encoding in khal_i2c_sim.c shows up in the answer.
 * The AOs (kaos.power, kaos.imu) own the chips, but these calls only read
 * registers, which the I2C model serialises under its own lock; no call here
 * waits (the model's QMC status always says data ready). A driver that its AO
 * has not brought up yet refuses: no answer is made and the asker retries.
 */
#include <stdio.h>
#include <string.h>

#include "kdotsim.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_qmi8658.h"

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
