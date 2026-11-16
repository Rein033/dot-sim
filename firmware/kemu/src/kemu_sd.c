/**
 * @file kemu_sd.c
 * @brief The microSD slot: ESP-IDF's SDMMC host, answered by kemu's card.
 *
 * esp-emu models no SDMMC controller (espressif/esp-emulator#33), so a card
 * never answers and kdrv_sd finds the slot empty. kemu stands in for the
 * host - the functions SDMMC_HOST_DEFAULT() points at, and the ones
 * esp_vfs_fat_sdmmc_mount() and khal_sdmmc call by name - and hands each
 * command to the card model in models/sdcard.c. Everything above stays the
 * board's own: khal_sdmmc's mount and its 4-bit-then-1-bit retry, ESP-IDF's
 * sdmmc_card_init(), FATFS and the VFS, kdrv_sd, AO_Storage.
 *
 * The card's blocks are on the Mac, in a card image: a read asks for them
 * (SD_READ) and waits for SD_DATA; a write sends them (SD_WRITE), and the
 * one UART1 stream keeps them in order. Each read has a tag of its own,
 * which its SD_DATA echoes: an answer to a read that timed out is never
 * taken into the next one, even when its blocks would fit there. The Mac says what is in the slot
 * after every HELLO and whenever the page takes the card out or puts it
 * back (SD_CARD). sdmmc_host_deinit() is not here: khal_sdmmc wraps it
 * itself, for esp-hosted's teardown.
 */
#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "kemu_link.h"
#include "kemu_proto.h"
#include "kemu_sd.h"
#include "kemu_sdcard.h"

#define READ_TIMEOUT_MS 3000U
#define WRITE_CHUNK     16U          /* blocks per SD_WRITE frame */

/* ---- The blocks, on the Mac ---------------------------------------------- */

static SemaphoreHandle_t s_io;       /* one read in flight */
static SemaphoreHandle_t s_done;
static uint8_t *volatile s_dst;      /* the read in flight; NULL when none */
static uint32_t s_lba, s_count, s_got;
static volatile uint16_t s_tag;      /* the read in flight's */
static volatile bool s_fail;

static void put_head(uint8_t h[6], uint32_t lba, uint32_t count)
{
    memcpy(h, &lba, 4);
    uint16_t const n = (uint16_t)count;
    memcpy(&h[4], &n, 2);
}

static void on_sd_data(const uint8_t *p, uint32_t len)
{
    uint32_t lba;
    uint16_t n, tag;
    uint8_t *const dst = s_dst;
    if ((len < KEMU_SD_HEAD) || (dst == NULL)) {
        return;                      /* no read in flight */
    }
    memcpy(&lba, p, 4);
    memcpy(&n, &p[4], 2);
    memcpy(&tag, &p[6], 2);
    if (tag != s_tag) {
        return;                      /* the answer to a read that gave up */
    }
    if ((n == 0U) || (lba < s_lba) || ((uint64_t)lba + n > (uint64_t)s_lba + s_count)
        || (len < KEMU_SD_HEAD + (uint32_t)n * KEMU_SD_BLOCK)) {
        s_fail = true;               /* no card, or past its end */
        xSemaphoreGive(s_done);
        return;
    }
    if (lba != s_lba + s_got) {
        return;                      /* not the next blocks: never counted twice */
    }
    memcpy(&dst[(size_t)(lba - s_lba) * KEMU_SD_BLOCK], &p[KEMU_SD_HEAD], (size_t)n * KEMU_SD_BLOCK);
    s_got += n;
    if (s_got >= s_count) {
        xSemaphoreGive(s_done);
    }
}

static bool mac_read(uint32_t lba, uint32_t count, uint8_t *dst)
{
    uint8_t h[KEMU_SD_HEAD];
    xSemaphoreTake(s_io, portMAX_DELAY);
    (void)xSemaphoreTake(s_done, 0);
    s_lba = lba;
    s_count = count;
    s_got = 0;
    s_fail = false;
    uint16_t const tag = (uint16_t)(s_tag + 1U);
    s_tag = tag;
    s_dst = dst;
    put_head(h, lba, count);
    memcpy(&h[6], &tag, 2);
    kemu_send(KEMU_T_SD_READ, h, sizeof h);
    bool const ok = (xSemaphoreTake(s_done, pdMS_TO_TICKS(READ_TIMEOUT_MS)) == pdTRUE) && !s_fail;
    s_dst = NULL;
    xSemaphoreGive(s_io);
    return ok;
}

static bool mac_write(uint32_t lba, uint32_t count, const uint8_t *src)
{
    uint8_t h[6];
    for (uint32_t at = 0; at < count; at += WRITE_CHUNK) {
        uint32_t const n = ((count - at) < WRITE_CHUNK) ? (count - at) : WRITE_CHUNK;
        put_head(h, lba + at, n);
        kemu_send2(KEMU_T_SD_WRITE, h, sizeof h, &src[(size_t)at * KEMU_SD_BLOCK],
                   n * KEMU_SD_BLOCK);
    }
    return true;
}

static const kemu_sd_store_t MAC = {mac_read, mac_write};

/* The slot changes between commands, never under one: the rx task only
 * leaves the news here, and the next transaction takes it. */
static volatile uint32_t s_slot_sectors;
static volatile bool s_slot_news;

static void on_sd_card(const uint8_t *p, uint32_t len)
{
    uint32_t sectors = 0;
    if (len >= 4U) {
        memcpy(&sectors, p, 4);
    }
    s_slot_sectors = sectors;
    s_slot_news = true;
}

void kemu_sd_start(void)
{
    s_io = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    configASSERT((s_io != NULL) && (s_done != NULL));
    kemu_on(KEMU_T_SD_CARD, on_sd_card);
    kemu_on(KEMU_T_SD_DATA, on_sd_data);
}

/* ---- The host ------------------------------------------------------------ */

static size_t s_width = 1;
static uint32_t s_khz = 400;

esp_err_t __wrap_sdmmc_host_init(void) { return ESP_OK; }

/* The width the slot is wired for, which sdmmc_card_init() asks before it
 * takes the card to 4-bit; khal_sdmmc says 4, then 1 on its retry. */
esp_err_t __wrap_sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *cfg)
{
    (void)slot;
    s_width = ((cfg != NULL) && (cfg->width != 0U)) ? cfg->width : 4U;
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_deinit_slot(int slot)
{
    (void)slot;
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_set_bus_width(int slot, size_t width)
{
    (void)slot;
    s_width = width;
    return ESP_OK;
}

size_t __wrap_sdmmc_host_get_slot_width(int slot)
{
    (void)slot;
    return s_width;
}

esp_err_t __wrap_sdmmc_host_set_card_clk(int slot, uint32_t freq_khz)
{
    (void)slot;
    s_khz = freq_khz;
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_get_real_freq(int slot, int *real_freq_khz)
{
    (void)slot;
    if (real_freq_khz != NULL) {
        *real_freq_khz = (int)s_khz;
    }
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_set_bus_ddr_mode(int slot, bool ddr)
{
    (void)slot; (void)ddr;
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_set_cclk_always_on(int slot, bool on)
{
    (void)slot; (void)on;
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_set_input_delay(int slot, sdmmc_delay_phase_t phase)
{
    (void)slot; (void)phase;
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_set_input_delayline(int slot, sdmmc_delay_line_t line)
{
    (void)slot; (void)line;
    return ESP_OK;
}

bool __wrap_sdmmc_host_check_buffer_alignment(int slot, const void *buf, size_t size)
{
    (void)slot; (void)buf; (void)size;
    return true;                     /* no DMA: any buffer will do */
}

esp_err_t __wrap_sdmmc_host_is_slot_set_to_uhs1(int slot, bool *is_uhs1)
{
    (void)slot;
    if (is_uhs1 != NULL) {
        *is_uhs1 = false;
    }
    return ESP_OK;
}

esp_err_t __wrap_sdmmc_host_io_int_enable(int slot)
{
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;    /* a memory card: no SDIO interrupt */
}

esp_err_t __wrap_sdmmc_host_io_int_wait(int slot, uint32_t timeout_ticks)
{
    (void)slot; (void)timeout_ticks;
    return ESP_ERR_TIMEOUT;
}

esp_err_t __wrap_sdmmc_host_do_transaction(int slot, sdmmc_command_t *c)
{
    if (s_slot_news) {
        s_slot_news = false;
        kemu_sd_insert(s_slot_sectors, &MAC);
    }
    if (slot != SDMMC_HOST_SLOT_0) {
        c->error = ESP_ERR_TIMEOUT;  /* the card is in slot 0; slot 1 is the C5's */
        return ESP_OK;
    }
    kemu_sd_cmd_t m = {.opcode = c->opcode, .arg = c->arg, .data = c->data, .datalen = c->datalen};
    kemu_sd_cmd(&m);
    memcpy(c->response, m.response, sizeof c->response);
    c->error = (m.result == KEMU_SD_OK) ? ESP_OK
             : (m.result == KEMU_SD_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}
