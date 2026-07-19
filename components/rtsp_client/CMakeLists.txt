# esp_media_protocols (プリビルド、RTSP Digest 認証未対応) の置き換え用
# 自前 RTSP クライアント。alc-gw-p4#1 の実機検証で判明した認証ギャップの解消。
#
# PR1 時点では digest 計算のみ (mbedtls の md5 だけに依存、target を問わず
# ビルド可能)。PR3 で lwip ソケットを使う実装ファイルを追加する際は、
# host_test (linux target) から除外するため target 分岐を追加すること。
idf_component_register(
    SRCS "src/rtsp_digest.c" "src/rtsp_sdp.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls
)
