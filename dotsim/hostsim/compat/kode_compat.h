/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_compat.h
 * @brief The sim's bridge for apps written against the newer kode SDK, where
 *        the Active Object layer is kode_ao.h / kode.h with kode_X names and
 *        the public SDK has it as kode_sys.h with kode_sys_X names.
 *
 * Generated from kode_sys.h; only names the public kode.h does not already
 * use are aliased. kode_audio_tone() and kode_haptic_play() exist in both
 * with other arguments, so they dispatch on their argument count.
 */
#ifndef KODE_COMPAT_H_
#define KODE_COMPAT_H_

#include "kode_sys.h"
#include "kode_variables.h"
#include "kode_functions.h"
#include "kode_log.h"
#include "kaos.h"

#define kode_audio_info_read kode_sys_audio_info_read
#define kode_audio_mic_gain kode_sys_audio_mic_gain
#define kode_audio_mute kode_sys_audio_mute
#define kode_audio_record kode_sys_audio_record
#define kode_audio_stream_close kode_sys_audio_stream_close
#define kode_audio_stream_open kode_sys_audio_stream_open
#define kode_audio_stream_write kode_sys_audio_stream_write
#define kode_audio_volume kode_sys_audio_volume
#define kode_battery_read kode_sys_battery_read
#define kode_ble_enabled kode_sys_ble_enabled
#define kode_ble_service_fault kode_sys_ble_service_fault
#define kode_ble_set_data kode_sys_ble_set_data
#define kode_ble_subscribe kode_sys_ble_subscribe
#define kode_button_read_mask kode_sys_button_read_mask
#define kode_button_read_presses kode_sys_button_read_presses
#define kode_clock_local kode_sys_clock_local
#define kode_clock_read kode_sys_clock_read
#define kode_console_congested kode_sys_console_congested
#define kode_exit_to_os kode_sys_exit_to_os
#define kode_exp_adc kode_sys_exp_adc
#define kode_exp_ctrl kode_sys_exp_ctrl
#define kode_exp_info kode_sys_exp_info
#define kode_exp_input kode_sys_exp_input
#define kode_exp_irq kode_sys_exp_irq
#define kode_exp_irq_enable kode_sys_exp_irq_enable
#define kode_exp_lend kode_sys_exp_lend
#define kode_exp_level kode_sys_exp_level
#define kode_exp_output kode_sys_exp_output
#define kode_exp_power kode_sys_exp_power
#define kode_exp_pwm kode_sys_exp_pwm
#define kode_exp_release kode_sys_exp_release
#define kode_exp_return kode_sys_exp_return
#define kode_exp_write kode_sys_exp_write
#define kode_flash kode_sys_flash
#define kode_haptic_hold kode_sys_haptic_hold
#define kode_haptic_is_holding kode_sys_haptic_is_holding
#define kode_haptic_play_buzz kode_sys_haptic_play_buzz
#define kode_haptic_play_pattern kode_sys_haptic_play_pattern
#define kode_haptic_stop kode_sys_haptic_stop
#define kode_home_defer kode_sys_home_defer
#define kode_home_now kode_sys_home_now
#define kode_ir_capture kode_sys_ir_capture
#define kode_ir_send_nec kode_sys_ir_send_nec
#define kode_led_set kode_sys_led_set
#define kode_mag_cal_clear kode_sys_mag_cal_clear
#define kode_mag_cal_get kode_sys_mag_cal_get
#define kode_mag_cal_set kode_sys_mag_cal_set
#define kode_mqtt_status kode_sys_mqtt_status
#define kode_net_lookup kode_sys_net_lookup
#define kode_nfc_is_emulating kode_sys_nfc_is_emulating
#define kode_nfc_ndef_read kode_sys_nfc_ndef_read
#define kode_nfc_ndef_write_text kode_sys_nfc_ndef_write_text
#define kode_nfc_ndef_write_uri kode_sys_nfc_ndef_write_uri
#define kode_nfc_read_tag kode_sys_nfc_read_tag
#define kode_nvs_get kode_sys_nvs_get
#define kode_power_boost kode_sys_power_boost
#define kode_power_charge kode_sys_power_charge
#define kode_power_charge_limit kode_sys_power_charge_limit
#define kode_power_sleep kode_sys_power_sleep
#define kode_rfid_is_emulating kode_sys_rfid_is_emulating
#define kode_rfid_read kode_sys_rfid_read
#define kode_screen_brightness kode_sys_screen_brightness
#define kode_time_ms kode_sys_time_ms
#define kode_touch_subscribe kode_sys_touch_subscribe
#define kode_wifi_frames kode_sys_wifi_frames
#define kode_wifi_status kode_sys_wifi_status
#define kode_buttons_read kode_sys_button_read_mask
#define kode_led_rgb kode_sys_led_set_rgb

#define KSIM_ARGN_(_1, _2, _3, N, ...) N

static inline bool ksim_audio_tone_2(uint16_t hz, uint32_t ms) { return (kode_audio_tone)(hz, ms); }
static inline kode_status_t ksim_audio_tone_3(uint16_t hz, uint32_t ms, uint8_t vol)
{
    return kode_sys_audio_tone(hz, (uint16_t)((ms > 65535U) ? 65535U : ms), vol);
}
#define kode_audio_tone(...) KSIM_ARGN_(__VA_ARGS__, ksim_audio_tone_3, ksim_audio_tone_2, _)(__VA_ARGS__)

static inline bool ksim_haptic_play_1(const char *profile) { return (kode_haptic_play)(profile); }
static inline kode_status_t ksim_haptic_play_2(uint16_t ms, uint8_t gain) { return kode_sys_haptic_play_buzz(ms, gain); }
#define kode_haptic_play(...) KSIM_ARGN_(__VA_ARGS__, _, ksim_haptic_play_2, ksim_haptic_play_1)(__VA_ARGS__)

#endif /* KODE_COMPAT_H_ */
