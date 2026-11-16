# kode-sim: one Kode Dot app, built for the host (ESP-IDF's linux target) on
# the real kode SDK, with the board simulated under it.
#
#   KODE_APP_DIR  the app's checkout (its main/ is this project's main)
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{KODE_APP_DIR})
    message(FATAL_ERROR "set KODE_APP_DIR to the app's checkout")
endif()
set(SDK "${CMAKE_CURRENT_LIST_DIR}/sdk/components")

set(EXTRA_COMPONENT_DIRS
    "${SDK}/kode"
    "${SDK}/kode_sys"
    "${SDK}/kaos"
    "${SDK}/kqp"
    "${SDK}/klog"
    "${SDK}/kdefs"
    "${SDK}/qpc"
    "$ENV{KODE_APP_DIR}/main")
if(EXISTS "$ENV{KODE_APP_DIR}/components")
    list(APPEND EXTRA_COMPONENT_DIRS "$ENV{KODE_APP_DIR}/components")
endif()

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_BINARY_DIR}/app.sdkconfig")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/components/khal/include/kode_sim_shim.h" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error;-m32" APPEND)
idf_build_set_property(LINK_OPTIONS "-m32" APPEND)
project(kode_sim_app)

# The app's own sources see the compat headers first (compat/kode.h wraps the
# real one with #include_next).
idf_component_get_property(main_lib main COMPONENT_LIB)
target_include_directories(${main_lib} BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat")

# The executable's own stub source is not a component: it needs -m32 too.
target_compile_options(${CMAKE_PROJECT_NAME}.elf PRIVATE -m32)
