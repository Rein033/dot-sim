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
TOP_CMAKE = '''cmake_minimum_required(VERSION 3.22)
# A Kode Dot app on the kode SDK, in the shape of kodediy/kode-app-template.
set(IDF_TARGET esp32p4)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "01.00.00")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
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
SDKCONFIG = '''CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="managed_components/kodediy__kbsp/partitions.csv"
CONFIG_KODEOS_BOOT_ROLE_APP=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_KODEOS_RADIO=n
CONFIG_ESP_HOSTED_ENABLED=n
CONFIG_BT_ENABLED=n
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
