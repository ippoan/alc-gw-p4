# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# esp32p4 は内蔵無線を持たないため、有線 Ethernet (IP101GRI PHY) で通信する。
# IDF 標準の ethernet_init コンポーネントを流用する — espressif/esp-webrtc-solution
# の whip_demo が esp32p4 ターゲットで使っているのと同じパターン
# (docs/hardware-notes.md の PHY GPIO 設定は sdkconfig.defaults 側で指定)
if("${IDF_TARGET}" STREQUAL "esp32p4")
    set(EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/ethernet/basic/components/ethernet_init")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(alc_gw_p4_relay)
