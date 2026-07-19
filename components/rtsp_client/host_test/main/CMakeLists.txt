idf_component_register(
    SRCS "test_main.c" "test_rtsp_digest.c" "test_rtsp_sdp.c" "test_rtsp_client.c"
    INCLUDE_DIRS "."
    REQUIRES unity rtsp_client
)
