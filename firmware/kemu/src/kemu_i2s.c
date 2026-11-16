/**
 * @file kemu_i2s.c
 * @brief The I2S's word clock kept off its pad, in the simulator only.
 *
 * kdrv_es8311 routes the ES8311's clocks to their pads, as the board needs.
 * esp-emu 1.0.0 drives the word clock (WS, GPIO 37) on its pad, against its
 * own guide, and publishes every edge to kode-sim over --cosim-tcp: some
 * 96,000 messages a second of emulated time while anything plays, which the
 * emulator builds and kode-sim drops (espressif/esp-emulator#52). The sound
 * itself reaches kode-sim as I2S data, not through any pad, so the word
 * clock is left unrouted here: the channels are configured exactly as asked
 * otherwise. Goes when esp-emu keeps the clock off the pad.
 */
#include "driver/i2s_std.h"

esp_err_t __real_i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg);
esp_err_t __real_i2s_channel_reconfig_std_gpio(i2s_chan_handle_t handle, const i2s_std_gpio_config_t *gpio_cfg);

esp_err_t __wrap_i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg)
{
    if (std_cfg == NULL) {
        return __real_i2s_channel_init_std_mode(handle, std_cfg);
    }
    i2s_std_config_t cfg = *std_cfg;
    cfg.gpio_cfg.ws = I2S_GPIO_UNUSED;
    return __real_i2s_channel_init_std_mode(handle, &cfg);
}

esp_err_t __wrap_i2s_channel_reconfig_std_gpio(i2s_chan_handle_t handle, const i2s_std_gpio_config_t *gpio_cfg)
{
    if (gpio_cfg == NULL) {
        return __real_i2s_channel_reconfig_std_gpio(handle, gpio_cfg);
    }
    i2s_std_gpio_config_t cfg = *gpio_cfg;
    cfg.ws = I2S_GPIO_UNUSED;
    return __real_i2s_channel_reconfig_std_gpio(handle, &cfg);
}
