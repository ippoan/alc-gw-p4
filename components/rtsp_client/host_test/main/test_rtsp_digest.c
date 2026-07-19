// rtsp_digest の host テスト。期待値は以下の2系統で、いずれも実際に MD5 を
// 計算して事前に確認済み (このファイルの数値を変える場合は再計算すること):
//
//  1. RFC 2617 Section 3.5 の worked example (qop=auth)。
//     username="Mufasa", password="Circle Of Life" (スペース含む), realm/nonce/
//     opaque/cnonce は RFC の値そのまま。response="6629fae49393a05397450978507c4ef1"
//     は RFC 本文に記載の値と一致することを確認済み。
//  2. C212 の実機ログと同じ「qop 無し」形式を模した合成ベクタ (実クレデンシャル
//     はリポジトリに置かないため、ダミーのユーザー名/パスワードを使用)。
#include <string.h>
#include <stdio.h>

#include "unity.h"
#include "rtsp_digest.h"

TEST_CASE("digest: RFC2617 worked example (qop=auth)", "[rtsp_digest]") {
    rtsp_digest_challenge_t ch;
    const char *www_auth =
        "Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
        "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
        "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";

    TEST_ASSERT_EQUAL(ESP_OK, rtsp_digest_parse_challenge(www_auth, &ch));
    TEST_ASSERT_EQUAL_STRING("testrealm@host.com", ch.realm);
    TEST_ASSERT_EQUAL_STRING("dcd98b7102dd2f0e8b11d0f600bfb0c093", ch.nonce);
    TEST_ASSERT_EQUAL_STRING("auth", ch.qop);
    TEST_ASSERT_TRUE(ch.has_opaque);
    TEST_ASSERT_EQUAL_STRING("5ccc069c403ebaf9f0171e9517f40e41", ch.opaque);
    TEST_ASSERT_FALSE(ch.stale);

    char out[384];
    esp_err_t err = rtsp_digest_build_authorization(&ch, "Mufasa", "Circle Of Life",
                                                      "GET", "/dir/index.html",
                                                      "0a4f113b", out, sizeof(out));
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(out, "response=\"6629fae49393a05397450978507c4ef1\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "username=\"Mufasa\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "nc=00000001"));
    TEST_ASSERT_NOT_NULL(strstr(out, "cnonce=\"0a4f113b\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\""));
}

TEST_CASE("digest: qop無しchallenge (C212実機と同形式)", "[rtsp_digest]") {
    rtsp_digest_challenge_t ch;
    const char *www_auth =
        "Digest realm=\"rtsp-test-realm\", nonce=\"0123456789abcdef0123456789abcdef\"";

    TEST_ASSERT_EQUAL(ESP_OK, rtsp_digest_parse_challenge(www_auth, &ch));
    TEST_ASSERT_EQUAL_STRING("", ch.qop);
    TEST_ASSERT_FALSE(ch.has_opaque);

    char out[384];
    esp_err_t err = rtsp_digest_build_authorization(&ch, "testuser", "testpass123",
                                                      "DESCRIBE", "rtsp://192.0.2.1/stream2",
                                                      NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(out, "response=\"80ccdf0dce1676a4b3a1c9a77ffa7df8\""));
    TEST_ASSERT_NULL(strstr(out, "qop=")); // qop 無しリクエストには qop/nc/cnonce を含めない
}

TEST_CASE("digest: qop=authはcnonce必須", "[rtsp_digest]") {
    rtsp_digest_challenge_t ch;
    memset(&ch, 0, sizeof(ch));
    snprintf(ch.realm, sizeof(ch.realm), "rtsp-test-realm");
    snprintf(ch.nonce, sizeof(ch.nonce), "0123456789abcdef0123456789abcdef");
    snprintf(ch.qop, sizeof(ch.qop), "auth");

    char out[384];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        rtsp_digest_build_authorization(&ch, "testuser", "testpass123",
                                         "DESCRIBE", "rtsp://192.0.2.1/stream2",
                                         NULL, out, sizeof(out)));

    esp_err_t err = rtsp_digest_build_authorization(&ch, "testuser", "testpass123",
                                                      "DESCRIBE", "rtsp://192.0.2.1/stream2",
                                                      "deadbeef01234567", out, sizeof(out));
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(out, "response=\"67fa264bedfe7909ece6723d1f425eb4\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "nc=00000001"));
    TEST_ASSERT_NOT_NULL(strstr(out, "cnonce=\"deadbeef01234567\""));
}

TEST_CASE("digest: auth-intのみの提示はqop無し扱い", "[rtsp_digest]") {
    rtsp_digest_challenge_t ch;
    const char *www_auth =
        "Digest realm=\"r\", nonce=\"n\", qop=\"auth-int\"";
    TEST_ASSERT_EQUAL(ESP_OK, rtsp_digest_parse_challenge(www_auth, &ch));
    TEST_ASSERT_EQUAL_STRING("", ch.qop); // auth-int はボディハッシュが必要で非対応
}

TEST_CASE("digest: Basicスキームは非対応", "[rtsp_digest]") {
    rtsp_digest_challenge_t ch;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
        rtsp_digest_parse_challenge("Basic realm=\"foo\"", &ch));
}

TEST_CASE("digest: realm/nonce欠落はエラー", "[rtsp_digest]") {
    rtsp_digest_challenge_t ch;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        rtsp_digest_parse_challenge("Digest qop=\"auth\"", &ch));
}
