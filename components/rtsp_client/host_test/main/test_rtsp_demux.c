// rtsp_demux の host テスト。RTSP over TCP の interleaved フレーミング
// ($<channel><len16><data>) が、任意の分割位置・混在テキストでも正しく
// デコードできることを検証する (alc-gw-p4#1 の最大リスク項目)。
#include <string.h>

#include "unity.h"
#include "rtsp_demux.h"

typedef struct {
    uint8_t last_channel;
    uint8_t last_payload[64];
    size_t last_len;
    int call_count;
} demux_capture_t;

static void capture_cb(uint8_t channel, const uint8_t *payload, size_t len, void *ctx) {
    demux_capture_t *c = (demux_capture_t *)ctx;
    c->last_channel = channel;
    size_t n = len < sizeof(c->last_payload) ? len : sizeof(c->last_payload);
    memcpy(c->last_payload, payload, n);
    c->last_len = len;
    c->call_count++;
}

TEST_CASE("demux: 完全なフレームを1回のfeedで処理", "[rtsp_demux]") {
    uint8_t frame_buf[128];
    demux_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    rtsp_demux_t d;
    rtsp_demux_init(&d, frame_buf, sizeof(frame_buf), capture_cb, &cap);

    uint8_t data[] = {0x24, 0x00, 0x00, 0x05, 'H', 'E', 'L', 'L', 'O'};
    rtsp_demux_feed(&d, data, sizeof(data));

    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_EQUAL(0, cap.last_channel);
    TEST_ASSERT_EQUAL(5, cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"HELLO", cap.last_payload, 5);
}

TEST_CASE("demux: 1バイトずつfeedしても同じ結果になる", "[rtsp_demux]") {
    uint8_t frame_buf[128];
    demux_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    rtsp_demux_t d;
    rtsp_demux_init(&d, frame_buf, sizeof(frame_buf), capture_cb, &cap);

    uint8_t data[] = {0x24, 0x01, 0x00, 0x04, 'A', 'B', 'C', 'D'};
    for (size_t i = 0; i < sizeof(data); i++) {
        rtsp_demux_feed(&d, &data[i], 1);
    }

    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_EQUAL(1, cap.last_channel);
    TEST_ASSERT_EQUAL(4, cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"ABCD", cap.last_payload, 4);
}

TEST_CASE("demux: フレーム境界(長さフィールド途中)をまたぐ分割でも処理できる", "[rtsp_demux]") {
    uint8_t frame_buf[128];
    demux_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    rtsp_demux_t d;
    rtsp_demux_init(&d, frame_buf, sizeof(frame_buf), capture_cb, &cap);

    uint8_t data[] = {0x24, 0x00, 0x00, 0x06, 'F', 'O', 'O', 'B', 'A', 'R'};
    rtsp_demux_feed(&d, data, 3); // 長さフィールドの2バイト目までで打ち切る
    rtsp_demux_feed(&d, data + 3, sizeof(data) - 3);

    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_EQUAL(6, cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"FOOBAR", cap.last_payload, 6);
}

TEST_CASE("demux: RTSPテキスト応答が混在していても$から正しく開始する", "[rtsp_demux]") {
    uint8_t frame_buf[128];
    demux_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    rtsp_demux_t d;
    rtsp_demux_init(&d, frame_buf, sizeof(frame_buf), capture_cb, &cap);

    const char *text = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n";
    rtsp_demux_feed(&d, (const uint8_t *)text, strlen(text));

    uint8_t frame[] = {0x24, 0x00, 0x00, 0x03, 'X', 'Y', 'Z'};
    rtsp_demux_feed(&d, frame, sizeof(frame));

    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_EQUAL(3, cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"XYZ", cap.last_payload, 3);
}

TEST_CASE("demux: バッファを超えるフレームは安全にスキップされ、次のフレームは正常処理される", "[rtsp_demux]") {
    uint8_t frame_buf[4]; // 意図的に小さいバッファ
    demux_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    rtsp_demux_t d;
    rtsp_demux_init(&d, frame_buf, sizeof(frame_buf), capture_cb, &cap);

    // 長さ10 (バッファ4を超える) → コールバックされずスキップされるはず
    uint8_t oversized[] = {0x24, 0x00, 0x00, 0x0A, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    rtsp_demux_feed(&d, oversized, sizeof(oversized));
    TEST_ASSERT_EQUAL(0, cap.call_count);

    // 長さ3、バッファに収まる → 正常に処理される
    uint8_t ok_frame[] = {0x24, 0x02, 0x00, 0x03, 'a', 'b', 'c'};
    rtsp_demux_feed(&d, ok_frame, sizeof(ok_frame));
    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_EQUAL(2, cap.last_channel);
    TEST_ASSERT_EQUAL(3, cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"abc", cap.last_payload, 3);
}
