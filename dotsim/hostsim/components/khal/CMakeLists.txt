# khal for the sim: the board's HAL on the host. The portable sources are the
# SDK's own; the ones that touch the chip are sim versions (src/).
set(SDK "${CMAKE_CURRENT_LIST_DIR}/../../sdk/components/khal")

file(GLOB khal_incs LIST_DIRECTORIES true "${SDK}/*/include")

idf_component_register(
    SRCS
        "${SDK}/crc32/src/khal_crc32_esp.c"
        "${SDK}/zlib/src/khal_zlib_esp.c"
        "${SDK}/sync/src/khal_once.c"
        "src/khal_sync_sim.c"
        "${SDK}/worker/src/khal_worker_esp.c"
        "${SDK}/time/src/khal_time_esp.c"
        "${SDK}/time/src/khal_time_zone.c"
        "src/khal_nvs_sim.c"
        "${SDK}/exp/src/khal_exp.c"
        "${SDK}/pwm/src/khal_pwm.c"
        "src/khal_gpio_sim.c"
        "src/khal_i2c_sim.c"
        "src/khal_misc_sim.c"
        "src/khal_system_sim.c"
        "src/kdotsim_bridge.c"
        "src/esp_timer_sim.c"
    INCLUDE_DIRS ${khal_incs} "include"
    PRIV_INCLUDE_DIRS "${SDK}/exp/src" "${SDK}/pwm/src"
    REQUIRES kdefs klog kbsp
    PRIV_REQUIRES esp_timer esp_rom freertos esp_partition
)
target_link_libraries(${COMPONENT_LIB} PRIVATE pthread)
