cmake_minimum_required(VERSION 3.20.0)

set(BOARD esp32_devkitc/esp32/procpu)
set(DTC_OVERLAY_FILE boards/esp32_devkitc.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(simcomdemo)

target_sources(app PRIVATE src/main.c)
