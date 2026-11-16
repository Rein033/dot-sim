# kdrv for the sim: the SDK's own drivers wherever they speak only khal (they
# then run on the sim's I2C chips), sim versions where they speak to a chip
# through ESP-IDF peripherals (src/).
set(D "${CMAKE_CURRENT_LIST_DIR}/../../sdk/components/kdrv")

set(srcs
    "${D}/led/src/kdrv_led.c"
    "${D}/clocks/src/kdrv_clocks.c"
    "${D}/tca6408a/src/kdrv_tca6408a.c"
    "${D}/cst820/src/kdrv_cst820.c"
    "${D}/buttons/src/kdrv_buttons.c"
    "${D}/ir/src/kdrv_ir.c"
    "${D}/ir/src/knec.c"
    "${D}/audio/src/kes_fields.c"
    "${D}/audio/src/kring.c"
    "${D}/audio/src/ktone.c"
    "${D}/audio/src/kwav.c"
    "${D}/motion/src/kimu_energy.c"
    "${D}/motion/src/kcompass.c"
    "${D}/motion/src/kdrv_qmi8658.c"
    "${D}/motion/src/kdrv_qmc5883p.c"
    "${D}/motion/src/kdrv_aw86233.c"
    "${D}/power/src/kdrv_bq25896.c"
    "${D}/power/src/kdrv_cw2217.c"
    "${D}/storage/src/kdrv_storage.c"
    "${D}/storage/src/kdrv_sd.c"
    "${D}/apps/src/kdrv_apps.c"
    "${D}/apps/src/kdrv_kfw.c"
    "${D}/apps/src/kdrv_image_check.c"
    "${D}/apps/src/kapp_desc.c"
    "${D}/apps/src/kapp_cat.c"
    "${D}/apps/src/kapp_data.c"
    "${D}/nfc/src/knfc_crypto1.c"
    "${D}/nfc/src/knfc_emu_img.c"
    "${D}/nfc/src/knfc_field.c"
    "${D}/nfc/src/knfc_mfc.c"
    "${D}/nfc/src/knfc_mfc_dict.c"
    "${D}/nfc/src/knfc_ndef.c"
    "${D}/nfc/src/knfc_power.c"
    "${D}/nfc/src/knfc_ident.c"
    "${D}/nfc/src/knfc_tag.c"
    "${D}/rfid/src/krfid_em4100.c"
    "${D}/rfid/src/krfid_env.c"
    "${D}/rfid/src/krfid_mod.c"
    "${D}/rfid/src/krfid_wave.c"
    "src/kdrv_radio_sim.c"
    "src/kdrv_display_sim.c"
    "src/kdrv_es8311_sim.c"
    "src/kdrv_absent_sim.c"
    "src/kdrv_probe_sim.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "${D}/apps/include" "${D}/audio/include" "${D}/buttons/include"
        "${D}/clocks/include" "${D}/cst820/include" "${D}/display/include"
        "${D}/exp_bus/include" "${D}/ir/include" "${D}/led/include"
        "${D}/mirror/include" "${D}/motion/include" "${D}/nfc/include"
        "${D}/ov5640/include" "${D}/power/include" "${D}/radio/include"
        "${D}/rfid/include" "${D}/station/include" "${D}/storage/include"
        "${D}/tca6408a/include"
    PRIV_INCLUDE_DIRS "${D}/apps/src" "${D}/radio/src"
        "$ENV{IDF_PATH}/components/bootloader_support/include"
    REQUIRES kdefs klog khal kbsp
    PRIV_REQUIRES esp_timer freertos esp_partition esp_app_format esp_rom mbedtls
)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,kdotsim_probe")   # khal's bridge calls it; keep it in the link
