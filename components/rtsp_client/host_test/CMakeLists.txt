# components/rtsp_client (このディレクトリの1つ上) を単一コンポーネントとして
# 直接登録する host テスト専用プロジェクト。target=linux でビルド・実行し、
# 実機 (esp32p4) が無くても digest/SDP パース等のロジックを CI で検証する。
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rtsp_client_host_test)
