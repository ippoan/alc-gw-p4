// h264_depacket の host テスト。単一NAL/STAP-A/FU-A の各パケタイズ形式と、
// シーケンス番号ギャップ検出→次のIDRまで破棄→復帰を検証する。
#include <string.h>

#include "unity.h"
#include "h264_depacket.h"

typedef struct {
    uint8_t last_au[256];
    size_t last_len;
    bool last_keyframe;
    int call_count;
} h264_capture_t;

static void h264_cb(const uint8_t *annexb, size_t len, bool keyframe, void *ctx) {
    h264_capture_t *c = (h264_capture_t *)ctx;
    size_t n = len < sizeof(c->last_au) ? len : sizeof(c->last_au);
    memcpy(c->last_au, annexb, n);
    c->last_len = len;
    c->last_keyframe = keyframe;
    c->call_count++;
}

static void build_rtp_header(uint8_t *buf, bool marker, uint8_t pt, uint16_t seq, uint32_t ts) {
    buf[0] = 0x80; // V=2,P=0,X=0,CC=0
    buf[1] = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7F));
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = (uint8_t)(seq & 0xFF);
    buf[4] = (uint8_t)(ts >> 24);
    buf[5] = (uint8_t)(ts >> 16);
    buf[6] = (uint8_t)(ts >> 8);
    buf[7] = (uint8_t)(ts & 0xFF);
    buf[8] = 0;
    buf[9] = 0;
    buf[10] = 0;
    buf[11] = 0; // SSRC (テストでは任意値でよい)
}

TEST_CASE("h264: 単一NALパケット(非IDR)", "[h264_depacket]") {
    uint8_t au_buf[256];
    h264_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    h264_depacket_t d;
    h264_depacket_init(&d, au_buf, sizeof(au_buf), h264_cb, &cap);

    uint8_t nal[] = {0x61, 0xDE, 0xAD, 0xBE, 0xEF}; // type=1 (非IDR slice)
    uint8_t rtp[12 + sizeof(nal)];
    build_rtp_header(rtp, true, 96, 1, 1000);
    memcpy(rtp + 12, nal, sizeof(nal));

    h264_depacket_feed_rtp(&d, rtp, sizeof(rtp));

    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_FALSE(cap.last_keyframe);
    uint8_t expected[] = {0, 0, 0, 1, 0x61, 0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL(sizeof(expected), cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cap.last_au, sizeof(expected));
}

TEST_CASE("h264: STAP-A (SPS+PPS+IDR)", "[h264_depacket]") {
    uint8_t au_buf[256];
    h264_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    h264_depacket_t d;
    h264_depacket_init(&d, au_buf, sizeof(au_buf), h264_cb, &cap);

    uint8_t sps[] = {0x67, 0x01, 0x02};       // type=7
    uint8_t pps[] = {0x68, 0x03};             // type=8
    uint8_t idr[] = {0x65, 0xAA, 0xBB, 0xCC}; // type=5

    uint8_t stap[1 + 2 + sizeof(sps) + 2 + sizeof(pps) + 2 + sizeof(idr)];
    size_t p = 0;
    stap[p++] = 0x78; // STAP-A header (type=24)
    stap[p++] = 0x00;
    stap[p++] = (uint8_t)sizeof(sps);
    memcpy(&stap[p], sps, sizeof(sps));
    p += sizeof(sps);
    stap[p++] = 0x00;
    stap[p++] = (uint8_t)sizeof(pps);
    memcpy(&stap[p], pps, sizeof(pps));
    p += sizeof(pps);
    stap[p++] = 0x00;
    stap[p++] = (uint8_t)sizeof(idr);
    memcpy(&stap[p], idr, sizeof(idr));
    p += sizeof(idr);

    uint8_t rtp[12 + sizeof(stap)];
    build_rtp_header(rtp, true, 96, 10, 2000);
    memcpy(rtp + 12, stap, sizeof(stap));

    h264_depacket_feed_rtp(&d, rtp, sizeof(rtp));

    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_TRUE(cap.last_keyframe);

    uint8_t expected[] = {
        0, 0, 0, 1, 0x67, 0x01, 0x02, 0, 0, 0, 1, 0x68, 0x03, 0, 0, 0, 1, 0x65, 0xAA, 0xBB, 0xCC,
    };
    TEST_ASSERT_EQUAL(sizeof(expected), cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cap.last_au, sizeof(expected));
}

TEST_CASE("h264: FU-Aで3パケットに分割されたNALを再構成", "[h264_depacket]") {
    uint8_t au_buf[256];
    h264_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    h264_depacket_t d;
    h264_depacket_init(&d, au_buf, sizeof(au_buf), h264_cb, &cap);

    // FU indicator=0x7C (NRI=3,type=28), FU header: S=1(0x85)/中間(0x05)/E=1(0x45)、元type=5(IDR)
    uint8_t p1[] = {0x7C, 0x85, 0x11, 0x22};
    uint8_t p2[] = {0x7C, 0x05, 0x33, 0x44};
    uint8_t p3[] = {0x7C, 0x45, 0x55, 0x66};

    uint8_t rtp1[12 + sizeof(p1)];
    build_rtp_header(rtp1, false, 96, 20, 3000);
    memcpy(rtp1 + 12, p1, sizeof(p1));
    h264_depacket_feed_rtp(&d, rtp1, sizeof(rtp1));
    TEST_ASSERT_EQUAL(0, cap.call_count); // marker無し、まだAUは完成しない

    uint8_t rtp2[12 + sizeof(p2)];
    build_rtp_header(rtp2, false, 96, 21, 3000);
    memcpy(rtp2 + 12, p2, sizeof(p2));
    h264_depacket_feed_rtp(&d, rtp2, sizeof(rtp2));
    TEST_ASSERT_EQUAL(0, cap.call_count);

    uint8_t rtp3[12 + sizeof(p3)];
    build_rtp_header(rtp3, true, 96, 22, 3000); // marker=1 → AU境界
    memcpy(rtp3 + 12, p3, sizeof(p3));
    h264_depacket_feed_rtp(&d, rtp3, sizeof(rtp3));

    TEST_ASSERT_EQUAL(1, cap.call_count);
    TEST_ASSERT_TRUE(cap.last_keyframe);
    uint8_t expected[] = {0, 0, 0, 1, 0x65, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    TEST_ASSERT_EQUAL(sizeof(expected), cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cap.last_au, sizeof(expected));
}

TEST_CASE("h264: seqギャップ検出→破棄→次のIDRで復帰", "[h264_depacket]") {
    uint8_t au_buf[256];
    h264_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    h264_depacket_t d;
    h264_depacket_init(&d, au_buf, sizeof(au_buf), h264_cb, &cap);

    // seq=1: 通常フレーム (非IDR)
    uint8_t nal1[] = {0x61, 0x01};
    uint8_t rtp1[12 + sizeof(nal1)];
    build_rtp_header(rtp1, true, 96, 1, 100);
    memcpy(rtp1 + 12, nal1, sizeof(nal1));
    h264_depacket_feed_rtp(&d, rtp1, sizeof(rtp1));
    TEST_ASSERT_EQUAL(1, cap.call_count);

    // seq=5 (本来2であるべきがギャップ)、非IDR → 破棄されコールバックされない
    uint8_t nal2[] = {0x61, 0x02};
    uint8_t rtp2[12 + sizeof(nal2)];
    build_rtp_header(rtp2, true, 96, 5, 200);
    memcpy(rtp2 + 12, nal2, sizeof(nal2));
    h264_depacket_feed_rtp(&d, rtp2, sizeof(rtp2));
    TEST_ASSERT_EQUAL(1, cap.call_count); // 増えない

    // seq=6、IDR → 復帰してコールバックされる
    uint8_t nal3[] = {0x65, 0x03};
    uint8_t rtp3[12 + sizeof(nal3)];
    build_rtp_header(rtp3, true, 96, 6, 300);
    memcpy(rtp3 + 12, nal3, sizeof(nal3));
    h264_depacket_feed_rtp(&d, rtp3, sizeof(rtp3));

    TEST_ASSERT_EQUAL(2, cap.call_count);
    TEST_ASSERT_TRUE(cap.last_keyframe);
    uint8_t expected[] = {0, 0, 0, 1, 0x65, 0x03};
    TEST_ASSERT_EQUAL(sizeof(expected), cap.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cap.last_au, sizeof(expected));
}
