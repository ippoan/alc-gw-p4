# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# esp32p4 は内蔵無線を持たないため、有線 Ethernet (IP101GRI PHY) で通信する。
# IDF 標準の ethernet_init コンポーネントを流用する — espressif/esp-webrtc-solution
# の whip_demo が esp32p4 ターゲットで使っているのと同じパターン
# (docs/hardware-notes.md の PHY GPIO 設定は sdkconfig.defaults 側で指定)。
#
# project() / include(project.cmake) より前のこの時点では IDF_TARGET は
# ESP-IDF 自身の「sdkconfig.defaults から推測」ロジック (project.cmake 内、
# この if より後) ではまだ解決されていないため、ここでは自前で判定する必要が
# ある。判定元は 2 通りありどちらも有効な経路として実際に確認した:
#   - `idf.py set-target esp32p4` 経由 (ローカル開発) は `-DIDF_TARGET=esp32p4`
#     という cmake cache 変数として渡ってくる (env var ではない — ここは
#     以前 $ENV{IDF_TARGET} のみを見ていて実際には常に false だった。
#     `idf.py set-target` は os.environ を設定しないため — ローカルでの
#     初回ビルドで実際に踏んだ)。
#   - CI (espressif/esp-idf-ci-action) は set-target を経由せず、素の環境変数
#     IDF_TARGET だけを渡して直接 build する。
# 両方を OR で見る。
if("$ENV{IDF_TARGET}" STREQUAL "esp32p4" OR "${IDF_TARGET}" STREQUAL "esp32p4")
    set(EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/ethernet/basic/components/ethernet_init")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(alc_gw_p4_relay)
