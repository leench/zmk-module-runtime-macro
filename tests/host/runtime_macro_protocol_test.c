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
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC 1
#define CONFIG_ZMK_BLE 1
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_USB_DISCONNECT 1
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE 1
#define CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_ENDPOINT_CHANGE 1

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
#include "../../src/runtime_macro_dynamic.c"
#include "../../src/runtime_macro_protocol.c"

int host_work_schedule(struct k_work_delayable *work, k_timeout_t delay,
                       bool reschedule) {
  (void)reschedule;
  work->scheduled = true;
  work->delay = delay;
  return 0;
}

int zmk_runtime_macro_executor_start(const uint8_t *text, size_t length) {
  (void)text;
  (void)length;
  return 0;
}

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
  if (runtime_macro_protocol_dynamic_timeout_owner != NULL) {
    zmk_runtime_macro_protocol_discard(
        runtime_macro_protocol_dynamic_timeout_owner);
  }
  runtime_macro_auth_test_reset();
  host_uptime = 0;
  zmk_runtime_macro_dynamic_reset();
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

static void expect_dynamic_text(const uint8_t *expected, size_t length) {
  EXPECT_TRUE(runtime_macro_dynamic_state.committed_valid);
  EXPECT_EQ(length, runtime_macro_dynamic_state.committed_length);
  EXPECT_TRUE(memcmp(runtime_macro_dynamic_state.committed, expected, length) ==
              0);
  for (size_t i = length;
       i < sizeof(runtime_macro_dynamic_state.committed); i++) {
    EXPECT_EQ(0, runtime_macro_dynamic_state.committed[i]);
  }
}

static void expect_dynamic_empty(void) {
  EXPECT_TRUE(!runtime_macro_dynamic_state.committed_valid);
  EXPECT_EQ(0, runtime_macro_dynamic_state.committed_length);
  EXPECT_TRUE(!runtime_macro_dynamic_state.staging_active);
  for (size_t i = 0; i < sizeof(runtime_macro_dynamic_state.committed); i++) {
    EXPECT_EQ(0, runtime_macro_dynamic_state.committed[i]);
  }
  for (size_t i = 0; i < sizeof(runtime_macro_dynamic_state.staging); i++) {
    EXPECT_EQ(0, runtime_macro_dynamic_state.staging[i]);
  }
}

static void dynamic_begin(struct zmk_runtime_macro_protocol *protocol,
                          uint8_t request_id, uint16_t total_length,
                          uint32_t ttl_seconds, uint8_t *request,
                          uint8_t *response) {
  uint8_t ttl[4];
  uint8_t payload_length = 0U;
  const void *payload = NULL;

  if (ttl_seconds != 0U) {
    ttl[0] = (uint8_t)ttl_seconds;
    ttl[1] = (uint8_t)(ttl_seconds >> 8);
    ttl[2] = (uint8_t)(ttl_seconds >> 16);
    ttl[3] = (uint8_t)(ttl_seconds >> 24);
    payload_length = sizeof(ttl);
    payload = ttl;
  }

  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION,
               ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN, request_id, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, total_length,
               payload_length, payload);
  process_request(protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN,
                 request_id, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0,
                 total_length, NULL, 0);
}

static void dynamic_data(struct zmk_runtime_macro_protocol *protocol,
                         uint8_t request_id, uint16_t offset,
                         uint16_t total_length, const void *payload,
                         uint8_t payload_length, uint8_t *request,
                         uint8_t *response) {
  make_request(request, ZMK_RUNTIME_MACRO_PROTOCOL_VERSION,
               ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, request_id, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, offset, total_length,
               payload_length, payload);
  process_request(protocol, request, response);
}

static void commit_dynamic_text(struct zmk_runtime_macro_protocol *protocol,
                                uint8_t request_id, const uint8_t *text,
                                uint16_t length, uint8_t *request,
                                uint8_t *response) {
  dynamic_begin(protocol, request_id, length, 0U, request, response);
  uint16_t offset = 0U;
  while (offset < length) {
    uint16_t remaining = (uint16_t)(length - offset);
    uint8_t chunk = (uint8_t)(remaining > 22U ? 22U : remaining);
    dynamic_data(protocol, request_id, offset, length, text + offset, chunk,
                 request, response);
    expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA,
                   request_id, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
                   offset + chunk, length, NULL, 0);
    offset += chunk;
  }
}

static void test_dynamic_wire_constants_and_capabilities(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  EXPECT_EQ(0x20, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN);
  EXPECT_EQ(0x21, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA);
  EXPECT_EQ(0x22, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR);
  EXPECT_EQ(0x23, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES);
  EXPECT_EQ(0xff, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT);
  EXPECT_EQ(22, ZMK_RUNTIME_MACRO_PROTOCOL_CAPABILITY_PAYLOAD_LENGTH);
  EXPECT_EQ(0x003f,
            ZMK_RUNTIME_MACRO_PROTOCOL_CAPABILITY_FIXED_LIFECYCLE_FLAGS |
                ZMK_RUNTIME_MACRO_PROTOCOL_CAPABILITY_CLEAR_ON_USB_DISCONNECT |
                ZMK_RUNTIME_MACRO_PROTOCOL_CAPABILITY_CLEAR_ON_BLE_PROFILE_CHANGE |
                ZMK_RUNTIME_MACRO_PROTOCOL_CAPABILITY_CLEAR_ON_ENDPOINT_CHANGE);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 1,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  uint8_t expected[22] = {
      1, 1, 0x3f, 0x00, 0x00, 0x01, 0x2c, 0x01, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x80, 0x51, 0x01, 0x00, 0x1e, 0x00,
      0x00, 0x00,
  };
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 1,
                 ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 22, expected,
                 22);
  expect_dynamic_empty();

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 2,
               0, 0, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 2,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 3,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 1, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 3,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
}

static void test_dynamic_upload_sizes_ttl_and_no_readback(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  uint8_t text[256];
  const uint16_t lengths[] = {1, 22, 23, 256};

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  memset(text, 'd', sizeof(text));

  for (size_t test = 0; test < sizeof(lengths) / sizeof(lengths[0]); test++) {
    uint16_t length = lengths[test];
    uint8_t request_id = (uint8_t)(10U + test);
    dynamic_begin(&protocol, request_id, length, test == 1U ? 600U : 0U,
                  request, response);
    uint16_t offset = 0U;
    while (offset < length) {
      uint16_t remaining = (uint16_t)(length - offset);
      uint8_t chunk = (uint8_t)(remaining > 22U ? 22U : remaining);
      dynamic_data(&protocol, request_id, offset, length, text + offset, chunk,
                   request, response);
      expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA,
                     request_id, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
                     offset + chunk, length, NULL, 0);
      for (size_t i = 10; i < 32; i++) {
        EXPECT_EQ(0, response[i]);
      }
      offset += chunk;
    }
    expect_dynamic_text(text, length);
    if (test == 0U) {
      EXPECT_EQ(300000, runtime_macro_dynamic_state.ttl_deadline_ms);
    }
    if (test == 1U) {
      EXPECT_EQ(600000, runtime_macro_dynamic_state.ttl_deadline_ms);
    }

    make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR,
                 (uint8_t)(100U + test), 0,
                 ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
    process_request(&protocol, request, response);
    expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR,
                   (uint8_t)(100U + test),
                   ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, NULL, 0);
    expect_dynamic_empty();
  }
}

static void test_dynamic_validation_and_restart(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  const uint8_t old_text[] = "old";

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  commit_dynamic_text(&protocol, 1, old_text, sizeof(old_text) - 1U, request,
                      response);
  expect_dynamic_text(old_text, sizeof(old_text) - 1U);

  dynamic_begin(&protocol, 2, 4, 0, request, response);
  dynamic_data(&protocol, 2, 0, 4, "ab", 2, request, response);
  dynamic_data(&protocol, 2, 0, 4, "ab", 2, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 2,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  EXPECT_TRUE(!protocol.dynamic_active);
  expect_dynamic_text(old_text, sizeof(old_text) - 1U);

  dynamic_begin(&protocol, 3, 4, 0, request, response);
  dynamic_data(&protocol, 3, 1, 4, "a", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 3,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
  dynamic_begin(&protocol, 4, 4, 0, request, response);
  dynamic_data(&protocol, 5, 0, 4, "a", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 5,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  dynamic_begin(&protocol, 6, 4, 0, request, response);
  dynamic_data(&protocol, 6, 0, 3, "a", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 6,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  dynamic_begin(&protocol, 7, 4, 0, request, response);
  dynamic_data(&protocol, 7, 0, 4, NULL, 0, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 7,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  dynamic_begin(&protocol, 8, 4, 0, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 8,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 4, 23, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 8,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  dynamic_begin(&protocol, 9, 2, 0, request, response);
  dynamic_data(&protocol, 9, 0, 2, "a\r", 2, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 9,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INVALID_TEXT);
  expect_dynamic_text(old_text, sizeof(old_text) - 1U);

  dynamic_begin(&protocol, 10, 1, 0, request, response);
  dynamic_data(&protocol, 10, 2, 1, "a", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 10,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);

  dynamic_begin(&protocol, 11, 1, 0, request, response);
  dynamic_data(&protocol, 11, 0, 1, "ab", 2, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 11,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  dynamic_begin(&protocol, 12, 2, 0, request, response);
  dynamic_data(&protocol, 12, 0, 2, "xy", 2, request, response);
  expect_dynamic_text((const uint8_t *)"xy", 2);
  dynamic_data(&protocol, 12, 0, 2, "xy", 2, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 12,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  expect_dynamic_text((const uint8_t *)"xy", 2);

  dynamic_begin(&protocol, 13, 4, 0, request, response);
  dynamic_data(&protocol, 13, 0, 4, "ab", 2, request, response);
  dynamic_begin(&protocol, 14, 2, 0, request, response);
  dynamic_data(&protocol, 14, 0, 2, "ok", 2, request, response);
  expect_dynamic_text((const uint8_t *)"ok", 2);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN,
               15, 0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 2, 4,
               (uint8_t[]){0, 0, 0, 0});
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN,
               15, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN,
               16, 0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 2, 4,
               (uint8_t[]){0x51, 0xc4, 0x01, 0x00});
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN,
               16, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN,
               17, 0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 2, 1,
               "x");
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN,
               17, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);

  dynamic_begin(&protocol, 18, 2, 0, request, response);
  dynamic_data(&protocol, 18, 0, 2, "a", 1, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA,
               18, 1, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 1, 2, 1,
               "b");
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA,
               18, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(!protocol.dynamic_active);
  expect_dynamic_text((const uint8_t *)"ok", 2);
}

static void test_dynamic_clear_and_static_staging_isolation(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 20, 0, 1,
               0, 6, 2, "st");
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 20, 1, 2, 6,
                 NULL, 0);
  EXPECT_TRUE(protocol.set_active);

  dynamic_begin(&protocol, 21, 3, 0, request, response);
  dynamic_data(&protocol, 21, 0, 3, "dyn", 3, request, response);
  expect_dynamic_text((const uint8_t *)"dyn", 3);
  EXPECT_TRUE(protocol.set_active);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR,
               22, 0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 1, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR, 22,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(protocol.set_active);
  expect_dynamic_text((const uint8_t *)"dyn", 3);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR,
               23, 0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR, 23,
                 ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, NULL, 0);
  expect_dynamic_empty();
  EXPECT_TRUE(protocol.set_active);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR,
               24, 0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR, 24,
                 ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, NULL, 0);
  expect_dynamic_empty();
  EXPECT_TRUE(protocol.set_active);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 20, 0, 1,
               2, 6, 4, "atic");
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET, 20, 1, 6, 6,
                 NULL, 0);
  expect_slot(1, "static", 6);

  commit_dynamic_text(&protocol, 25, (const uint8_t *)"dynamic", 7, request,
                      response);
  uint8_t list_expected[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE] = {0};
  list_expected[0] = CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT;
  list_expected[3] = 6;
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 26, 0,
               ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST, 26,
                 ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT, 0,
                 (uint16_t)(1U + 2U * CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT),
                 list_expected, ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE);
  EXPECT_EQ(6, frame_get_u16(response, 13));

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 27, 0, 1,
               0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_success(response, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET, 27, 1, 0, 6,
                 "static", 6);
}

static void test_dynamic_transaction_timeout_and_discard(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];
  const uint8_t old_text[] = "keep";

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  commit_dynamic_text(&protocol, 30, old_text, sizeof(old_text) - 1U, request,
                      response);
  dynamic_begin(&protocol, 31, 5, 0, request, response);
  dynamic_data(&protocol, 31, 0, 5, "new", 3, request, response);
  EXPECT_TRUE(protocol.dynamic_active);
  host_uptime = 30001;
  runtime_macro_protocol_dynamic_timeout_work.work.handler(
      &runtime_macro_protocol_dynamic_timeout_work.work);
  EXPECT_TRUE(!protocol.dynamic_active);
  EXPECT_TRUE(!runtime_macro_dynamic_state.staging_active);
  expect_dynamic_text(old_text, sizeof(old_text) - 1U);

  dynamic_begin(&protocol, 32, 3, 0, request, response);
  dynamic_data(&protocol, 32, 0, 3, "abc", 3, request, response);
  EXPECT_TRUE(!protocol.dynamic_active);
  zmk_runtime_macro_protocol_discard(&protocol);
  expect_dynamic_text((const uint8_t *)"abc", 3);

  dynamic_begin(&protocol, 33, 3, 0, request, response);
  dynamic_data(&protocol, 33, 0, 3, "ab", 2, request, response);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR, 34,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 1, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR,
               34, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  EXPECT_TRUE(protocol.dynamic_active);
  EXPECT_TRUE(runtime_macro_dynamic_state.staging_active);
  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR, 35,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_dynamic_empty();
}

static void test_dynamic_wrong_version_staging_rules(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  dynamic_begin(&protocol, 50, 2, 0, request, response);
  dynamic_data(&protocol, 50, 0, 2, "a", 1, request, response);
  make_request(request, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN, 51,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 2, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 1,
               ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_BEGIN, 51,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
  EXPECT_TRUE(!protocol.dynamic_active);
  dynamic_data(&protocol, 50, 1, 2, "b", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 50,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  dynamic_begin(&protocol, 52, 2, 0, request, response);
  dynamic_data(&protocol, 52, 0, 2, "a", 1, request, response);
  make_request(request, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 52,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 1, 2, 1, "b");
  process_request(&protocol, request, response);
  expect_error(response, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 52,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
  EXPECT_TRUE(!protocol.dynamic_active);
  dynamic_data(&protocol, 52, 1, 2, "b", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 52,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);

  dynamic_begin(&protocol, 53, 2, 0, request, response);
  dynamic_data(&protocol, 53, 0, 2, "c", 1, request, response);
  make_request(request, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR, 53,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_CLEAR,
               53, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
  EXPECT_TRUE(protocol.dynamic_active);
  dynamic_data(&protocol, 53, 1, 2, "d", 1, request, response);
  expect_dynamic_text((const uint8_t *)"cd", 2);

  dynamic_begin(&protocol, 54, 2, 0, request, response);
  dynamic_data(&protocol, 54, 0, 2, "e", 1, request, response);
  make_request(request, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 54,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  expect_error(response, 1, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES,
               54, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
  EXPECT_TRUE(protocol.dynamic_active);
  dynamic_data(&protocol, 54, 1, 2, "f", 1, request, response);
  expect_dynamic_text((const uint8_t *)"ef", 2);
}

static void set_dynamic_credential(uint8_t marker) {
  struct zmk_runtime_macro_auth_credential credential = {
      .iterations = ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT,
  };
  memset(credential.salt, marker, sizeof(credential.salt));
  memset(credential.key, (int)(marker + 1U), sizeof(credential.key));
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
}

static void authenticate_dynamic_direct(void) {
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  memset(proof, 0x5a, sizeof(proof));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
}

static void test_dynamic_bypasses_auth_without_refresh(void) {
  struct zmk_runtime_macro_protocol protocol;
  uint8_t request[32];
  uint8_t response[32];

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  dynamic_begin(&protocol, 39, 1, 0, request, response);
  dynamic_data(&protocol, 39, 0, 1, "o", 1, request, response);
  expect_dynamic_text((const uint8_t *)"o", 1);
  dynamic_begin(&protocol, 38, 2, 0, request, response);
  dynamic_data(&protocol, 38, 0, 2, "x", 1, request, response);

  set_dynamic_credential(0x41);
  dynamic_data(&protocol, 38, 1, 2, "y", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 38,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  expect_dynamic_text((const uint8_t *)"o", 1);
  dynamic_begin(&protocol, 40, 1, 0, request, response);
  dynamic_data(&protocol, 40, 0, 1, "p", 1, request, response);
  expect_dynamic_text((const uint8_t *)"p", 1);

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  set_dynamic_credential(0x42);
  authenticate_dynamic_direct();
  int64_t deadline = runtime_macro_auth.session_deadline_ms;
  dynamic_begin(&protocol, 41, 1, 0, request, response);
  dynamic_data(&protocol, 41, 0, 1, "q", 1, request, response);
  EXPECT_EQ(deadline, runtime_macro_auth.session_deadline_ms);
  expect_dynamic_text((const uint8_t *)"q", 1);

  make_request(request, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CAPABILITIES, 43,
               0, ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT, 0, 0, 0, NULL);
  process_request(&protocol, request, response);
  EXPECT_EQ(deadline, runtime_macro_auth.session_deadline_ms);

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  set_dynamic_credential(0x44);
  authenticate_dynamic_direct();
  dynamic_begin(&protocol, 44, 1, 600, request, response);
  dynamic_data(&protocol, 44, 0, 1, "s", 1, request, response);
  dynamic_begin(&protocol, 45, 2, 0, request, response);
  dynamic_data(&protocol, 45, 0, 2, "t", 1, request, response);
  host_uptime = 300001;
  dynamic_data(&protocol, 45, 1, 2, "u", 1, request, response);
  expect_error(response, 2, ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_DYNAMIC_DATA, 45,
               ZMK_RUNTIME_MACRO_PROTOCOL_DYNAMIC_SLOT,
               ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
  expect_dynamic_text((const uint8_t *)"s", 1);

  reset_slots();
  zmk_runtime_macro_protocol_init(&protocol);
  set_dynamic_credential(0x43);
  runtime_macro_auth.state = ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED;
  dynamic_begin(&protocol, 42, 1, 0, request, response);
  dynamic_data(&protocol, 42, 0, 1, "r", 1, request, response);
  expect_dynamic_text((const uint8_t *)"r", 1);
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
  test_dynamic_wire_constants_and_capabilities();
  test_dynamic_upload_sizes_ttl_and_no_readback();
  test_dynamic_validation_and_restart();
  test_dynamic_clear_and_static_staging_isolation();
  test_dynamic_transaction_timeout_and_discard();
  test_dynamic_wrong_version_staging_rules();
  test_dynamic_bypasses_auth_without_refresh();

  if (failures != 0) {
    fprintf(stderr, "%d test assertion(s) failed\n", failures);
    return 1;
  }

  puts("runtime macro protocol tests: PASS");
  return 0;
}
