#include "http_api.h"
#include "tinytest_compat.h"
#include "webrtc_signaling.h"

void test_signaling_and_http_api_instances_have_independent_lifecycles(void) {
  webrtc_signaling_config_t signaling_config = {0};
  http_api_config_t http_config_a = {0};
  http_api_config_t http_config_b = {0};
  webrtc_signaling_server_t *signaling_a = NULL;
  webrtc_signaling_server_t *signaling_b = NULL;
  http_api_server_t *http_a = NULL;
  http_api_server_t *http_b = NULL;

  signaling_config.host = "127.0.0.1";
  signaling_a = webrtc_signaling_create(NULL, &signaling_config);
  signaling_b = webrtc_signaling_create(NULL, &signaling_config);
  TEST_ASSERT_NOT_NULL(signaling_a);
  TEST_ASSERT_NOT_NULL(signaling_b);

  http_config_a.host = "0.0.0.0";
  http_config_a.port = 18081;
  http_config_b.host = "0.0.0.0";
  http_config_b.port = 18082;
  http_a = http_api_create(NULL, &http_config_a, signaling_a);
  http_b = http_api_create(NULL, &http_config_b, signaling_b);
  TEST_ASSERT_NOT_NULL(http_a);
  TEST_ASSERT_NOT_NULL(http_b);

  TEST_ASSERT_EQUAL_INT(0, http_api_start(http_a));
  TEST_ASSERT_EQUAL_INT(0, http_api_start(http_b));
  http_api_stop(http_b);
  http_api_stop(http_a);

  http_api_destroy(http_a);
  http_api_destroy(http_b);
  webrtc_signaling_destroy(signaling_a);
  webrtc_signaling_destroy(signaling_b);
}

void test_http_api_rejects_invalid_configuration(void) {
  webrtc_signaling_config_t signaling_config = {0};
  http_api_config_t http_config = {0};
  webrtc_signaling_server_t *signaling = NULL;

  signaling = webrtc_signaling_create(NULL, &signaling_config);
  TEST_ASSERT_NOT_NULL(signaling);

  TEST_ASSERT_NULL(http_api_create(NULL, NULL, signaling));
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));
  http_config.port = 8080;
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, NULL));
  http_config.host = "127.0.0.1";
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));
  http_config.host = "0.0.0.0";
  http_config.auth_enabled = 1;
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));

  webrtc_signaling_destroy(signaling);
}

spec("test_signaling_lifecycle") {
  TT_TEST(test_signaling_and_http_api_instances_have_independent_lifecycles);
  TT_TEST(test_http_api_rejects_invalid_configuration);
}
