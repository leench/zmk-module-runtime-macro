/*
 * Host-side tests for the transport-independent runtime macro protocol.
 *
 * The production sources are included directly so protocol behavior can be
 * exercised without a USB device or a Settings backend.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>

#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_RUNTIME_MACRO_USB_HID 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_TEST 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT 30
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT 300
#define CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT 16
#define CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN 64

#define ZMK_RUNTIME_MACRO_AUTH_TEST 1

int64_t host_uptime;

int64_t runtime_macro_auth_test_now_ms(void) { return host_uptime; }

int runtime_macro_auth_test_random(void *destination, size_t length) {
  memset(destination, 0xa5, length);
  return 0;
}

int runtime_macro_auth_test_hmac(const uint8_t *key, const uint8_t *input,
                                 size_t input_length, uint8_t *output) {
  (void)key;
  (void)input;
  (void)input_length;
  memset(output, 0x5a, 32U);
  return 0;
}

#include "../../src/runtime_macro_auth.c"
#include "../../src/runtime_macro.c"
#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro_protocol.c"

_Static_assert(ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE == 32U,
               "frame size changed");
_Static_assert(ZMK_RUNTIME_MACRO_PROTOCOL_HEADER_SIZE == 10U,
               "header size changed");
_Static_assert(ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE == 22U,
               "payload size changed");
_Static_assert(ZMK_RUNTIME_MACRO_PROTOCOL_VERSION == 2U,
               "protocol version changed");
_Static_assert(ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET == 10U,
               "payload offset changed");

static int save_result;
static int delete_result;
static unsigned int save_calls;
static unsigned int delete_calls;
static char saved_name[32];
static char saved_value[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
static size_t saved_length;

static int failures;

#define EXPECT_TRUE(condition)                                                 \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
      failures++;                                                              \
    }                                                                          \
  } while (false)

#define EXPECT_EQ(expected, actual)                                            \
  do {                                                                         \
    long long expected_value = (long long)(expected);                          \
    long long actual_value = (long long)(actual);                              \
    if (expected_value != actual_value) {                                      \
      fprintf(stderr, "FAIL: %s:%d: expected %lld, got %lld\n", __FILE__,      \
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
  saved_value[0] = '\0';
  saved_length = 0;
}

int settings_save_one(const char *name, const void *value, size_t length) {
  save_calls++;
  snprintf(saved_name, sizeof(saved_name), "%s", name);
  saved_length = length;
  memcpy(saved_value, value, length);
  saved_value[length] = '\0';
  return save_result;
}

int settings_delete(const char *name) {
  delete_calls++;
  snprintf(saved_name, sizeof(saved_name), "%s", name);
  return delete_result;
}

static void reset_slots(void) {
  runtime_macro_auth_test_reset();
  host_uptime = 0;
  save_result = 0;
  delete_result = 0;
  for (uint8_t slot = 0; slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT; slot++) {
    (void)zmk_runtime_macro_slot_clear(slot);
  }
  reset_backend();
}

static void expect_slot(uint8_t slot, const char *expected,
                        size_t expected_length) {
  char snapshot[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1];
  size_t length;

  EXPECT_EQ(0, zmk_runtime_macro_slot_copy(slot, snapshot, sizeof(snapshot),
                                           &length));
  EXPECT_EQ(expected_length, length);
  EXPECT_TRUE(memcmp(snapshot, expected, expected_length) == 0);
  EXPECT_EQ(0, snapshot[expected_length]);
}

static void set_slot_text(uint8_t slot, const char *text) {
  EXPECT_EQ(0, zmk_runtime_macro_slot_set(slot, text, strlen(text)));
}

static void set_slot_bytes(uint8_t slot, const char *text, size_t length) {
  EXPECT_EQ(0, zmk_runtime_macro_slot_set(slot, text, length));
}

static void frame_put_u16(uint8_t *frame, size_t offset, uint16_t value) {
  frame[offset] = (uint8_t)value;
  frame[offset + 1U] = (uint8_t)(value >> 8);
}

static uint16_t frame_get_u16(const uint8_t *frame, size_t offset) {
  return (uint16_t)frame[offset] | ((uint16_t)frame[offset + 1U] << 8);
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
  if (payload_length > 0U && payload != NULL && payload_length <= 22U) {
    memcpy(request + 10, payload, payload_length);
  }
}

static void expect_echo(const uint8_t *response, uint8_t version,
                        uint8_t opcode, uint8_t request_id, uint8_t slot) {
  EXPECT_EQ(version, response[0]);
  EXPECT_EQ(opcode, response[1]);
  EXPECT_EQ(request_id, response[2]);
  EXPECT_EQ(slot, response[4]);
}

static void expect_error(const uint8_t *response, uint8_t version,
                         uint8_t opcode, uint8_t request_id, uint8_t slot,
                         enum zmk_runtime_macro_protocol_status status) {
  expect_echo(response, version, opcode, request_id, slot);
  EXPECT_EQ(status, response[3]);
  EXPECT_EQ(0, response[5]);
  EXPECT_EQ(0, frame_get_u16(response, 6));
  EXPECT_EQ(0, frame_get_u16(response, 8));
  for (size_t i = 10; i < ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE; i++) {
    EXPECT_EQ(0, response[i]);
  }
}

static void expect_success(const uint8_t *response, uint8_t opcode,
                           uint8_t request_id, uint8_t slot, uint16_t offset,
                           uint16_t total_length, const void *payload,
                           uint8_t payload_length) {
  expect_echo(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, opcode, request_id,
              slot);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);
  EXPECT_EQ(payload_length, response[5]);
  EXPECT_EQ(offset, frame_get_u16(response, 6));
  EXPECT_EQ(total_length, frame_get_u16(response, 8));
  if (payload_length > 0U) {
    EXPECT_TRUE(memcmp(response + 10, payload, payload_length) == 0);
  }
  for (size_t i = 10U + payload_length;
       i < ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE; i++) {
    EXPECT_EQ(0, response[i]);
  }
}

static void process_request(struct zmk_runtime_macro_protocol *protocol,
                            const uint8_t *request, uint8_t *response) {
  uint8_t request_copy[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];

  memcpy(request_copy, request, sizeof(request_copy));
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(protocol, request, response));
  EXPECT_TRUE(memcmp(request, request_copy, sizeof(request_copy)) == 0);
}

static void test_wire_constants(void) {
  EXPECT_EQ(32, ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE);
  EXPECT_EQ(10, ZMK_RUNTIME_MACRO_PROTOCOL_HEADER_SIZE);
  EXPECT_EQ(22, ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE);
  EXPECT_EQ(2, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION);
  EXPECT_EQ(0, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET);
  EXPECT_EQ(1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET);
  EXPECT_EQ(2, ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET);
  EXPECT_EQ(3, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET);
  EXPECT_EQ(4, ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET);
  EXPECT_EQ(5, ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET);
  EXPECT_EQ(6, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET);
  EXPECT_EQ(8, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET);
  EXPECT_EQ(10, ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET);
  EXPECT_EQ(0xff, ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT);

  EXPECT_EQ(1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST);
  EXPECT_EQ(2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET);
  EXPECT_EQ(3, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET);
  EXPECT_EQ(4, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR);

  EXPECT_EQ(0, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK);
  EXPECT_EQ(1, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
  EXPECT_EQ(2, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OPCODE);
  EXPECT_EQ(3, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_EQ(4, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT);
  EXPECT_EQ(5, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  EXPECT_EQ(6, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
  EXPECT_EQ(7, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INVALID_TEXT);
  EXPECT_EQ(8, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR);
  EXPECT_EQ(9, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL);
}

static void test_malformed_common_requests(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t payload = 'x';

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 1, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT, 0, 0, 0, NULL);
  memset(response, 0xaa, sizeof(response));
  process_request(&protocol, request, response);
  expect_error(response, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 1,
               ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, 0x99, 2, 0, 0xff, 0, 0, 0, NULL);
  memset(response, 0xaa, sizeof(response));
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, 0x99, 2, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OPCODE);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 3, 1, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 3, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 4, 0, 0, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 4, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 5, 0, 0xff,
               0, 0, 1, &payload);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 5, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 6, 0, 16, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 6, 16,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 7, 0, 0, 0, 1,
               0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 7, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 8, 0, 0, 1,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 8, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 9, 0, 16, 0,
               1, 1, &payload);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 9, 16,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 10, 0, 0, 0,
               1, 23, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 10, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 11, 0, 0, 0,
               0, 0, NULL);
  request[31] = 0x5a;
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 11, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  EXPECT_EQ(-EINVAL,
            zmk_runtime_macro_protocol_process(&protocol, request, NULL));
  memset(response, 0xaa, sizeof(response));
  EXPECT_EQ(-EINVAL,
            zmk_runtime_macro_protocol_process(&protocol, NULL, response));
  for (size_t i = 0; i < sizeof(response); i++) {
    EXPECT_EQ(0, response[i]);
  }

  memset(response, 0xaa, sizeof(response));
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(NULL, request, response));
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 11, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL);
}

static void
seed_list_slots(uint16_t lengths[CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT]) {
  static const char slot2_text[] = "abcdefghijklmnopqrstuvw";
  static const char slot10_text[] = "1234567";
  char maximum[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN];

  memset(lengths, 0, sizeof(uint16_t) * CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT);
  memset(maximum, 'm', sizeof(maximum));
  set_slot_text(1, "abcde");
  set_slot_bytes(2, slot2_text, sizeof(slot2_text) - 1U);
  set_slot_text(10, slot10_text);
  set_slot_bytes(15, maximum, sizeof(maximum));
  lengths[1] = 5;
  lengths[2] = 23;
  lengths[10] = 7;
  lengths[15] = CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN;
}

static void test_list_chunks_and_boundaries(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint16_t lengths[CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT];
  uint8_t logical[1U + (2U * CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT)];

  reset_slots();
  seed_list_slots(lengths);
  memset(logical, 0, sizeof(logical));
  logical[0] = CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT;
  for (uint8_t slot = 0; slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT; slot++) {
    logical[1U + (2U * slot)] = (uint8_t)lengths[slot];
    logical[2U + (2U * slot)] = (uint8_t)(lengths[slot] >> 8);
  }
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 20, 0, 0xff,
               0, 0, 0, NULL);
  memset(response, 0xaa, sizeof(response));
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 20, 0xff, 0,
                 33, logical, 22);
  EXPECT_EQ(16, response[10]);
  EXPECT_EQ(5, response[13]);
  EXPECT_EQ(23, response[15]);
  EXPECT_EQ(7, response[31]);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 21, 0, 0xff,
               22, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 21, 0xff, 22,
                 33, logical + 22, 11);
  EXPECT_EQ(22, response[6]);
  EXPECT_EQ(0, response[7]);
  EXPECT_EQ(33, response[8]);
  EXPECT_EQ(0, response[9]);
  EXPECT_EQ(0, response[10]);
  EXPECT_EQ(0, response[11]);
  EXPECT_EQ(0, response[12]);
  EXPECT_EQ(64, response[19]);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 22, 0, 0xff,
               33, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 22, 0xff, 33,
                 33, NULL, 0);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 23, 0, 0xff,
               34, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 23, 0xff,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
}

static void test_get_chunks_and_boundaries(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  static const char text23[] = "abcdefghijklmnopqrstuvw";
  char maximum[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN];

  reset_slots();
  memset(maximum, 'M', sizeof(maximum));
  set_slot_bytes(2, text23, sizeof(text23) - 1U);
  set_slot_bytes(3, maximum, sizeof(maximum));
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 30, 0, 0, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 30, 0, 0, 0,
                 NULL, 0);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 31, 0, 2, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 31, 2, 0, 23,
                 text23, 22);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 32, 0, 2, 22,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 32, 2, 22, 23,
                 text23 + 22, 1);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 33, 0, 2, 23,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 33, 2, 23, 23,
                 NULL, 0);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 34, 0, 2, 24,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 34, 2,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 35, 0, 3, 22,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 35, 3, 22, 64,
                 maximum + 22, 22);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 36, 0, 3, 44,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 36, 3, 44, 64,
                 maximum + 44, 20);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 37, 0, 3, 64,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 37, 3, 64, 64,
                 NULL, 0);
}

static void test_set_single_empty_and_deferred_commit(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  static const char first_chunk[] = "he";
  static const char second_chunk[] = "llo";

  reset_slots();
  set_slot_text(0, "old");
  reset_backend();
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 40, 0, 0, 0,
               5, 5, "hello");
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 40, 0, 5, 5,
                 NULL, 0);
  expect_slot(0, "hello", 5);
  EXPECT_EQ(1, save_calls);
  EXPECT_TRUE(strcmp(saved_name, "runtime_macro/slot/0") == 0);
  EXPECT_EQ(5, saved_length);
  EXPECT_TRUE(strcmp(saved_value, "hello") == 0);
  EXPECT_TRUE(!protocol.set_active);

  set_slot_text(0, "not-empty");
  reset_backend();
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 41, 0, 0, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 41, 0, 0, 0,
                 NULL, 0);
  expect_slot(0, "", 0);
  EXPECT_EQ(1, save_calls);

  set_slot_text(1, "old");
  reset_backend();
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 42, 0, 1, 0,
               5, 2, first_chunk);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 42, 1, 2, 5,
                 NULL, 0);
  expect_slot(1, "old", 3);
  EXPECT_EQ(0, save_calls);
  EXPECT_TRUE(protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 43, 0, 0xff,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);
  EXPECT_TRUE(protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 44, 0, 1, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 44, 1, 0, 3,
                 "old", 3);
  EXPECT_TRUE(protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 42, 0, 1, 2,
               5, 3, second_chunk);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 42, 1, 5, 5,
                 NULL, 0);
  expect_slot(1, "hello", 5);
  EXPECT_EQ(1, save_calls);
  EXPECT_TRUE(!protocol.set_active);
}

static void test_set_split_22_23_and_maximum(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  static const char text23[] = "abcdefghijklmnopqrstuvw";
  char maximum[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN];

  reset_slots();
  memset(maximum, 'q', sizeof(maximum));
  set_slot_text(2, "old");
  set_slot_text(3, "old");
  reset_backend();
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 50, 0, 2, 0,
               23, 22, text23);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 50, 2, 22, 23,
                 NULL, 0);
  expect_slot(2, "old", 3);
  EXPECT_EQ(0, save_calls);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 50, 0, 2, 22,
               23, 1, text23 + 22);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 50, 2, 23, 23,
                 NULL, 0);
  expect_slot(2, text23, 23);
  EXPECT_EQ(1, save_calls);

  reset_backend();
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 51, 0, 3, 0,
               64, 22, maximum);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 51, 3, 22, 64,
                 NULL, 0);
  EXPECT_EQ(0, save_calls);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 51, 0, 3, 22,
               64, 22, maximum + 22);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 51, 3, 44, 64,
                 NULL, 0);
  EXPECT_EQ(0, save_calls);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 51, 0, 3, 44,
               64, 20, maximum + 44);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 51, 3, 64, 64,
                 NULL, 0);
  expect_slot(3, maximum, sizeof(maximum));
  EXPECT_EQ(1, save_calls);
}

static void test_set_replacement_and_independent_contexts(void) {
  struct zmk_runtime_macro_protocol first;
  struct zmk_runtime_macro_protocol second;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];

  reset_slots();
  set_slot_text(4, "old");
  set_slot_text(5, "old");
  set_slot_text(6, "old");
  reset_backend();
  zmk_runtime_macro_protocol_init(&first);
  zmk_runtime_macro_protocol_init(&second);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 60, 0, 4, 0,
               5, 2, "ab");
  process_request(&first, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 60, 4, 2, 5,
                 NULL, 0);
  EXPECT_TRUE(first.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 61, 0, 5, 0,
               2, 2, "xy");
  process_request(&second, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 61, 5, 2, 2,
                 NULL, 0);
  expect_slot(5, "xy", 2);
  EXPECT_TRUE(first.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 60, 0, 4, 0,
               2, 2, "zz");
  process_request(&first, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 60, 4, 2, 2,
                 NULL, 0);
  expect_slot(4, "zz", 2);
  EXPECT_TRUE(!first.set_active);

  reset_backend();
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 62, 0, 6, 0,
               4, 2, "ab");
  process_request(&first, request, response);
  EXPECT_TRUE(first.set_active);
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 63, 0, 7, 0,
               0, 0, NULL);
  process_request(&first, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 63, 7, 0, 0,
                 NULL, 0);
  EXPECT_TRUE(first.set_active);
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 62, 0, 6, 2,
               4, 2, "cd");
  process_request(&first, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 62, 6, 4, 4,
                 NULL, 0);
  expect_slot(6, "abcd", 4);
}

static void test_set_duplicate_chunks_require_restart(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];

  reset_slots();
  set_slot_text(7, "old");
  reset_backend();
  zmk_runtime_macro_protocol_init(&protocol);

  /* Repeating an already accepted non-final chunk invalidates staging. */
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 0, 7, 0,
               6, 2, "ab");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 0, 7, 2,
               6, 2, "cd");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 0, 7, 2,
               6, 2, "cd");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  EXPECT_TRUE(!protocol.set_active);
  expect_slot(7, "old", 3);

  /* A final ACK lost by the host is also recovered by restarting at offset 0.
   */
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 0, 7, 0,
               4, 2, "ne");
  process_request(&protocol, request, response);
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 0, 7, 2,
               4, 2, "w!");
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 7, 4, 4,
                 NULL, 0);
  expect_slot(7, "new!", 4);

  /* The retransmitted final chunk has no active transaction to resume. */
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 69, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.set_active);
  expect_slot(7, "new!", 4);

  /* Restarting from offset 0 is accepted after either recovery path. */
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               2, 2, "ok");
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 7, 2, 2,
                 NULL, 0);
  expect_slot(7, "ok", 2);
}

static void test_set_invalid_chunks_clear_staging(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];

  reset_slots();
  set_slot_text(7, "keep");
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               4, 2, "ke");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 3,
               4, 1, "p");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  EXPECT_TRUE(!protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 2,
               4, 2, "ep");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  expect_slot(7, "keep", 4);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               4, 2, "ke");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 71, 0, 7, 2,
               4, 2, "ep");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 71, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               4, 2, "ke");
  process_request(&protocol, request, response);
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 8, 2,
               4, 2, "ep");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               4, 2, "ke");
  process_request(&protocol, request, response);
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 2,
               5, 2, "ep");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               4, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
  EXPECT_TRUE(!protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               4, 2, "a\r");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INVALID_TEXT);
  EXPECT_TRUE(!protocol.set_active);
  expect_slot(7, "keep", 4);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 0, 7, 0,
               2, 2, "ok");
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 70, 7, 2, 2,
                 NULL, 0);
  expect_slot(7, "ok", 2);
}

static void test_set_range_validation_and_common_clear(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t payload[22];

  memset(payload, 'x', sizeof(payload));
  reset_slots();
  set_slot_text(8, "keep");
  zmk_runtime_macro_protocol_init(&protocol);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 1,
               1, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 0,
               1, 2, payload);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 0,
               65, 1, payload);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 0,
               4, 1, payload);
  request[31] = 1;
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 0,
               4, 2, "ab");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 1, 8, 2,
               4, 2, "cd");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 0,
               4, 2, "ab");
  process_request(&protocol, request, response);
  EXPECT_TRUE(protocol.set_active);
  make_request(request, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 2,
               4, 2, "cd");
  process_request(&protocol, request, response);
  expect_error(response, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
  EXPECT_TRUE(!protocol.set_active);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 0,
               0, 1, "x");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 0, 8, 1,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 80, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  expect_slot(8, "keep", 4);
}

static void test_storage_errors_and_clear(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];

  reset_slots();
  set_slot_text(9, "old");
  reset_backend();
  zmk_runtime_macro_protocol_init(&protocol);

  save_result = -EIO;
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 90, 0, 9, 0,
               3, 3, "new");
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 90, 9,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR);
  expect_slot(9, "new", 3);
  EXPECT_EQ(1, save_calls);
  EXPECT_TRUE(!protocol.set_active);

  reset_backend();
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 91, 0, 9, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 91, 9, 0, 0,
                 NULL, 0);
  expect_slot(9, "", 0);
  EXPECT_EQ(1, delete_calls);

  set_slot_text(9, "again");
  reset_backend();
  delete_result = -ENOSPC;
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 92, 0, 9, 0,
               0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR, 92, 9,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR);
  expect_slot(9, "", 0);
  EXPECT_EQ(1, delete_calls);
}

int main(void) {
  test_wire_constants();
  test_malformed_common_requests();
  test_list_chunks_and_boundaries();
  test_get_chunks_and_boundaries();
  test_set_single_empty_and_deferred_commit();
  test_set_split_22_23_and_maximum();
  test_set_replacement_and_independent_contexts();
  test_set_duplicate_chunks_require_restart();
  test_set_invalid_chunks_clear_staging();
  test_set_range_validation_and_common_clear();
  test_storage_errors_and_clear();

  if (failures != 0) {
    fprintf(stderr, "%d test assertion(s) failed\n", failures);
    return 1;
  }

  puts("runtime macro protocol tests: PASS");
  return 0;
}
