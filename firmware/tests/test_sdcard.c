/* The microSD card, driven the way ESP-IDF's sdmmc_card_init() and its sector I/O drive it. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "kemu_sdcard.h"

#define SECTORS 65536U   /* 32 MB */

static uint8_t *disk;

static bool ram_read(uint32_t lba, uint32_t count, uint8_t *dst)
{
    if ((uint64_t)lba + count > SECTORS) {
        return false;
    }
    memcpy(dst, &disk[(size_t)lba * KEMU_SD_BLOCK], (size_t)count * KEMU_SD_BLOCK);
    return true;
}

static bool ram_write(uint32_t lba, uint32_t count, const uint8_t *src)
{
    if ((uint64_t)lba + count > SECTORS) {
        return false;
    }
    memcpy(&disk[(size_t)lba * KEMU_SD_BLOCK], src, (size_t)count * KEMU_SD_BLOCK);
    return true;
}

static const kemu_sd_store_t RAM = {ram_read, ram_write};

/* ESP-IDF's MMC_RSP_BITS(): bit n of a response lives at bit n of response[0..3]. */
static uint32_t rsp_bits(const uint32_t *src, int start, int len)
{
    uint32_t const mask = (len % 32 == 0) ? 0xFFFFFFFFU : 0xFFFFFFFFU >> (32 - (len % 32));
    size_t const word = (size_t)start / 32U;
    size_t const shift = (size_t)start % 32U;
    uint32_t const right = src[word] >> shift;
    uint32_t const left = (len + shift <= 32U) ? 0U : src[word + 1U] << ((32U - shift) % 32U);
    return (left | right) & mask;
}

static kemu_sd_cmd_t cmd(uint32_t op, uint32_t arg, void *data, size_t len)
{
    kemu_sd_cmd_t c = {.opcode = op, .arg = arg, .data = data, .datalen = len};
    kemu_sd_cmd(&c);
    return c;
}

static kemu_sd_cmd_t acmd(uint32_t op, uint32_t arg, void *data, size_t len)
{
    kemu_sd_cmd_t const a = cmd(55, 0, NULL, 0);
    CHECK(a.result == KEMU_SD_OK && (a.response[0] & (1U << 5)) != 0);   /* MMC_R1_APP_CMD */
    return cmd(op, arg, data, len);
}

static void fresh(void)
{
    memset(disk, 0, (size_t)SECTORS * KEMU_SD_BLOCK);
    kemu_sd_insert(SECTORS, &RAM);
    kemu_sd_reset();
}

/* sdmmc_card_init(), step by step, on an SD memory card. */
static void identifies_as_an_sdhc_card(void)
{
    fresh();
    CHECK(cmd(52, 0x88000C08U, NULL, 0).result == KEMU_SD_TIMEOUT);   /* sdmmc_io_reset: not SDIO */
    CHECK(cmd(0, 0, NULL, 0).result == KEMU_SD_OK);
    kemu_sd_cmd_t c = cmd(8, 0x1AA, NULL, 0);
    CHECK(c.result == KEMU_SD_OK && c.response[0] == 0x1AA);
    CHECK(cmd(5, 0, NULL, 0).result == KEMU_SD_TIMEOUT);              /* IO_SEND_OP_COND */
    c = acmd(41, 0x40FF8000U, NULL, 0);
    CHECK(c.result == KEMU_SD_OK);
    CHECK((c.response[0] & 0x80000000U) != 0);                         /* ready */
    CHECK((c.response[0] & 0x40000000U) != 0);                         /* SDHC: block addressing */
    CHECK((c.response[0] & (1U << 24)) == 0);                          /* no UHS-I 1.8 V */
    c = cmd(2, 0, NULL, 0);                                            /* CID */
    CHECK(c.result == KEMU_SD_OK && rsp_bits(c.response, 120, 8) != 0);
    CHECK(rsp_bits(c.response, 0, 1) == 1);
    c = cmd(3, 0, NULL, 0);                                            /* RCA */
    uint32_t const rca = c.response[0] >> 16;
    CHECK(c.result == KEMU_SD_OK && rca != 0);
    c = cmd(9, rca << 16, NULL, 0);                                    /* CSD */
    CHECK(c.result == KEMU_SD_OK);
    CHECK(rsp_bits(c.response, 126, 2) == 1);                          /* CSD 2.0 */
    CHECK(((rsp_bits(c.response, 48, 22) + 1U) << 10) == SECTORS);     /* SD_CSD_V2_CAPACITY */
    CHECK(rsp_bits(c.response, 80, 4) == 9 && rsp_bits(c.response, 22, 4) == 9);
    CHECK(cmd(7, rca << 16, NULL, 0).result == KEMU_SD_OK);
    CHECK(cmd(16, 512, NULL, 0).result == KEMU_SD_OK);
}

static void reports_its_scr_and_status(void)
{
    fresh();
    uint32_t const rca = cmd(3, 0, NULL, 0).response[0] >> 16;
    CHECK(((cmd(13, rca << 16, NULL, 0).response[0] >> 9) & 0xF) == 3);   /* stby until selected */
    CHECK(cmd(7, rca << 16, NULL, 0).result == KEMU_SD_OK);
    uint8_t scr[8];
    CHECK(acmd(51, 0, scr, sizeof scr).result == KEMU_SD_OK);
    CHECK((scr[0] & 0x0F) == 2);                                       /* SD_SPEC 2 */
    CHECK((scr[1] & 0x0F) == 0x05);                                    /* 1- and 4-bit */
    CHECK((scr[2] & 0x80) != 0);                                       /* SD_SPEC3 */
    CHECK((scr[3] & 0x03) == 0);                                       /* no CMD23/CMD20 */
    uint8_t ssr[64];
    CHECK(acmd(13, 0, ssr, sizeof ssr).result == KEMU_SD_OK);
    CHECK(acmd(6, 2, NULL, 0).result == KEMU_SD_OK);                   /* 4-bit */
    kemu_sd_cmd_t const s = cmd(13, 0, NULL, 0);
    CHECK(((s.response[0] >> 9) & 0xF) == 4 && (s.response[0] & (1U << 8)) != 0);   /* tran, ready */
}

/* CMD6: high speed supported, and selected when asked for. */
static void switches_to_high_speed(void)
{
    fresh();
    uint8_t st[64];
    CHECK(cmd(6, 0x00FFFFF1U, st, sizeof st).result == KEMU_SD_OK);   /* check mode, group 1 fn 1 */
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {                                     /* sdmmc_flip_byte_order() */
        uint8_t const *b = &st[(15 - i) * 4];
        w[i] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    }
    CHECK((rsp_bits(w, 400, 16) & (1U << 1)) != 0);                   /* SD_SFUNC_SUPPORTED(g1) has HS */
    CHECK(rsp_bits(w, 376, 4) == 1);                                   /* SD_SFUNC_SELECTED(g1) */
}

static void reads_and_writes_blocks(void)
{
    fresh();
    uint8_t out[3 * KEMU_SD_BLOCK], in[3 * KEMU_SD_BLOCK];
    for (size_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)(i * 7U + 3U);
    }
    CHECK(cmd(24, 100, out, KEMU_SD_BLOCK).result == KEMU_SD_OK);
    CHECK(memcmp(&disk[100U * KEMU_SD_BLOCK], out, KEMU_SD_BLOCK) == 0);
    CHECK(cmd(25, 200, out, sizeof out).result == KEMU_SD_OK);
    CHECK(cmd(12, 0, NULL, 0).result == KEMU_SD_OK);
    CHECK(cmd(18, 200, in, sizeof in).result == KEMU_SD_OK);
    CHECK(cmd(12, 0, NULL, 0).result == KEMU_SD_OK);
    CHECK(memcmp(in, out, sizeof in) == 0);
    memset(in, 0, sizeof in);
    CHECK(cmd(17, 100, in, KEMU_SD_BLOCK).result == KEMU_SD_OK);
    CHECK(memcmp(in, out, KEMU_SD_BLOCK) == 0);
}

static void refuses_past_its_end(void)
{
    fresh();
    uint8_t b[2 * KEMU_SD_BLOCK];
    CHECK(cmd(17, SECTORS, b, KEMU_SD_BLOCK).result == KEMU_SD_FAIL);
    CHECK(cmd(18, SECTORS - 1U, b, sizeof b).result == KEMU_SD_FAIL);
}

/* An empty slot answers nothing at all, as a card that is not there. */
static void an_empty_slot_never_answers(void)
{
    fresh();
    kemu_sd_insert(0, NULL);
    CHECK(!kemu_sd_present());
    CHECK(cmd(0, 0, NULL, 0).result == KEMU_SD_TIMEOUT);
    CHECK(cmd(8, 0x1AA, NULL, 0).result == KEMU_SD_TIMEOUT);
    uint8_t b[KEMU_SD_BLOCK];
    CHECK(cmd(17, 0, b, sizeof b).result == KEMU_SD_TIMEOUT);
}

int main(void)
{
    disk = calloc(SECTORS, KEMU_SD_BLOCK);
    RUN(identifies_as_an_sdhc_card);
    RUN(reports_its_scr_and_status);
    RUN(switches_to_high_speed);
    RUN(reads_and_writes_blocks);
    RUN(refuses_past_its_end);
    RUN(an_empty_slot_never_answers);
    free(disk);
    CHECK_DONE();
}
