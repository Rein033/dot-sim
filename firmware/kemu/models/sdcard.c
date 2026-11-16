/**
 * @file sdcard.c
 * @brief The microSD card in the slot (see kemu_sdcard.h).
 *
 * The card is the SD Physical Layer spec's, as ESP-IDF's sdmmc driver asks
 * it things: CMD0, CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7 to identify it;
 * ACMD51 (SCR), CMD6 (switch to high speed), ACMD6 (4-bit), ACMD13 (SD
 * status) to set it up; CMD13, CMD16, CMD17/18, CMD24/25, CMD12 and the erase
 * commands to use it. SDIO's CMD5/52/53 and anything else get no response,
 * as a memory-only card gives none.
 */
#include <string.h>

#include "kemu_sdcard.h"

#define RCA            0x5D1AU
#define OCR            0xC0FF8000U   /* ready, SDHC, 2.7-3.6 V */
#define R1_APP_CMD     (1U << 5)
#define R1_READY       (1U << 8)
#define STATE_TRAN     4U
#define STATE_STBY     3U

static uint32_t s_sectors;
static kemu_sd_store_t s_store;
static bool s_app;          /* the last command was CMD55 */
static bool s_selected;
static uint8_t s_access;    /* function group 1: 0 default speed, 1 high speed */

/* ---- 128-bit registers, bit n at bit n of r[0..3] ---------------------- */

static void put_bits(uint32_t *r, unsigned start, unsigned len, uint32_t v)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned const b = start + i;
        if ((v >> i) & 1U) {
            r[b / 32U] |= 1U << (b % 32U);
        }
    }
}

static void cid(uint32_t *r)
{
    static const char PNM[5] = {'K', 'D', 'S', 'I', 'M'};
    memset(r, 0, 4 * sizeof *r);
    put_bits(r, 120, 8, 0x4B);                 /* MID */
    put_bits(r, 104, 16, ('K' << 8) | 'D');    /* OID */
    for (unsigned i = 0; i < 5U; i++) {        /* PNM, first character highest */
        put_bits(r, 96U - 8U * i, 8, (uint8_t)PNM[i]);
    }
    put_bits(r, 56, 8, 0x10);                  /* PRV 1.0 */
    put_bits(r, 24, 32, 0x0C0DE5D1U);          /* PSN */
    put_bits(r, 8, 12, (26U << 4) | 10U);      /* MDT: 2026-10 */
    put_bits(r, 0, 1, 1);
}

static void csd(uint32_t *r)
{
    memset(r, 0, 4 * sizeof *r);
    put_bits(r, 126, 2, 1);                    /* CSD 2.0: SDHC */
    put_bits(r, 112, 8, 0x0E);                 /* TAAC */
    put_bits(r, 96, 8, 0x32);                  /* TRAN_SPEED: 25 MHz */
    put_bits(r, 84, 12, 0x5B5);                /* CCC */
    put_bits(r, 80, 4, 9);                     /* READ_BL_LEN: 512 */
    put_bits(r, 48, 22, (s_sectors >> 10) - 1U);   /* C_SIZE, in 512 KB */
    put_bits(r, 46, 1, 1);                     /* ERASE_BLK_EN */
    put_bits(r, 39, 7, 0x7F);                  /* SECTOR_SIZE */
    put_bits(r, 26, 3, 2);                     /* R2W_FACTOR */
    put_bits(r, 22, 4, 9);                     /* WRITE_BL_LEN: 512 */
    put_bits(r, 0, 1, 1);
}

/* ---- Data blocks, big-endian as on the wire ------------------------------ */

/* Bit n of a 512-bit status (bit 511 first on the wire). */
static void put_status_bits(uint8_t *s, unsigned start, unsigned len, uint32_t v)
{
    for (unsigned i = 0; i < len; i++) {
        unsigned const b = start + i;
        if ((v >> i) & 1U) {
            s[(511U - b) / 8U] |= (uint8_t)(1U << (b % 8U));
        }
    }
}

static void switch_status(uint8_t *s, uint32_t arg)
{
    uint8_t const want = (uint8_t)(arg & 0xFU);
    uint8_t got = s_access;
    if (want != 0xFU) {
        got = (want <= 1U) ? want : 0xFU;      /* default or high speed; anything else is refused */
        if ((arg & 0x80000000U) && (got != 0xFU)) {
            s_access = got;                    /* switch mode */
        }
    }
    memset(s, 0, 64);
    put_status_bits(s, 496, 16, 100);          /* max current, mA */
    put_status_bits(s, 400, 16, 0x8003);       /* group 1: default and high speed */
    for (unsigned g = 2; g <= 6U; g++) {
        put_status_bits(s, 400U + (g - 1U) * 16U, 16, 0x8001);
    }
    put_status_bits(s, 376, 4, got);           /* group 1's function */
    put_status_bits(s, 368, 8, 1);             /* status version */
}

static uint32_t r1(void)
{
    uint32_t const state = s_selected ? STATE_TRAN : STATE_STBY;
    return (state << 9) | R1_READY | (s_app ? R1_APP_CMD : 0U);
}

/* ---- Blocks ---------------------------------------------------------------- */

static kemu_sd_result_t blocks(kemu_sd_cmd_t *c, bool write)
{
    uint32_t const count = (uint32_t)(c->datalen / KEMU_SD_BLOCK);
    if ((c->data == NULL) || (count == 0U) || ((uint64_t)c->arg + count > s_sectors)) {
        return KEMU_SD_FAIL;
    }
    bool const ok = write ? s_store.write(c->arg, count, c->data)
                          : s_store.read(c->arg, count, c->data);
    return ok ? KEMU_SD_OK : KEMU_SD_FAIL;
}

/* ---- The card ------------------------------------------------------------- */

void kemu_sd_insert(uint32_t sectors, const kemu_sd_store_t *store)
{
    s_sectors = ((store != NULL) && (sectors >= 1024U)) ? (sectors & ~1023U) : 0U;
    if (s_sectors != 0U) {
        s_store = *store;
    }
    kemu_sd_reset();
}

bool kemu_sd_present(void)
{
    return s_sectors != 0U;
}

void kemu_sd_reset(void)
{
    s_app = false;
    s_selected = false;
    s_access = 0U;
}

void kemu_sd_cmd(kemu_sd_cmd_t *c)
{
    bool const app = s_app;
    memset(c->response, 0, sizeof c->response);
    c->result = KEMU_SD_OK;
    if (!kemu_sd_present()) {
        c->result = KEMU_SD_TIMEOUT;
        return;
    }
    s_app = false;
    if (app) {
        switch (c->opcode) {
            case 41: c->response[0] = OCR; return;                       /* SD_SEND_OP_COND */
            case 6:  c->response[0] = r1(); return;                      /* SET_BUS_WIDTH */
            case 23: c->response[0] = r1(); return;                      /* SET_WR_BLK_ERASE_COUNT */
            case 13:                                                      /* SD_STATUS */
                if (c->data != NULL) {
                    memset(c->data, 0, c->datalen);
                    if (c->datalen >= 1U) {
                        ((uint8_t *)c->data)[0] = 0x80;                 /* DAT_BUS_WIDTH: 4-bit */
                    }
                }
                c->response[0] = r1();
                return;
            case 51:                                                      /* SEND_SCR */
                if ((c->data == NULL) || (c->datalen < 8U)) {
                    c->result = KEMU_SD_FAIL;
                    return;
                }
                memset(c->data, 0, c->datalen);
                ((uint8_t *)c->data)[0] = 0x02;      /* SCR 1.0, SD_SPEC 2 */
                ((uint8_t *)c->data)[1] = 0x35;      /* SDHC security, 1- and 4-bit */
                ((uint8_t *)c->data)[2] = 0x80;      /* SD_SPEC3 */
                c->response[0] = r1();
                return;
            default:
                break;                               /* an ordinary command after all */
        }
    }
    switch (c->opcode) {
        case 0:                                                           /* GO_IDLE_STATE */
            kemu_sd_reset();
            return;
        case 8:  c->response[0] = c->arg & 0xFFFU; return;                /* SEND_IF_COND */
        case 55: s_app = true; c->response[0] = r1(); return;             /* APP_CMD */
        case 2:  cid(c->response); return;                                /* ALL_SEND_CID */
        case 3:  c->response[0] = (RCA << 16) | (2U << 9) | R1_READY; return;   /* SEND_RELATIVE_ADDR */
        case 9:  csd(c->response); return;                                /* SEND_CSD */
        case 7:                                                           /* SELECT_CARD */
            s_selected = ((c->arg >> 16) == RCA);
            c->response[0] = r1();
            return;
        case 6:                                                           /* SWITCH_FUNC */
            if ((c->data == NULL) || (c->datalen < 64U)) {
                c->result = KEMU_SD_FAIL;
                return;
            }
            switch_status(c->data, c->arg);
            c->response[0] = r1();
            return;
        case 12: case 13: case 16: case 32: case 33: case 38:             /* STOP, STATUS, BLOCKLEN, ERASE */
            c->response[0] = r1();
            return;
        case 17: case 18:                                                 /* READ_SINGLE/MULTIPLE_BLOCK */
            c->result = blocks(c, false);
            c->response[0] = r1();
            return;
        case 24: case 25:                                                 /* WRITE_BLOCK/MULTIPLE_BLOCK */
            c->result = blocks(c, true);
            c->response[0] = r1();
            return;
        default:                                                          /* SDIO, or not a command we know */
            c->result = KEMU_SD_TIMEOUT;
            return;
    }
}
