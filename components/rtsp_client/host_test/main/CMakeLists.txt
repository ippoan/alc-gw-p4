idf_component_register(
    SRCS "test_main.c" "test_rtsp_digest.c"
    INCLUDE_DIRS "."
    REQUIRES unity rtsp_client
)
