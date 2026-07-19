# esp_media_protocols (プリビルド、RTSP Digest 認証未対応) の置き換え用
# 自前 RTSP クライアント。alc-gw-p4#1 の実機検証で判明した認証ギャップの解消。
#
# rtsp_io_socket.c (lwip BSD socket) は host_test (linux target) では
# ビルド対象から除外する。host テストはモックioを注入するため実ソケット
# 実装を必要とせず、linux targetでもlwip自体は使えるが不要な依存を増やさない。
set(rtsp_client_srcs "src/rtsp_digest.c" "src/rtsp_sdp.c" "src/rtsp_client.c" "src/rtsp_demux.c" "src/h264_depacket.c")
set(rtsp_client_requires "mbedtls")
if(NOT IDF_TARGET STREQUAL "linux")
    list(APPEND rtsp_client_srcs "src/rtsp_io_socket.c")
    list(APPEND rtsp_client_requires "lwip")
endif()

idf_component_register(
    SRCS ${rtsp_client_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${rtsp_client_requires}
)
