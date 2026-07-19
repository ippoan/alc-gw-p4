// rtsp_client の host テスト。実ネットワークの代わりに「台本」駆動のモックioを
// 注入し、OPTIONS→DESCRIBE(401→再送)→SETUP→PLAY の状態遷移を検証する。
// DESCRIBE再送時のAuthorizationヘッダの response 値は、PowerShellの
// System.Security.Cryptography.MD5 で事前に計算し確認済み
// (username=testuser, realm=rtsp-test-realm, password=testpass123,
//  nonce=0123456789abcdef0123456789abcdef, method=DESCRIBE,
//  uri=rtsp://192.0.2.1:554/stream2 → response=540db9bd7670c5230352ae76fe0b597f)。
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "unity.h"
#include "rtsp_client.h"

typedef struct {
    const char *expect_contains; // 送信リクエストがこの部分文字列を含むこと
    const char *response;        // このステップでrecv()から返す応答バイト列
} mock_step_t;

typedef struct {
    const mock_step_t *steps;
    size_t step_count;
    size_t cur_step;
    size_t response_pos;
    bool mismatch; // 送信内容が期待と食い違ったら立つ
    char last_sent[1024];
} mock_io_ctx_t;

static int mock_connect(void *io_ctx, const char *host, uint16_t port, uint32_t timeout_ms) {
    (void)host;
    (void)port;
    (void)timeout_ms;
    mock_io_ctx_t *m = (mock_io_ctx_t *)io_ctx;
    m->cur_step = 0;
    m->response_pos = 0;
    m->mismatch = false;
    return 0;
}

static int mock_send(void *io_ctx, const uint8_t *buf, size_t len) {
    mock_io_ctx_t *m = (mock_io_ctx_t *)io_ctx;
    if (m->cur_step >= m->step_count) {
        // 台本を意図的に途中で終わらせているテスト (末尾のリクエスト内容までは
        // 検証しない) 用。ここでは mismatch を立てず、単に送信失敗として扱う
        return -1;
    }
    const mock_step_t *step = &m->steps[m->cur_step];
    size_t n = len < sizeof(m->last_sent) - 1 ? len : sizeof(m->last_sent) - 1;
    memcpy(m->last_sent, buf, n);
    m->last_sent[n] = '\0';
    if (strstr(m->last_sent, step->expect_contains) == NULL) {
        printf("mock mismatch: expected substring not found\n  expect: %s\n  actual: %s\n",
               step->expect_contains, m->last_sent);
        m->mismatch = true;
        return -1;
    }
    return (int)len;
}

static int mock_recv(void *io_ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms) {
    (void)timeout_ms;
    mock_io_ctx_t *m = (mock_io_ctx_t *)io_ctx;
    if (m->cur_step >= m->step_count) {
        return -1;
    }
    const mock_step_t *step = &m->steps[m->cur_step];
    size_t resp_len = strlen(step->response);
    size_t remain = resp_len - m->response_pos;
    size_t n = remain < cap ? remain : cap;
    memcpy(buf, step->response + m->response_pos, n);
    m->response_pos += n;
    if (m->response_pos == resp_len) {
        m->cur_step++;
        m->response_pos = 0;
    }
    return (int)n;
}

static void mock_close(void *io_ctx) {
    (void)io_ctx;
}

static const rtsp_io_ops_t mock_ops = {
    .connect = mock_connect,
    .send = mock_send,
    .recv = mock_recv,
    .close = mock_close,
};

TEST_CASE("rtsp_client: OPTIONS->DESCRIBE(401->再送)->SETUP->PLAY 正常系", "[rtsp_client]") {
    char sdp_body[256];
    int sdp_len = snprintf(sdp_body, sizeof(sdp_body),
                            "v=0\r\n"
                            "o=- 1 1 IN IP4 0.0.0.0\r\n"
                            "s=Stream\r\n"
                            "t=0 0\r\n"
                            "m=video 0 RTP/AVP 96\r\n"
                            "a=rtpmap:96 H264/90000\r\n"
                            "a=control:track1\r\n");
    TEST_ASSERT_TRUE(sdp_len > 0 && (size_t)sdp_len < sizeof(sdp_body));

    char describe_200[384];
    int n = snprintf(describe_200, sizeof(describe_200),
                      "RTSP/1.0 200 OK\r\nCSeq: 3\r\nContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
                      sdp_len, sdp_body);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(describe_200));

    const mock_step_t steps[] = {
        {
            .expect_contains = "OPTIONS rtsp://192.0.2.1:554/stream2 RTSP/1.0",
            .response = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n",
        },
        {
            .expect_contains = "DESCRIBE rtsp://192.0.2.1:554/stream2 RTSP/1.0",
            .response = "RTSP/1.0 401 Unauthorized\r\nCSeq: 2\r\n"
                        "WWW-Authenticate: Digest realm=\"rtsp-test-realm\", "
                        "nonce=\"0123456789abcdef0123456789abcdef\"\r\n\r\n",
        },
        {
            .expect_contains = "response=\"540db9bd7670c5230352ae76fe0b597f\"",
            .response = describe_200,
        },
        {
            .expect_contains = "SETUP rtsp://192.0.2.1:554/stream2/track1 RTSP/1.0",
            .response = "RTSP/1.0 200 OK\r\nCSeq: 4\r\nSession: 12345678;timeout=60\r\n"
                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n",
        },
        {
            .expect_contains = "PLAY rtsp://192.0.2.1:554/stream2 RTSP/1.0",
            .response = "RTSP/1.0 200 OK\r\nCSeq: 5\r\nSession: 12345678\r\n\r\n",
        },
    };

    mock_io_ctx_t mock;
    memset(&mock, 0, sizeof(mock));
    mock.steps = steps;
    mock.step_count = sizeof(steps) / sizeof(steps[0]);

    rtsp_client_config_t cfg = {
        .url = "rtsp://192.0.2.1:554/stream2",
        .username = "testuser",
        .password = "testpass123",
        .io = &mock_ops,
        .io_ctx = &mock,
        .response_timeout_ms = 1000,
    };

    rtsp_client_t *c = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_client_open(&cfg, &c));
    TEST_ASSERT_NOT_NULL(c);

    rtsp_sdp_video_t video;
    esp_err_t err = rtsp_client_play(c, &video);
    TEST_ASSERT_FALSE(mock.mismatch);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(96, video.payload_type);
    TEST_ASSERT_EQUAL_STRING("track1", video.control);

    rtsp_client_close(c);
}

TEST_CASE("rtsp_client: DESCRIBEが2回目も401なら即失敗(無限再送しない)", "[rtsp_client]") {
    const mock_step_t steps[] = {
        {
            .expect_contains = "OPTIONS rtsp://192.0.2.1:554/stream2 RTSP/1.0",
            .response = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n",
        },
        {
            .expect_contains = "DESCRIBE rtsp://192.0.2.1:554/stream2 RTSP/1.0",
            .response = "RTSP/1.0 401 Unauthorized\r\nCSeq: 2\r\n"
                        "WWW-Authenticate: Digest realm=\"rtsp-test-realm\", "
                        "nonce=\"0123456789abcdef0123456789abcdef\"\r\n\r\n",
        },
        {
            .expect_contains = "Authorization: Digest username=\"testuser\"",
            .response = "RTSP/1.0 401 Unauthorized\r\nCSeq: 3\r\n"
                        "WWW-Authenticate: Digest realm=\"rtsp-test-realm\", "
                        "nonce=\"0123456789abcdef0123456789abcdef\"\r\n\r\n",
        },
    };

    mock_io_ctx_t mock;
    memset(&mock, 0, sizeof(mock));
    mock.steps = steps;
    mock.step_count = sizeof(steps) / sizeof(steps[0]);

    rtsp_client_config_t cfg = {
        .url = "rtsp://192.0.2.1:554/stream2",
        .username = "testuser",
        .password = "wrongpassword",
        .io = &mock_ops,
        .io_ctx = &mock,
        .response_timeout_ms = 1000,
    };

    rtsp_client_t *c = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_client_open(&cfg, &c));

    rtsp_sdp_video_t video;
    esp_err_t err = rtsp_client_play(c, &video);
    TEST_ASSERT_FALSE(mock.mismatch);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);
    // 3ステップ (OPTIONS, DESCRIBE 1回目, DESCRIBE 2回目) しか消費していない
    // = 3回目の401の後にさらに再送していないことの確認
    TEST_ASSERT_EQUAL(3, mock.cur_step);

    rtsp_client_close(c);
}

TEST_CASE("rtsp_client: H264映像が無いSDPはESP_ERR_NOT_SUPPORTED", "[rtsp_client]") {
    char sdp_body[128];
    int sdp_len = snprintf(sdp_body, sizeof(sdp_body),
                            "v=0\r\n"
                            "m=audio 0 RTP/AVP 0\r\n"
                            "a=rtpmap:0 PCMU/8000\r\n");
    TEST_ASSERT_TRUE(sdp_len > 0 && (size_t)sdp_len < sizeof(sdp_body));

    char describe_200[256];
    int n = snprintf(describe_200, sizeof(describe_200), "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: %d\r\n\r\n%s",
                      sdp_len, sdp_body);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(describe_200));

    const mock_step_t steps[] = {
        {
            .expect_contains = "OPTIONS rtsp://192.0.2.1:554/stream2 RTSP/1.0",
            .response = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n",
        },
        {
            .expect_contains = "DESCRIBE rtsp://192.0.2.1:554/stream2 RTSP/1.0",
            .response = describe_200,
        },
    };

    mock_io_ctx_t mock;
    memset(&mock, 0, sizeof(mock));
    mock.steps = steps;
    mock.step_count = sizeof(steps) / sizeof(steps[0]);

    rtsp_client_config_t cfg = {
        .url = "rtsp://192.0.2.1:554/stream2",
        .io = &mock_ops,
        .io_ctx = &mock,
        .response_timeout_ms = 1000,
    };

    rtsp_client_t *c = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_client_open(&cfg, &c));

    rtsp_sdp_video_t video;
    esp_err_t err = rtsp_client_play(c, &video);
    TEST_ASSERT_FALSE(mock.mismatch);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, err);

    rtsp_client_close(c);
}

TEST_CASE("rtsp_client: rtsp URLの解析 (デフォルトポート554)", "[rtsp_client]") {
    const mock_step_t steps[] = {
        {
            .expect_contains = "OPTIONS rtsp://192.0.2.5:554/cam/stream1 RTSP/1.0",
            .response = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n",
        },
    };
    mock_io_ctx_t mock;
    memset(&mock, 0, sizeof(mock));
    mock.steps = steps;
    mock.step_count = sizeof(steps) / sizeof(steps[0]);

    rtsp_client_config_t cfg = {
        .url = "rtsp://192.0.2.5/cam/stream1", // ポート省略 → 554
        .io = &mock_ops,
        .io_ctx = &mock,
        .response_timeout_ms = 1000,
    };
    rtsp_client_t *c = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_client_open(&cfg, &c));

    rtsp_sdp_video_t video;
    // OPTIONS成功直後、DESCRIBE用の応答が無いのでタイムアウトするが、
    // URLがOPTIONSのリクエスト行に正しく展開されたことだけを確認したい
    rtsp_client_play(c, &video);
    TEST_ASSERT_FALSE(mock.mismatch);

    rtsp_client_close(c);
}
