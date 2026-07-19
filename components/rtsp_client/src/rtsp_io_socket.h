// rtsp_client の既定 io 実装 (lwip BSD socket)。esp32p4 等の実機専用
// (CMakeLists.txt で linux target からは除外している)。io_ctx は
// rtsp_client_t が保持する int (ソケットfd) へのポインタを渡す想定。
#pragma once

#include "rtsp_client.h"

extern const rtsp_io_ops_t rtsp_io_socket_ops;
