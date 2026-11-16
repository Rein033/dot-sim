/**
 * @file kemu_sdcard.h
 * @brief A microSD card, as the SD protocol has one answer the host.
 *
 * An SDHC card (block addressing, CSD 2.0, SD 3.0 SCR, 4-bit, high speed)
 * that answers the commands ESP-IDF's sdmmc_card_init() and its sector
 * reads and writes send, with the responses laid out as ESP-IDF's host
 * leaves them in sdmmc_command_t: R1/R3/R6/R7 in response[0], R2 (CID, CSD)
 * as the 128-bit register with bit n at bit n of response[0..3], and data
 * blocks (SCR, SD status, switch status) big-endian as on the wire.
 *
 * Pure C: the blocks live wherever @ref kemu_sd_store_t says, so the host
 * tests keep them in RAM and the firmware asks the Mac for them.
 */
#ifndef KEMU_SDCARD_H_
#define KEMU_SDCARD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEMU_SD_BLOCK 512U

/** How a command ended, as a host sees it. */
typedef enum {
    KEMU_SD_OK = 0,
    KEMU_SD_TIMEOUT,     /**< no response: no card, an SDIO-only command, an illegal one */
    KEMU_SD_FAIL,        /**< the card answered with an error: out of range, a storage error */
} kemu_sd_result_t;

typedef struct {
    uint32_t opcode;
    uint32_t arg;
    uint32_t response[4];
    void *data;          /**< the data phase's buffer, NULL when there is none */
    size_t datalen;      /**< bytes in the data phase */
    kemu_sd_result_t result;
} kemu_sd_cmd_t;

/** Where the blocks are. Each returns false when they could not be moved. */
typedef struct {
    bool (*read)(uint32_t lba, uint32_t count, uint8_t *dst);
    bool (*write)(uint32_t lba, uint32_t count, const uint8_t *src);
} kemu_sd_store_t;

/** A card of @p sectors 512-byte blocks in the slot; 0 empties the slot. */
void kemu_sd_insert(uint32_t sectors, const kemu_sd_store_t *store);

/** Whether a card is in the slot. */
bool kemu_sd_present(void);

/** Back to power-on: idle, no RCA, no pending APP_CMD. */
void kemu_sd_reset(void);

/** One command, and its data phase if it has one. */
void kemu_sd_cmd(kemu_sd_cmd_t *c);

#endif
