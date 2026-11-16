"""Assemble each app under src/<id>.c into a full ESP-IDF app dir: <id>/ (kode-app-template's shape)."""
import json, os, re, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
APPS = {
  "slang":         ("Snake",         "#46BE5A", "games", "Eat apples, grow, don't bite yourself."),
  "muurbreker":    ("Wall Breaker",  "#F58228", "games", "Bounce the ball and break every brick."),
  "blokkenregen":  ("Block Rain",    "#9658DC", "games", "Spin and stack falling blocks."),
  "schuif":        ("Slide",         "#FAC828", "games", "Slide tiles, add them up to 2048."),
  "fladder":       ("Flutter",       "#3CC8DC", "games", "Flap between the pillars."),
  "tafeltennis":   ("Table Tennis",  "#028AC2", "games", "Pong against the Dot: first to 7 wins."),
  "kleurgeheugen": ("Color Memory",  "#C9303D", "games", "Remember the colors and tones."),
  "krabbel":       ("Doodle",        "#F06EAA", "tools", "Draw on the glass with your finger."),
  "stopwatch":     ("Stopwatch",     "#E8EAED", "tools", "Stopwatch with lap times."),
  "reflex":        ("Reflex",        "#46BE5A", "games", "How fast do you react to green?"),
}
# kodediy/kode-app-template's CMakeLists.txt (GPL-3.0), with the app's name: it fetches kbsp's board settings first.
TOP_CMAKE = '''cmake_minimum_required(VERSION 3.22)

# ---- The chip, which is the one board fact spelled out here ---------------
#
# Everything else about the Kode Dot arrives in kbsp's sdkconfig.board. The
# chip cannot, because it has to be known *before* that file has been fetched:
# ESP-IDF resolves the target from this variable, then from the CMake cache,
# then by guessing from whatever sdkconfig files already exist - and on a clean
# checkout there are none, so it falls back to `esp32`. The bootstrap below
# would then configure an ESP32, cache that, and refuse every later pass.
#
# CONFIG_PARTITION_TABLE_CUSTOM_FILENAME in sdkconfig.defaults is spelled out
# for the same class of reason: both are needed before the shared file they
# would otherwise come from can be resolved.
set(IDF_TARGET esp32p4)

# ---- The board's settings, fetched rather than copied ---------------------
#
# Never write flash, PSRAM or cache facts into this project. kbsp describes
# the board and travels with the SDK; a private copy is how an app once
# declared PSRAM this chip does not have, and the symptom was a hung boot with
# no message.
#
# The awkward part is timing, and it is ESP-IDF's: SDKCONFIG_DEFAULTS is read
# at project() time, and the component manager that fetches kbsp runs *inside*
# project(). So on a clean checkout the file this project wants does not exist
# yet.
#
# So a clean checkout configures twice, and the user sees one build. The first
# configure is a throwaway, run from here in a folder of its own, purely so
# that the manager fetches: managed_components/ is the project's, so what it
# fetches is there for the real configure that follows. Its sdkconfig is kept
# in that folder as well. It was written with ESP-IDF's defaults - no PSRAM,
# 2MB of flash - and in the project it would win from then on, because a
# cached value beats a new default every time.
set(KBSP_BOARD
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/kodediy__kbsp/sdkconfig.board")

if(NOT EXISTS "${KBSP_BOARD}" AND NOT KBSP_FETCH_PASS)
    message(STATUS "Fetching the SDK and the board settings (first build only)")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -G "${CMAKE_GENERATOR}"
                -S "${CMAKE_CURRENT_LIST_DIR}"
                -B "${CMAKE_BINARY_DIR}/kbsp_fetch"
                -DKBSP_FETCH_PASS=ON
                "-DSDKCONFIG=${CMAKE_BINARY_DIR}/kbsp_fetch/sdkconfig"
        RESULT_VARIABLE KBSP_FETCH_RC)
    file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/kbsp_fetch")
    if(NOT EXISTS "${KBSP_BOARD}")
        message(FATAL_ERROR
            "The SDK could not be fetched (cmake ended with ${KBSP_FETCH_RC}). "
            "Read the first error above. A refused git fetch means that your "
            "GitHub account does not list your SSH key.")
    endif()
endif()

if(EXISTS "${KBSP_BOARD}")
    set(SDKCONFIG_DEFAULTS
        "${KBSP_BOARD};${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
else()
    # The throwaway configure, which runs without the board settings.
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()

# ---- The app's version -----------------------------------------------------
#
# Explicit rather than left to ESP-IDF's git-describe default, which stamps a
# hash like "0d14d62-dirty" - and the launcher prints whatever is here under
# the name on the pill. Bare, so `-DPROJECT_VER=...` still overrides it.
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "01.00.00")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The project name is the app's identity on the Dot: the launcher tracks the
# app by it, and the image is build/<name>.bin. Rename it first.
project(%s)
'''
MAIN_CMAKE = '''idf_component_register(
    SRCS "app_main.c" "kk.c"
    INCLUDE_DIRS "."
    REQUIRES kode
    PRIV_REQUIRES kdrv khal kaos kqp
)
'''
COMPONENT_YML = '''dependencies:
  kodediy/kode:
    git: https://github.com/kodediy/kode-sdk.git
    path: components/kode
    version: v0.1.1
'''
# kode-app-template's sdkconfig.defaults, plus speed for the games.
SDKCONFIG = '''# This app's own settings. Everything about the board - flash, PSRAM, cache,
# the console - arrives from kbsp's sdkconfig.board, which CMakeLists.txt
# fetches and reads first. Do not copy any of it here.

CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="managed_components/kodediy__kbsp/partitions.csv"

# The role AO_Boot plays, and the safety property the slot model rests on:
# this firmware never writes otadata, so any reset returns to kodeOS.
CONFIG_KODEOS_BOOT_ROLE_APP=y

# The console over the USB-C cable: somewhere to watch the app from on the
# bench.
CONFIG_KODEOS_CONSOLE=y

# No radio until the app needs one: it is the most expensive thing an app can
# bring up for nothing. To turn the network on, delete the three lines below.
CONFIG_KODEOS_RADIO=n

# Turning AO_Radio off is not enough to keep esp-hosted out: kdrv depends on
# esp_wifi_remote, whose presence makes ESP_HOSTED_ENABLED default to y, and
# the component then installs a constructor that claims the SDMMC controller
# before app_main. KODEOS_RADIO selects the symbol; Kconfig cannot express
# "deselect", so a firmware that wants no radio says the opposite out loud.
CONFIG_ESP_HOSTED_ENABLED=n

# No radio, so no BLE host: the board file turns the NimBLE lines on.
CONFIG_BT_ENABLED=n

# The SDK's own log lines are compiled in at every level, and most of them are
# INFO: a shipped app that will never print them can compile them away, which
# is well over a hundred kB of format strings.
# CONFIG_KLOG_MAXIMUM_LEVEL_WARN=y

CONFIG_COMPILER_OPTIMIZATION_PERF=y
'''
only = sys.argv[1:]
for app_id, (name, color, cat, desc) in APPS.items():
    if only and app_id not in only:
        continue
    src = os.path.join(HERE, "src", app_id + ".c")
    if not os.path.exists(src):
        print("missing:", app_id); continue
    d = os.path.join(HERE, app_id)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "main")); os.makedirs(os.path.join(d, "appdata"))
    open(os.path.join(d, "CMakeLists.txt"), "w").write(TOP_CMAKE % app_id)
    open(os.path.join(d, "sdkconfig.defaults"), "w").write(SDKCONFIG)
    open(os.path.join(d, "main", "CMakeLists.txt"), "w").write(MAIN_CMAKE)
    open(os.path.join(d, "main", "idf_component.yml"), "w").write(COMPONENT_YML)
    code = open(src).read()
    code = code.replace("@NAME@", name).replace("@CAT@", cat)
    open(os.path.join(d, "main", "app_main.c"), "w").write(code)
    for f in ("kk.c", "kk.h", "font5x7.h"):
        shutil.copy(os.path.join(HERE, "kodekit", f), os.path.join(d, "main", f))
    json.dump({"kode": "1", "color": color, "description": desc[:52]}, open(os.path.join(d, "appdata", "app.json"), "w"))
    icon = os.path.join(HERE, "icons", app_id + ".png")
    if os.path.exists(icon):
        shutil.copy(icon, os.path.join(d, "appdata", "icon.png"))
    print("ok", app_id)
