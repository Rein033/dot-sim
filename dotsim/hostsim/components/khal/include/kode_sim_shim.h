/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sim_shim.h
 * @brief Force-included into every source of the sim: what the linux FreeRTOS
 *        port lacks that the SDK reads.
 */
#ifndef KODE_SIM_SHIM_H_
#define KODE_SIM_SHIM_H_

#define KODE_SIM 1

#ifndef __ASSEMBLER__

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifndef xPortCanYield
int xPortCanYield(void);
#endif

#endif /* __ASSEMBLER__ */

#endif /* KODE_SIM_SHIM_H_ */
