# kemu: what esp-emu does not model of the Kode Dot yet, made virtual under
# unchanged kode-sdk drivers: the panel (MIPI-DSI), the microSD (SDMMC), the
# C5's radio, the USB device port (TinyUSB's controller driver, with the page
# as the host), the console on UART1 and the RTC's time. The board's I2C chips
# and pins, and the sound (I2S, since esp-emu 1.0.0), are esp-emu's now,
# answered by kode-sim over --cosim-tcp; kemu only keeps the I2S's word clock
# off its pad (esp-emulator#52). Each piece here goes when esp-emu models it.
#
# WHOLE_ARCHIVE because nothing in the firmware calls into kemu by name: it
# is reached only through the --wrap'd symbols below, and the linker would
# otherwise drop it.
idf_component_register(
    SRCS "src/kemu_frame.c" "src/kemu_link.c" "src/kemu_panel.c" "src/kemu_usb.c" "src/kemu_console.c" "src/kemu_radio.c" "src/kemu_net.c" "src/kemu_sd.c" "src/kemu_clock.c" "src/kemu_i2s.c"
         "models/sdcard.c" "models/usbdcd.c"
    INCLUDE_DIRS "include" "models"
    REQUIRES kbsp khal
    PRIV_REQUIRES kaos kqp esp_driver_sdmmc sdmmc esp_eth esp_netif esp_event esp_driver_uart esp_driver_gptimer esp_driver_i2s esp_timer esp_partition spi_flash esp_app_format esp_rom freertos esp_lcd
    WHOLE_ARCHIVE)

# Every symbol kemu stands in for. A driver's call to one of these lands in
# __wrap_<sym> here; nothing else changes.
set(KEMU_WRAPS
    app_main
    esp_lcd_new_dsi_bus esp_lcd_del_dsi_bus esp_lcd_new_panel_io_dbi
    esp_lcd_new_panel_co5300 esp_lcd_new_panel_dpi esp_lcd_panel_co5300_set_brightness
    esp_lcd_dpi_panel_get_frame_buffer esp_lcd_dpi_panel_register_event_callbacks
    tinyusb_driver_install usb_new_phy usb_del_phy
    dcd_init dcd_deinit dcd_int_handler dcd_int_enable dcd_int_disable dcd_set_address
    dcd_remote_wakeup dcd_connect dcd_disconnect dcd_sof_enable dcd_edpt0_status_complete
    dcd_edpt_open dcd_edpt_close dcd_edpt_close_all dcd_edpt_iso_alloc dcd_edpt_iso_activate
    dcd_edpt_xfer dcd_edpt_xfer_fifo dcd_edpt_stall dcd_edpt_clear_stall
    sdmmc_host_init sdmmc_host_init_slot sdmmc_host_deinit_slot sdmmc_host_set_bus_width
    sdmmc_host_get_slot_width sdmmc_host_set_card_clk sdmmc_host_get_real_freq
    sdmmc_host_set_bus_ddr_mode sdmmc_host_set_cclk_always_on sdmmc_host_set_input_delay
    sdmmc_host_set_input_delayline sdmmc_host_check_buffer_alignment
    sdmmc_host_is_slot_set_to_uhs1 sdmmc_host_io_int_enable sdmmc_host_io_int_wait
    sdmmc_host_do_transaction
    usb_serial_jtag_read_bytes
    i2s_channel_init_std_mode i2s_channel_reconfig_std_gpio)

foreach(sym ${KEMU_WRAPS})
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${sym}")
endforeach()
