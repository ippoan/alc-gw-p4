# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# esp32p4 は内蔵無線を持たないため、有線 Ethernet (IP101GRI PHY) で通信する。
# IDF 標準の ethernet_init コンポーネントを流用する — espressif/esp-webrtc-solution
# の whip_demo が esp32p4 ターゲットで使っているのと同じパターン
# (docs/hardware-notes.md の PHY GPIO 設定は sdkconfig.defaults 側で指定)。
#
# $ENV{IDF_TARGET} を見る (${IDF_TARGET} という素の CMake 変数ではない):
# project() / include(project.cmake) より前のこの時点では IDF_TARGET は
# まだ CMake 変数として解決されておらず、確実に見えるのは環境変数のみ。
# `idf.py set-target esp32p4 && idf.py build` (ローカル開発) では
# set-target が環境変数を設定するため通るが、CI (espressif/esp-idf-ci-action)
# は set-target を経由せず IDF_TARGET 環境変数だけを渡して直接 build するため、
# ${IDF_TARGET} のままだと条件が常に false になり ethernet_init が解決できず
# ビルドエラーになる (実際に CI で踏んだ)
if("$ENV{IDF_TARGET}" STREQUAL "esp32p4")
    set(EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/ethernet/basic/components/ethernet_init")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(alc_gw_p4_relay)
