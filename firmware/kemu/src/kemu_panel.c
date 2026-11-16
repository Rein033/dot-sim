/**
 * @file kemu_panel.c
 * @brief The AMOLED, made virtual: what kdrv_display draws reaches the Mac.
 *
 * The emulator has no MIPI-DSI, so the DSI bus, the DBI command channel and
 * the CO5300 panel are stand-ins. The command channel and the panel are
 * real esp_lcd objects with their own function tables, so every generic
 * esp_lcd call a driver makes on them - DCS commands, deletes, the ones no
 * driver of today uses - lands here rather than on memory that is not one.
 * The DPI panel's contract is kept the way kdrv_display relies on it:
 *
 *  - two framebuffers in PSRAM, 64-byte aligned, handed out by
 *    esp_lcd_dpi_panel_get_frame_buffer();
 *  - draw_bitmap() with a pointer inside one of them is a flip: that buffer
 *    is the one shown from now on, and on_color_trans_done is called before
 *    draw_bitmap returns, as IDF does for a flip;
 *  - draw_bitmap() with any other pointer copies the rectangle into the
 *    shown buffer, then calls on_color_trans_done;
 *  - on_frame_buf_complete and then on_vsync fire from interrupt context
 *    once per refresh, at 30 Hz, off a GPTimer: the pair IDF's DMA-done ISR
 *    calls on a chip whose DSI bridge has no VSYNC event, in that order
 *    (esp_lcd_panel_dpi.c). Not an esp_timer with ISR dispatch, which IDF
 *    can lose the task list's alarm to under the emulator's coarse
 *    interrupt timing, after which no task-dispatched esp_timer (kodeOS's
 *    QP tick among them) runs again. kdrv_display stops the video in
 *    on_vsync, in the blanking, to keep a frame on the glass across a
 *    restart or a rest; it does that with one MIPI_DSI_HOST write, which
 *    esp-emu takes as it takes the host registers on_frame_buf_complete
 *    reads every frame and the bring-up writes.
 *
 * DCS on the command channel is the panel's state: DISPOFF/DISPON, SLPIN/
 * SLPOUT and brightness (0x51) reach the Mac as PANEL, and the power-mode
 * read (0x0A) answers as a CO5300 would. Taking the link down (the deletes)
 * and up again (esp_lcd_new_panel_dpi, as kodeOS's wake does) keeps the
 * framebuffers, as the panel keeps its GRAM.
 *
 * Every draw sends the Mac what changed, and only that: kemu keeps a copy
 * of the picture the Mac has, and each band of rows that differs from it
 * goes out as FB_PACKED, full width, packed with kemu_pack16(). Every byte
 * through UART1 is emulated - pushing whole frames held the emulator at
 * two-thirds of real time - and kodeOS's flat colours pack some thirtyfold.
 */
#include <stdarg.h>
#include <string.h>

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"

#include "kbsp_board.h"
#include "kemu_link.h"
#include "kemu_proto.h"

#define W        KBSP_LCD_H_RES
#define H        KBSP_LCD_V_RES
#define FB_BYTES ((size_t)W * H * 2U)
#define REFRESH_US (1000000U / 30U)

#define DCS_SLPIN   0x10
#define DCS_SLPOUT  0x11
#define DCS_DISPOFF 0x28
#define DCS_DISPON  0x29
#define DCS_WRDISBV 0x51
#define DCS_RDDPM   0x0A
#define DPM_AWAKE   0x9C    /* booster on, sleep out, normal mode, display on */
#define DPM_ASLEEP  0x08

/* The bus is only ever handed back to kemu; it needs to be non-NULL. */
static uint8_t s_bus_tag;
static esp_lcd_panel_t s_panel;
#define PANEL (&s_panel)

static uint8_t *s_fb[2];
static uint8_t *s_sent;      /* the picture the Mac has                */
static bool s_sent_valid;    /* false until the first full picture     */
static bool s_copied;        /* a copy went into the shown buffer alone */
static uint8_t *s_pack;      /* one packed band, at most a whole frame */
static uint8_t s_shown;
static bool s_on;            /* DISPON, or disp_on_off(true)  */
static bool s_awake = true;  /* SLPOUT                         */
static uint8_t s_brightness;
static esp_lcd_dpi_panel_color_trans_done_cb_t s_trans_done;
static esp_lcd_dpi_panel_frame_buf_complete_cb_t s_frame_done;
static esp_lcd_dpi_panel_vsync_cb_t s_vsync;
static void *s_user;
static gptimer_handle_t s_refresh;
static bool s_scanning;

static void send_panel(void)
{
    uint8_t const p[2] = {(s_on && s_awake) ? 1U : 0U, s_brightness};
    kemu_send(KEMU_T_PANEL, p, sizeof p);
}

#define ROW_BYTES ((size_t)W * 2U)

static bool row_changed(const uint8_t *fb, int y)
{
    return !s_sent_valid
           || (memcmp(&fb[(size_t)y * ROW_BYTES], &s_sent[(size_t)y * ROW_BYTES], ROW_BYTES) != 0);
}

/* Send the Mac the bands of rows [y0, y1) of @p fb that it does not have. */
static void send_changes(const uint8_t *fb, int y0, int y1)
{
    int y = y0;
    while (y < y1) {
        if (!row_changed(fb, y)) {
            y++;
            continue;
        }
        int const top = y;
        while ((y < y1) && row_changed(fb, y)) {
            y++;
        }
        size_t const off = (size_t)top * ROW_BYTES;
        size_t const len = (size_t)(y - top) * ROW_BYTES;
        memcpy(&s_sent[off], &fb[off], len);
        size_t const packed = kemu_pack16((const uint16_t *)&fb[off], len / 2U, s_pack);
        uint16_t const head[4] = {0, (uint16_t)top, (uint16_t)W, (uint16_t)(y - top)};
        kemu_send2(KEMU_T_FB_PACKED, head, sizeof head, s_pack, (uint32_t)packed);
    }
    if ((y0 == 0) && (y1 == (int)H)) {
        s_sent_valid = true;
    }
}

static bool IRAM_ATTR on_refresh(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata,
                                 void *arg)
{
    (void)timer; (void)edata; (void)arg;
    bool woke = false;
    esp_lcd_dpi_panel_event_data_t ev;
    if (s_frame_done != NULL) {
        woke = s_frame_done(PANEL, &ev, s_user);
    }
    if (s_vsync != NULL) {
        woke = s_vsync(PANEL, &ev, s_user) || woke;
    }
    return woke;
}

static bool alloc_fbs(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_fb[i] == NULL) {
            s_fb[i] = heap_caps_aligned_calloc(64, 1, FB_BYTES, MALLOC_CAP_SPIRAM);
            if (s_fb[i] == NULL) {
                return false;
            }
        }
    }
    if (s_sent == NULL) {
        s_sent = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (s_pack == NULL) {
        s_pack = heap_caps_malloc(KEMU_PACK16_MAX(FB_BYTES / 2U), MALLOC_CAP_SPIRAM);
    }
    return (s_sent != NULL) && (s_pack != NULL);
}

/* ---- The bus, the command channel, the panel ---------------------------- */

esp_err_t __wrap_esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t *cfg,
                                     esp_lcd_dsi_bus_handle_t *ret)
{
    (void)cfg;
    *ret = (esp_lcd_dsi_bus_handle_t)&s_bus_tag;
    return ESP_OK;
}

/* ---- The command channel: DCS --------------------------------------------- */

static esp_err_t io_tx_param(esp_lcd_panel_io_t *io, int cmd, const void *param, size_t size)
{
    const uint8_t *const p = param;
    (void)io;
    switch (cmd) {
        case DCS_DISPOFF: s_on = false; break;
        case DCS_DISPON:  s_on = true; break;
        case DCS_SLPIN:   s_awake = false; break;
        case DCS_SLPOUT:  s_awake = true; break;
        case DCS_WRDISBV:
            if ((p != NULL) && (size >= 1U)) {
                s_brightness = (uint8_t)((p[0] * 100U + 127U) / 255U);
            }
            break;
        default:
            return ESP_OK;          /* the rest of the CO5300's commands change nothing seen */
    }
    send_panel();
    return ESP_OK;
}

static esp_err_t io_rx_param(esp_lcd_panel_io_t *io, int cmd, void *param, size_t size)
{
    (void)io;
    if ((param != NULL) && (size >= 1U)) {
        memset(param, 0, size);
        if (cmd == DCS_RDDPM) {
            *(uint8_t *)param = s_awake ? DPM_AWAKE : DPM_ASLEEP;
        }
    }
    return ESP_OK;
}

static esp_err_t io_tx_color(esp_lcd_panel_io_t *io, int cmd, const void *color, size_t size)
{
    (void)io; (void)cmd; (void)color; (void)size;
    return ESP_OK;
}

static esp_err_t io_del(esp_lcd_panel_io_t *io)
{
    (void)io;
    return ESP_OK;
}

static esp_err_t io_register_event_callbacks(esp_lcd_panel_io_t *io,
                                             const esp_lcd_panel_io_callbacks_t *cbs, void *ctx)
{
    (void)io; (void)cbs; (void)ctx;
    return ESP_OK;
}

static esp_lcd_panel_io_t s_io = {
    .rx_param = io_rx_param, .tx_param = io_tx_param, .tx_color = io_tx_color,
    .del = io_del, .register_event_callbacks = io_register_event_callbacks,
};

esp_err_t __wrap_esp_lcd_new_panel_io_dbi(esp_lcd_dsi_bus_handle_t bus,
                                          const esp_lcd_dbi_io_config_t *cfg,
                                          esp_lcd_panel_io_handle_t *ret)
{
    (void)bus; (void)cfg;
    *ret = &s_io;
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_del_dsi_bus(esp_lcd_dsi_bus_handle_t bus)
{
    (void)bus;
    return ESP_OK;
}

/* ---- The panel ------------------------------------------------------------ */

static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    (void)panel;
    return ESP_OK;
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    (void)panel;
    if (s_refresh == NULL) {
        const gptimer_config_t cfg = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000,
        };
        const gptimer_event_callbacks_t cbs = {.on_alarm = on_refresh};
        const gptimer_alarm_config_t alarm = {
            .alarm_count = REFRESH_US,
            .flags.auto_reload_on_alarm = true,
        };
        ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &s_refresh));
        ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_refresh, &cbs, NULL));
        ESP_ERROR_CHECK(gptimer_set_alarm_action(s_refresh, &alarm));
        ESP_ERROR_CHECK(gptimer_enable(s_refresh));
    }
    if (!s_scanning) {
        ESP_ERROR_CHECK(gptimer_start(s_refresh));
        s_scanning = true;
    }
    s_awake = true;
    return ESP_OK;
}

/* The scan-out stops; the framebuffers stay, as the panel's GRAM does. */
static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    (void)panel;
    if (s_scanning) {
        (void)gptimer_stop(s_refresh);
        s_scanning = false;
    }
    s_trans_done = NULL;
    s_frame_done = NULL;
    s_vsync = NULL;
    return ESP_OK;
}

static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    (void)panel;
    s_on = on;
    send_panel();
    return ESP_OK;
}

static esp_err_t panel_disp_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    (void)panel;
    s_awake = !sleep;
    send_panel();
    return ESP_OK;
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x1, int y1, int x2, int y2,
                                   const void *data);

static esp_lcd_panel_t s_panel = {
    .reset = panel_reset, .init = panel_init, .del = panel_del,
    .draw_bitmap = panel_draw_bitmap, .disp_on_off = panel_disp_on_off,
    .disp_sleep = panel_disp_sleep,
};

esp_err_t __wrap_esp_lcd_new_panel_co5300(const esp_lcd_panel_io_handle_t io,
                                          const esp_lcd_panel_dev_config_t *cfg,
                                          esp_lcd_panel_handle_t *ret)
{
    (void)io; (void)cfg;
    if (!alloc_fbs()) {
        return ESP_ERR_NO_MEM;
    }
    *ret = PANEL;
    return ESP_OK;
}

/* kodeOS's wake from its GRAM rest rebuilds the scan-out alone. */
esp_err_t __wrap_esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus,
                                       const esp_lcd_dpi_panel_config_t *cfg,
                                       esp_lcd_panel_handle_t *ret)
{
    (void)bus; (void)cfg;
    if (!alloc_fbs()) {
        return ESP_ERR_NO_MEM;
    }
    *ret = PANEL;
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_panel_co5300_set_brightness(esp_lcd_panel_handle_t panel,
                                                     uint8_t percent)
{
    (void)panel;
    s_brightness = (percent > 100U) ? 100U : percent;
    send_panel();
    return ESP_OK;
}

/* ---- The DPI side -------------------------------------------------------- */

esp_err_t __wrap_esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t panel,
                                                    uint32_t fb_num, void **fb0, ...)
{
    va_list ap;
    (void)panel;
    if ((fb_num == 0U) || (fb_num > 2U) || !alloc_fbs()) {
        return ESP_ERR_INVALID_ARG;
    }
    *fb0 = s_fb[0];
    if (fb_num == 2U) {
        va_start(ap, fb0);
        void **fb1 = va_arg(ap, void **);
        va_end(ap);
        *fb1 = s_fb[1];
    }
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_dpi_panel_register_event_callbacks(
    esp_lcd_panel_handle_t panel, const esp_lcd_dpi_panel_event_callbacks_t *cbs,
    void *user_ctx)
{
    (void)panel;
    s_trans_done = cbs->on_color_trans_done;
    s_frame_done = cbs->on_frame_buf_complete;
    s_vsync = cbs->on_vsync;
    s_user = user_ctx;
    return ESP_OK;
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x1, int y1, int x2, int y2,
                                   const void *data)
{
    const uint8_t *const px = data;
    int const w = x2 - x1;
    int const h = y2 - y1;
    (void)panel;
    if ((w <= 0) || (h <= 0) || (x1 < 0) || (y1 < 0) || (x2 > (int)W) || (y2 > (int)H)) {
        return ESP_ERR_INVALID_ARG;
    }
    int flip = -1;
    for (int i = 0; i < 2; i++) {
        if ((px >= s_fb[i]) && (px < s_fb[i] + FB_BYTES)) {
            flip = i;
        }
    }
    if (flip >= 0) {
        /* The rows handed over are already in place; show that buffer. Only
         * they can differ from what the Mac has: kdrv_display hands over the
         * rows this frame painted together with those the last flip did, and
         * copies the rest from the shown buffer before drawing, so outside
         * the band the two buffers - and the Mac's picture - are the same.
         * The first picture, and the first flip after a copy into the shown
         * buffer (which the other one never got), are compared whole. */
        s_shown = (uint8_t)flip;
        bool const whole = !s_sent_valid || s_copied;
        send_changes(s_fb[flip], whole ? 0 : y1, whole ? (int)H : y2);
        s_copied = false;
    } else {
        uint8_t *const dst = s_fb[s_shown];
        for (int r = 0; r < h; r++) {
            memcpy(&dst[((size_t)(y1 + r) * W + (size_t)x1) * 2U],
                   &px[(size_t)r * (size_t)w * 2U], (size_t)w * 2U);
        }
        send_changes(dst, s_sent_valid ? y1 : 0, s_sent_valid ? y2 : (int)H);
        s_copied = true;
    }
    if (s_trans_done != NULL) {
        esp_lcd_dpi_panel_event_data_t ev;
        (void)s_trans_done(PANEL, &ev, s_user);
    }
    return ESP_OK;
}
