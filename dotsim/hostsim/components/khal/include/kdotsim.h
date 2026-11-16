/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdotsim.h
 * @brief The simulated Kode Dot board: shared state between the sim drivers and the bridge.
 *
 * The bridge (kdotsim_bridge.c) runs in a plain pthread, never a FreeRTOS task,
 * because a FreeRTOS task on the linux port must not block in a system call.
 * It talks to the browser server over a unix socket. Everything here is
 * lock-free or under a pthread mutex held for a copy only.
 */
#ifndef KDOTSIM_H_
#define KDOTSIM_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KDOTSIM_W 410U
#define KDOTSIM_H 502U

/* Buttons, one bit each, in kdrv_button_t order. */
#define KDOTSIM_BTN_UP    (1U << 0)
#define KDOTSIM_BTN_DOWN  (1U << 1)
#define KDOTSIM_BTN_LEFT  (1U << 2)
#define KDOTSIM_BTN_RIGHT (1U << 3)
#define KDOTSIM_BTN_TOP   (1U << 4)
#define KDOTSIM_BTN_BOT   (1U << 5)

/** @brief Start the bridge thread (once). Reads KODE_SIM_SOCK from the environment. */
void kdotsim_start(void);

/** @brief Buttons held now (KDOTSIM_BTN_*). */
uint32_t kdotsim_buttons(void);

/** @brief The touch point: true while a finger is down. */
bool kdotsim_touch(uint16_t *x, uint16_t *y);

/** @brief Hand a finished frame (RGB565, KDOTSIM_W x KDOTSIM_H, row stride in pixels) to the bridge. */
void kdotsim_frame(const uint16_t *px, uint32_t stride_px, int32_t y1, int32_t y2);

/** @brief The LED colour and the brightness of the panel, for the browser. */
void kdotsim_led(uint8_t r, uint8_t g, uint8_t b);
void kdotsim_brightness(uint8_t percent);

/** @brief A haptic pulse, a tone, or a block of PCM samples (16-bit mono). */
void kdotsim_haptic(uint32_t ms);
void kdotsim_tone(uint32_t hz, uint32_t ms, uint8_t volume);
void kdotsim_pcm(const int16_t *samples, size_t n, uint32_t rate);

/** @brief One log line to the browser's console. */
void kdotsim_log(const char *line, size_t len);

/** @brief The app left (kode_exit, restart, panic): tell the browser, then end the process. */
void kdotsim_exit(const char *why) __attribute__((noreturn));

/** @brief Where the sim keeps its files (KODE_SIM_DATA, default ./simdata). */
const char *kdotsim_data_dir(void);

/** @brief Send one text message over the link to the paired device (the hub in the sim). */
void kdotsim_link_send(const char *msg, size_t len);

/** @brief Take the next link message in; returns its length, or -1 when none waits. */
int kdotsim_link_recv(char *buf, size_t cap);

/** @brief Copy a new mirror picture of the paired device's screen; false when none arrived. */
bool kdotsim_mirror(uint16_t *dst, uint16_t *w, uint16_t *h, size_t cap_px);

/** @brief The battery's charge in percent and whether USB power is attached (the page's `p` line). */
void kdotsim_battery_set(uint8_t pct, bool usb);
void kdotsim_battery_get(uint8_t *pct, bool *usb);

/** @brief Motion sensors (the page's `i` line), in this order: acceleration x y z in milli-g,
 *         rotation x y z in milli-degrees/s, magnetic field x y z in milligauss, temperature in tenths of a degree. */
#define KDOTSIM_MOTION_N 10U
void kdotsim_motion_set(const int32_t v[KDOTSIM_MOTION_N]);
void kdotsim_motion_get(int32_t v[KDOTSIM_MOTION_N]);

/** @brief The microSD card in the slot or not (the page's `s` line); khal_sdmmc_sim.c keeps it. The card is the
 *         folder <data dir>/sd. */
void kdotsim_sd_set(bool in);
bool kdotsim_sd_in(void);

/** @brief Test probe (kdrv_probe_sim.c): what the SDK's drivers read back for "bat", "imu" or "sd", as the
 *         text line the bridge answers with; returns its length, 0 when a driver is not up yet. */
int kdotsim_probe(const char *what, char *out, size_t cap);

/* ---- The board, below the drivers: the I2C chips and the GPIO lines ---- */

/** @brief Called by the GPIO model when an input line changes (from the bridge). */
void kdotsim_gpio_set(uint8_t pin, bool level);

/** @brief The bridge tells the board that the inputs changed; the board updates its chips and lines. */
void kdotsim_inputs_changed(void);

#ifdef __cplusplus
}
#endif

#endif /* KDOTSIM_H_ */
