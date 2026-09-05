/*
 * Host-side tests for the v2 authenticated runtime macro protocol.
 *
 * The production authentication, slot, and protocol sources are included
 * directly so the state-machine and wire contract can be exercised without a
 * USB controller.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/runtime_macro_auth.h>

#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_RUNTIME_MACRO_USB_HID 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_TEST 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT 30
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT 300
#define CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT 4
#define CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN 64

#define ZMK_RUNTIME_MACRO_AUTH_TEST 1

int64_t host_uptime;

static int random_result;
static uint8_t random_bytes[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
static unsigned int random_calls;
static int hmac_result;
static uint8_t hmac_output[32];
static unsigned int hmac_calls;
static bool hmac_output_from_key;

int64_t runtime_macro_auth_test_now_ms(void) { return host_uptime; }

int runtime_macro_auth_test_random(void *destination, size_t length) {
  random_calls++;
  if (random_result != 0) {
    return random_result;
  }
  memcpy(destination, random_bytes, length);
  return 0;
}

int runtime_macro_auth_test_hmac(const uint8_t *key, const uint8_t *input,
                                 size_t input_length, uint8_t *output) {
  (void)key;
  (void)input;
  (void)input_length;
  hmac_calls++;
  if (hmac_result != 0) {
    return hmac_result;
  }
  if (hmac_output_from_key) {
    memset(output, key[0], 32U);
  } else {
    memcpy(output, hmac_output, sizeof(hmac_output));
  }
  return 0;
}

static int save_result;
static int delete_result;
static unsigned int save_calls;
static unsigned int delete_calls;
static char saved_name[64];
static uint8_t saved_value[64];
static size_t saved_length;

int settings_save_one(const char *name, const void *value, size_t length) {
  save_calls++;
  snprintf(saved_name, sizeof(saved_name), "%s", name);
  saved_length = length;
  if (length <= sizeof(saved_value)) {
    memcpy(saved_value, value, length);
  }
  return save_result;
}

int settings_delete(const char *name) {
  delete_calls++;
  snprintf(saved_name, sizeof(saved_name), "%s", name);
  return delete_result;
}

#include "../../src/runtime_macro_auth.c"
#include "../../src/runtime_macro.c"
#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro_protocol.c"

struct settings_reader {
  const uint8_t *data;
  size_t length;
  int error;
};

static ssize_t read_settings(void *arg, void *data, size_t length) {
  struct settings_reader *reader = arg;
  if (reader->error != 0) {
    return reader->error;
  }
  size_t copy_length = reader->length < length ? reader->length : length;
  memcpy(data, reader->data, copy_length);
  return (ssize_t)copy_length;
}

static int failures;

#define EXPECT_TRUE(condition)                                                 \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
      failures++;                                                              \
    }                                                                          \
  } while (false)

#define EXPECT_EQ(expected, actual)                                            \
  do {                                                                         \
    long long expected_value = (long long)(expected);                          \
    long long actual_value = (long long)(actual);                              \
    if (expected_value != actual_value) {                                      \
      fprintf(stderr, "FAIL: %s:%d: expected %lld, got %lld\n", __FILE__,    \
              __LINE__, expected_value, actual_value);                         \
      failures++;                                                              \
    }                                                                          \
  } while (false)

static void reset_backend(void) {
  save_result = 0;
  delete_result = 0;
  save_calls = 0;
  delete_calls = 0;
  saved_name[0] = '\0';
  memset(saved_value, 0, sizeof(saved_value));
  saved_length = 0;
}

static void reset_auth_seams(void) {
  random_result = 0;
  memset(random_bytes, 0xa0, sizeof(random_bytes));
  random_calls = 0;
  hmac_result = 0;
  memset(hmac_output, 0x5a, sizeof(hmac_output));
  hmac_calls = 0;
  hmac_output_from_key = false;
}

static void reset_slots(void) {
  runtime_macro_auth_test_reset();
  host_uptime = 0;
  reset_backend();
  reset_auth_seams();
  for (uint8_t slot = 0; slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT; slot++) {
    (void)zmk_runtime_macro_slot_clear(slot);
  }
  reset_backend();
}

static void frame_put_u16(uint8_t *frame, size_t offset, uint16_t value) {
  frame[offset] = (uint8_t)value;
  frame[offset + 1U] = (uint8_t)(value >> 8);
}

static uint16_t frame_get_u16(const uint8_t *frame, size_t offset) {
  return (uint16_t)frame[offset] | ((uint16_t)frame[offset + 1U] << 8);
}

static uint32_t frame_get_u32(const uint8_t *frame, size_t offset) {
  return (uint32_t)frame[offset] | ((uint32_t)frame[offset + 1U] << 8) |
         ((uint32_t)frame[offset + 2U] << 16) |
         ((uint32_t)frame[offset + 3U] << 24);
}

static void make_request(uint8_t *request, uint8_t version, uint8_t opcode,
                         uint8_t request_id, uint8_t status, uint8_t slot,
                         uint16_t offset, uint16_t total_length,
                         uint8_t payload_length, const void *payload) {
  memset(request, 0, ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE);
  request[0] = version;
  request[1] = opcode;
  request[2] = request_id;
  request[3] = status;
  request[4] = slot;
  request[5] = payload_length;
  frame_put_u16(request, 6, offset);
  frame_put_u16(request, 8, total_length);
  if (payload != NULL && payload_length <= 22U) {
    memcpy(request + 10, payload, payload_length);
  }
}

static void process_request(struct zmk_runtime_macro_protocol *protocol,
                            const uint8_t *request, uint8_t *response) {
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(protocol, request, response));
}

static void expect_error(const uint8_t *response, uint8_t version,
                         uint8_t opcode, uint8_t request_id, uint8_t slot,
                         enum zmk_runtime_macro_protocol_status status) {
  EXPECT_EQ(version, response[0]);
  EXPECT_EQ(opcode, response[1]);
  EXPECT_EQ(request_id, response[2]);
  EXPECT_EQ(status, response[3]);
  EXPECT_EQ(slot, response[4]);
  EXPECT_EQ(0, response[5]);
  EXPECT_EQ(0, frame_get_u16(response, 6));
  EXPECT_EQ(0, frame_get_u16(response, 8));
  for (size_t i = 10; i < 32; i++) {
    EXPECT_EQ(0, response[i]);
  }
}

static void expect_ok(const uint8_t *response, uint8_t opcode,
                      uint8_t request_id, uint8_t slot, uint16_t offset,
                      uint16_t total_length, const void *payload,
                      uint8_t payload_length) {
  EXPECT_EQ(2, response[0]);
  EXPECT_EQ(opcode, response[1]);
  EXPECT_EQ(request_id, response[2]);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);
  EXPECT_EQ(slot, response[4]);
  EXPECT_EQ(payload_length, response[5]);
  EXPECT_EQ(offset, frame_get_u16(response, 6));
  EXPECT_EQ(total_length, frame_get_u16(response, 8));
  if (payload_length > 0U && payload != NULL) {
    EXPECT_TRUE(memcmp(response + 10, payload, payload_length) == 0);
  }
  for (size_t i = 10U + payload_length; i < 32; i++) {
    EXPECT_EQ(0, response[i]);
  }
}

static struct zmk_runtime_macro_auth_credential make_credential(uint8_t marker) {
  struct zmk_runtime_macro_auth_credential credential = {
      .iterations = ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT,
  };
  memset(credential.salt, marker, sizeof(credential.salt));
  memset(credential.key, (int)(marker + 1U), sizeof(credential.key));
  return credential;
}

static void set_credential_direct(uint8_t marker) {
  struct zmk_runtime_macro_auth_credential credential = make_credential(marker);
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
}

static void authenticate(void) {
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  memcpy(proof, hmac_output, sizeof(proof));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
}

static void wire_authenticate(struct zmk_runtime_macro_protocol *protocol,
                              uint8_t *request, uint8_t *response,
                              uint8_t challenge_request_id,
                              uint8_t prove_request_id, uint8_t proof_value) {
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  memset(proof, proof_value, sizeof(proof));

  make_request(request, 2,
               ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               challenge_request_id, 0, 0xff, 0, 0, 0, NULL);
  process_request(protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
            challenge_request_id, 0xff, 0, ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE,
            NULL, ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE,
               prove_request_id, 0, 0xff, 0,
               ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE,
               ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE, proof);
  process_request(protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE,
            prove_request_id, 0xff, 0, 0, NULL, 0);
}

static void make_password_object(uint8_t *object, uint32_t iterations,
                                 uint8_t salt_value, uint8_t key_value) {
  memset(object, 0, 52U);
  object[0] = (uint8_t)iterations;
  object[1] = (uint8_t)(iterations >> 8);
  object[2] = (uint8_t)(iterations >> 16);
  object[3] = (uint8_t)(iterations >> 24);
  memset(object + 4, salt_value, ZMK_RUNTIME_MACRO_AUTH_SALT_SIZE);
  memset(object + 20, key_value, ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE);
}

static void send_password_object(
    struct zmk_runtime_macro_protocol *protocol, uint8_t request_id,
    const uint8_t *object, uint8_t *response) {
  uint8_t request[32];
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET,
               request_id, 0, 0xff, 0, 52, 22, object);
  process_request(protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET,
            request_id, 0xff, 22, 52, NULL, 0);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET,
               request_id, 0, 0xff, 22, 52, 22, object + 22);
  process_request(protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET,
            request_id, 0xff, 44, 52, NULL, 0);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET,
               request_id, 0, 0xff, 44, 52, 8, object + 44);
  process_request(protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET,
            request_id, 0xff, 52, 52, NULL, 0);
}

static void test_wire_constants_and_open_commands(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t info_payload[22];

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  EXPECT_EQ(2, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION);
  EXPECT_EQ(52, ZMK_RUNTIME_MACRO_PROTOCOL_PASSWORD_SET_LENGTH);
  EXPECT_EQ(0x10, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO);
  EXPECT_EQ(0x11, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE);
  EXPECT_EQ(0x12, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE);
  EXPECT_EQ(0x13, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET);
  EXPECT_EQ(0x14, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK);
  EXPECT_EQ(10, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);
  EXPECT_EQ(11, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_FAILED);
  EXPECT_EQ(12, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NOT_CONFIGURED);
  EXPECT_EQ(13, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_RATE_LIMITED);
  EXPECT_EQ(14, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NO_CHALLENGE);
  EXPECT_EQ(15, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 1, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 1, 0xff,
            0, 22, NULL, 22);
  memcpy(info_payload, response + 10, sizeof(info_payload));
  EXPECT_EQ(0, info_payload[0]);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_KDF_ID, info_payload[1]);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT,
            frame_get_u32(info_payload, 2));
  for (size_t i = 6; i < sizeof(info_payload); i++) {
    EXPECT_EQ(0, info_payload[i]);
  }

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 2, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 3, 0, 0,
               0, 2, 2, "ok");
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 3, 0, 2, 2,
            NULL, 0);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 4, 0, 0, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 4, 0, 0, 2,
            "ok", 2);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 5, 0, 0,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 5, 0, 0, 0,
            NULL, 0);

  /* v1 cannot be used as an unauthenticated compatibility escape hatch. */
  make_request(request, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 6, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 6, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
}

static void test_base_order_and_protected_gate(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];

  reset_slots();
  set_credential_direct(0x21);
  zmk_runtime_macro_protocol_init(&protocol);

  /* Semantic errors are hidden behind AUTH_REQUIRED. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 10, 0, 99,
               999, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 10, 99,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 11, 0, 99,
               999, 64, 1, "x");
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 11, 99,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 12, 0, 99,
               1, 1, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 12, 99,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 13, 0, 0,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 13, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);

  /* Basic frame errors precede the auth gate. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 14, 1, 99,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 14, 99,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 15, 0, 99,
               0, 0, 23, NULL);
  request[31] = 0;
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 15, 99,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 17, 0, 99,
               0, 0, 0, NULL);
  request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET] = 1U;
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 17, 99,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  make_request(request, 2, 0x99, 16, 1, 99, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, 0x99, 16, 99,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OPCODE);
}

static void test_challenge_error_mapping(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];

  reset_slots();
  set_credential_direct(0x25);
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               20, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 20,
            0xff, 0, ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE, NULL,
            ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE);
  EXPECT_TRUE(memcmp(response + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
                     random_bytes, sizeof(random_bytes)) == 0);

  /* A challenge that expires cannot be proved and returns an empty error. */
  host_uptime += CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT * 1000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 21,
               0, 0xff, 0, ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE,
               ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE, hmac_output);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 21,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NO_CHALLENGE);

  reset_slots();
  set_credential_direct(0x35);
  zmk_runtime_macro_protocol_init(&protocol);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               22, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  memset(proof, 0, sizeof(proof));
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 23,
               0, 0xff, 0, ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE,
               ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE, proof);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 23,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_FAILED);

  /* A failed proof starts cooldown; challenge is then rate limited. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               24, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               24, 0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_RATE_LIMITED);

  reset_slots();
  set_credential_direct(0x45);
  zmk_runtime_macro_protocol_init(&protocol);
  random_result = -EIO;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               25, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               25, 0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL);
}

static void test_auth_info_does_not_refresh_session(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];

  reset_slots();
  set_credential_direct(0x2a);
  zmk_runtime_macro_protocol_init(&protocol);
  authenticate();

  host_uptime = CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT * 1000 - 1;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 26,
               0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 26, 0xff,
            0, 22, NULL, 22);
  EXPECT_EQ(3, response[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET]);

  host_uptime += 2;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 27, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 27, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);
}

static void test_auth_info_challenge_proof_and_session(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t proof[16];

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 20,
               0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               20, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NOT_CONFIGURED);

  set_credential_direct(0x31);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 21, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 21, 0xff, 0,
            22, NULL, 22);
  EXPECT_EQ(1, response[10] & 1U);
  EXPECT_EQ(0, response[10] & 2U);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT,
            frame_get_u32(response + 10, 2));
  struct zmk_runtime_macro_auth_credential credential = make_credential(0x31);
  EXPECT_TRUE(memcmp(response + 16, credential.salt, 16) == 0);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 22,
               0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 22,
            0xff, 0, 16, NULL, 16);

  memcpy(proof, hmac_output, sizeof(proof));
  proof[0] ^= 1U;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 23, 0,
               0xff, 0, 16, 16, proof);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 23,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_FAILED);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 24, 0,
               0xff, 0, 16, 16, hmac_output);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 24,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NO_CHALLENGE);

  host_uptime += 1000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 25,
               0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 25,
            0xff, 0, 16, NULL, 16);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 26, 0,
               0xff, 0, 16, 16, hmac_output);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 26, 0xff, 0,
            0, NULL, 0);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 27, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 27, 0xff, 0,
            22, NULL, 22);
  EXPECT_EQ(3, response[10]);

  /* A successful protected operation refreshes the inactivity deadline. */
  host_uptime += (CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT - 1) * 1000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 28, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);
  host_uptime += 2000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 29, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);

  host_uptime += CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT * 1000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 30, 0, 0, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 30, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);
}

static void test_password_set_open_and_boundaries(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t object[52];
  uint8_t overrun_payload[9] = {0};

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  make_password_object(object, ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, 0x42,
                       0x43);

  /* Non-canonical slot, length, and offset are rejected. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 40,
               0, 0, 0, 52, 22, object);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 40,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 41,
               0, 0xff, 0, 51, 22, object);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 41,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 42,
               0, 0xff, 1, 52, 22, object);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 42,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  /* Duplicate non-final chunks require a complete restart. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 43,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 43, 0xff,
            22, 52, NULL, 0);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 43,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 43, 0xff,
            22, 52, NULL, 0);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 43,
               0, 0xff, 44, 52, 8, object + 44);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 43,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  EXPECT_TRUE(!protocol.password_set_active);

  /* A repeated non-zero offset is not a harmless duplicate. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 46,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 46,
               0, 0xff, 22, 52, 22, object + 22);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 46, 0xff,
            44, 52, NULL, 0);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 46,
               0, 0xff, 22, 52, 22, object + 22);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 46,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  EXPECT_TRUE(!protocol.password_set_active);

  /* Request ID and total-length changes invalidate the transaction. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 47,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 48,
               0, 0xff, 22, 52, 22, object + 22);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 48,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.password_set_active);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 49,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 49,
               0, 0xff, 22, 51, 22, object + 22);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 49,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
  EXPECT_TRUE(!protocol.password_set_active);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 50,
               0, 0xff, 0, 52, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 50,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 51,
               0, 0xff, 44, 52, 9, overrun_payload);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 51,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  /* Frame padding is checked before PASSWORD_SET semantics. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 52,
               0, 0xff, 0, 52, 0, NULL);
  request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET] = 1U;
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 52,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.password_set_active);

  send_password_object(&protocol, 53, object, response);
  EXPECT_EQ(1, saved_value[0]);
  EXPECT_EQ(52, saved_length - 1U);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_VERSION, saved_value[0]);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 45, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 45, 0xff, 0,
            22, NULL, 22);
  EXPECT_EQ(1, response[10] & 1U);
  EXPECT_TRUE(memcmp(response + 16, object + 4, 16) == 0);
}

static void test_password_set_storage_and_invalid_credential(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t object[52];
  uint8_t old_salt[16];

  reset_slots();
  set_credential_direct(0x51);
  zmk_runtime_macro_protocol_init(&protocol);
  authenticate();
  make_password_object(object, ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, 0x61,
                       0x62);
  memcpy(old_salt, make_credential(0x51).salt, sizeof(old_salt));

  /* A successful non-final protected chunk refreshes the inactivity window. */
  host_uptime = (CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT - 1) * 1000;

  /* A storage failure ends only PASSWORD_SET and leaves old session usable. */
  save_result = -EIO;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 50,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 50, 0xff,
            22, 52, NULL, 0);
  host_uptime += 2000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 50,
               0, 0xff, 22, 52, 22, object + 22);
  process_request(&protocol, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 50,
               0, 0xff, 44, 52, 8, object + 44);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 50,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR);
  EXPECT_TRUE(!protocol.password_set_active);
  EXPECT_TRUE(zmk_runtime_macro_auth_is_authenticated());
  save_result = 0;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 51, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);

  /* Invalid credentials are rejected without changing the old salt. */
  memset(object + 4, 0, 16);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 52,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  /* The failed storage test left a live session, so this starts a transaction. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 52,
               0, 0xff, 22, 52, 22, object + 22);
  process_request(&protocol, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 52,
               0, 0xff, 44, 52, 8, object + 44);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 52,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
  struct zmk_runtime_macro_auth_info info;
  EXPECT_EQ(0, zmk_runtime_macro_auth_get_info(&info));
  EXPECT_TRUE(memcmp(info.salt, old_salt, sizeof(old_salt)) == 0);
}

static void test_wire_prove_discards_staging(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t object[52];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];

  reset_slots();
  set_credential_direct(0x65);
  zmk_runtime_macro_protocol_init(&protocol);
  wire_authenticate(&protocol, request, response, 20, 21, 0x5a);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 22, 0, 0,
               0, 4, 2, "ab");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);

  make_password_object(object, ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, 0x75,
                       0x76);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 23,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.password_set_active);

  /* A second login through the wire invalidates both old transactions. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               24, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 24,
            0xff, 0, ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE, NULL,
            ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE);
  memcpy(proof, hmac_output, sizeof(proof));
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 25,
               0, 0xff, 0, ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE,
               ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE, proof);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 25, 0xff,
            0, 0, NULL, 0);
  EXPECT_TRUE(!protocol.set_active);
  EXPECT_TRUE(!protocol.password_set_active);
}

static void test_lock_preserves_cooldown(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t object[52];
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t bad_proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE] = {0};

  reset_slots();
  set_credential_direct(0x77);
  zmk_runtime_macro_protocol_init(&protocol);
  authenticate();

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 30, 0, 0,
               0, 4, 2, "ab");
  process_request(&protocol, request, response);
  make_password_object(object, ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, 0x87,
                       0x88);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 31,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);
  EXPECT_TRUE(protocol.password_set_active);

  /* Establish cooldown independently; LOCK itself must not clear it. */
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_verify_proof(bad_proof,
                                                          sizeof(bad_proof)));
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK, 32, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK, 32, 0xff, 0, 0,
            NULL, 0);
  EXPECT_TRUE(!protocol.set_active);
  EXPECT_TRUE(!protocol.password_set_active);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               33, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               33, 0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_RATE_LIMITED);
}

static void test_password_set_expiry_discards_staging(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t object[52];

  reset_slots();
  set_credential_direct(0x97);
  zmk_runtime_macro_protocol_init(&protocol);
  authenticate();

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 40, 0, 0,
               0, 4, 2, "ab");
  process_request(&protocol, request, response);
  make_password_object(object, ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, 0xa7,
                       0xa8);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 41,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);
  EXPECT_TRUE(protocol.password_set_active);

  host_uptime = CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT * 1000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 41,
               0, 0xff, 22, 52, 22, object + 22);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 41,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);
  EXPECT_TRUE(!protocol.set_active);
  EXPECT_TRUE(!protocol.password_set_active);
}

static void test_password_set_success_reauth(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t object[52];
  uint8_t old_proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  uint8_t new_proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];

  reset_slots();
  hmac_output_from_key = true;
  set_credential_direct(0x91);
  zmk_runtime_macro_protocol_init(&protocol);
  wire_authenticate(&protocol, request, response, 50, 51, 0x92);

  make_password_object(object, ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, 0xa1,
                       0xb2);
  send_password_object(&protocol, 52, object, response);
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 53, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 53, 0xff,
            0, 22, NULL, 22);
  EXPECT_EQ(1, response[10] & 1U);
  EXPECT_EQ(0, response[10] & 2U);
  EXPECT_TRUE(memcmp(response + 16, object + 4, 16) == 0);

  /* The old key must fail after replacement. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               54, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 54,
            0xff, 0, ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE, NULL,
            ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE);
  memset(old_proof, 0x92, sizeof(old_proof));
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 55,
               0, 0xff, 0, ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE,
               ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE, old_proof);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 55,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_FAILED);

  /* Wait out the first failure's one-second cooldown, then use the new key. */
  host_uptime += 1000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE,
               56, 0, 0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 56,
            0xff, 0, ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE, NULL,
            ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE);
  memset(new_proof, 0xb2, sizeof(new_proof));
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 57,
               0, 0xff, 0, ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE,
               ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE, new_proof);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE, 57, 0xff,
            0, 0, NULL, 0);
  EXPECT_TRUE(zmk_runtime_macro_auth_is_authenticated());
}

static void test_staging_lock_expiry_and_transport_api(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t object[52];

  reset_slots();
  set_credential_direct(0x71);
  zmk_runtime_macro_protocol_init(&protocol);
  authenticate();
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 60, 0, 0, 0,
               4, 2, "ab");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);
  make_password_object(object, ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, 0x81,
                       0x82);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 61,
               0, 0xff, 0, 52, 22, object);
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.password_set_active);

  /* LOCK clears both transactions but does not clear auth cooldown here. */
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK, 62, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_ok(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK, 62, 0xff, 0, 0,
            NULL, 0);
  EXPECT_TRUE(!protocol.set_active);
  EXPECT_TRUE(!protocol.password_set_active);
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());

  /* Re-authentication never resumes transactions from before AUTH_PROVE. */
  authenticate();
  EXPECT_TRUE(!protocol.set_active);
  EXPECT_TRUE(!protocol.password_set_active);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 63, 0, 0, 0,
               4, 2, "cd");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);
  host_uptime += CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT * 1000;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 64, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 64, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);
  EXPECT_TRUE(!protocol.set_active);
  EXPECT_TRUE(!protocol.password_set_active);

  zmk_runtime_macro_protocol_discard(&protocol);
  EXPECT_TRUE(!protocol.auth_state_known);
}

static void test_error_locked_and_common_auth_errors(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t bad_credential[53];
  struct settings_reader reader;

  reset_slots();
  memset(bad_credential, 0, sizeof(bad_credential));
  bad_credential[0] = ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_VERSION + 1U;
  reader.data = bad_credential;
  reader.length = sizeof(bad_credential);
  reader.error = 0;
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_settings_set("auth/credential",
                                                     sizeof(bad_credential),
                                                     read_settings, &reader));
  EXPECT_EQ(0, zmk_runtime_macro_auth_settings_commit());
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 70, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO, 70,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 71, 0, 0, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 71, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 72, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 72, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 73, 0, 0,
               0, 1, 1, "x");
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 73, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 74, 0,
               0, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 74, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
  make_request(request, 2,
               ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 75, 0,
               0xff, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2,
               ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE, 75, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 76,
               0, 0xff, 0, 52, 22, bad_credential);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET, 76,
               0xff, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
}

int main(void) {
  test_wire_constants_and_open_commands();
  test_base_order_and_protected_gate();
  test_challenge_error_mapping();
  test_auth_info_does_not_refresh_session();
  test_auth_info_challenge_proof_and_session();
  test_password_set_open_and_boundaries();
  test_password_set_storage_and_invalid_credential();
  test_wire_prove_discards_staging();
  test_lock_preserves_cooldown();
  test_password_set_expiry_discards_staging();
  test_password_set_success_reauth();
  test_staging_lock_expiry_and_transport_api();
  test_error_locked_and_common_auth_errors();

  if (failures != 0) {
    fprintf(stderr, "runtime macro authenticated protocol tests: %d failure(s)\n",
            failures);
    return 1;
  }

  puts("runtime macro authenticated protocol tests: PASS");
  return 0;
}
