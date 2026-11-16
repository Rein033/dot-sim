/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_sim.c
 * @brief The ESP32-C5 with the radio off: held in reset, and a peer link
 *        that never comes up (so the infrared, which the C5 serves, is absent).
 */
#include "kdrv_radio.h"

#include "kbsp.h"
#include "khal_gpio.h"

static bool s_held;

kode_status_t kdrv_radio_c5_hold(bool hold)
{
    kode_status_t const rc = khal_gpio_output((uint8_t)KBSP_PIN_C5_EN, !hold);
    if ( KODE_OK == rc )
    {
        s_held = hold;
    }
    return rc;
}

bool kdrv_radio_c5_held(void)
{
    return s_held;
}

kode_status_t kdrv_radio_peer_listen(uint32_t msg_id, kdrv_radio_peer_cb_t cb, kdrv_radio_peer_up_t up, void *ctx)
{
    (void)msg_id; (void)cb; (void)up; (void)ctx;
    return KODE_OK;
}

kode_status_t kdrv_radio_peer_send(uint32_t msg_id, const uint8_t *data, size_t len)
{
    (void)msg_id; (void)data; (void)len;
    return KODE_IO_ERROR;
}
