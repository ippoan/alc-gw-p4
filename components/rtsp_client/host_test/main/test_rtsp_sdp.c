// rtsp_sdp の host テスト。SDP サンプルは実カメラの DESCRIBE 応答を模した
// 合成データ (このリポジトリに実機ログの生ログは含めていないため)。
// sprop-parameter-sets の base64 値は、SPS=0x00..0x0F の16バイト、
// PPS=0xF0F1F2F3 の4バイトを事前に base64 エンコードしたもの
// (PowerShell の [Convert]::ToBase64String で生成・確認済み)。
#include <string.h>

#include "unity.h"
#include "rtsp_sdp.h"

static const uint8_t EXPECTED_SPS[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t EXPECTED_PPS[4] = {0xF0, 0xF1, 0xF2, 0xF3};

TEST_CASE("sdp: audio+video, sprop-parameter-setsあり", "[rtsp_sdp]") {
    const char *sdp =
        "v=0\r\n"
        "o=- 1234567890 1 IN IP4 192.0.2.10\r\n"
        "s=IPCamera Video Stream\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "a=range:npt=0-\r\n"
        "m=audio 0 RTP/AVP 0\r\n"
        "a=control:track2\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=4d0028;"
        "sprop-parameter-sets=AAECAwQFBgcICQoLDA0ODw==,8PHy8w==\r\n"
        "a=control:track1\r\n";

    rtsp_sdp_video_t out;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_sdp_parse(sdp, strlen(sdp), &out));
    TEST_ASSERT_EQUAL(96, out.payload_type);
    TEST_ASSERT_TRUE(out.has_control);
    TEST_ASSERT_EQUAL_STRING("track1", out.control);
    TEST_ASSERT_TRUE(out.has_sprop);
    TEST_ASSERT_EQUAL(sizeof(EXPECTED_SPS), out.sps_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(EXPECTED_SPS, out.sps, sizeof(EXPECTED_SPS));
    TEST_ASSERT_EQUAL(sizeof(EXPECTED_PPS), out.pps_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(EXPECTED_PPS, out.pps, sizeof(EXPECTED_PPS));
}

TEST_CASE("sdp: sprop-parameter-sets無し (インバンドSPS/PPS前提)", "[rtsp_sdp]") {
    const char *sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 0.0.0.0\r\n"
        "s=Stream\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "a=fmtp:97 packetization-mode=1;profile-level-id=640028\r\n"
        "a=control:track1\r\n";

    rtsp_sdp_video_t out;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_sdp_parse(sdp, strlen(sdp), &out));
    TEST_ASSERT_EQUAL(97, out.payload_type);
    TEST_ASSERT_TRUE(out.has_control);
    TEST_ASSERT_EQUAL_STRING("track1", out.control);
    TEST_ASSERT_FALSE(out.has_sprop);
    TEST_ASSERT_EQUAL(0, out.sps_len);
}

TEST_CASE("sdp: 音声のみ (H264映像なし) はESP_ERR_NOT_SUPPORTED", "[rtsp_sdp]") {
    const char *sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 0.0.0.0\r\n"
        "s=AudioOnly\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n";

    rtsp_sdp_video_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, rtsp_sdp_parse(sdp, strlen(sdp), &out));
}

TEST_CASE("sdp: a=fmtp/a=controlがa=rtpmapより前でも解析できる(行順不同)", "[rtsp_sdp]") {
    const char *sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 0.0.0.0\r\n"
        "s=Stream\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=control:track1\r\n"
        "a=fmtp:96 sprop-parameter-sets=AAECAwQFBgcICQoLDA0ODw==,8PHy8w==\r\n"
        "a=rtpmap:96 H264/90000\r\n";

    rtsp_sdp_video_t out;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_sdp_parse(sdp, strlen(sdp), &out));
    TEST_ASSERT_EQUAL(96, out.payload_type);
    TEST_ASSERT_TRUE(out.has_sprop);
    TEST_ASSERT_EQUAL(sizeof(EXPECTED_SPS), out.sps_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(EXPECTED_SPS, out.sps, sizeof(EXPECTED_SPS));
}

TEST_CASE("sdp: \\nのみの改行でも解析できる", "[rtsp_sdp]") {
    const char *sdp =
        "v=0\n"
        "m=video 0 RTP/AVP 96\n"
        "a=rtpmap:96 H264/90000\n"
        "a=control:track1\n";

    rtsp_sdp_video_t out;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_sdp_parse(sdp, strlen(sdp), &out));
    TEST_ASSERT_EQUAL(96, out.payload_type);
    TEST_ASSERT_EQUAL_STRING("track1", out.control);
}
