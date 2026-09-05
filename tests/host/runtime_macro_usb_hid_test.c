/*
 * Host-side tests for the optional legacy USB HID transport.
 *
 * The production sources are included directly so descriptor registration,
 * class callbacks, queueing, work scheduling, and endpoint throttling can be
 * checked without a USB controller.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/usb.h>

#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_RUNTIME_MACRO_USB_HID 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_TEST 1
#define ZMK_RUNTIME_MACRO_USB_HID_TEST 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT 30
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT 300
#define CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT 2
#define CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN 8
#define CONFIG_ZMK_MACRO_DEFAULT_TAP_MS 10
#define CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS 20
#define CONFIG_USB_HID_DEVICE_COUNT 2
#define CONFIG_HID_INTERRUPT_EP_MPS 32
#define CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE "HID_1"
#define CONFIG_ZMK_USB_HID_INIT_PRIORITY 95
#define ZMK_RUNTIME_MACRO_AUTH_TEST 1

int64_t host_uptime;
static enum usb_dc_status_code usb_status;
static bool usb_status_change_on_next_read;
static enum usb_dc_status_code usb_status_next_read;

enum usb_dc_status_code zmk_usb_get_status(void) {
  enum usb_dc_status_code status = usb_status;
  if (usb_status_change_on_next_read) {
    usb_status_change_on_next_read = false;
    usb_status = usb_status_next_read;
  }
  return status;
}

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

static int save_result;
static int delete_result;
static unsigned int save_calls;
static unsigned int delete_calls;
static unsigned int work_submit_calls;
static int work_submit_failure_call = -1;
static unsigned int hid_register_calls;
static unsigned int hid_init_calls;
static unsigned int hid_write_calls;
static int hid_init_result;
static int hid_write_result;
static const struct device *bound_device;
static const struct device *registered_device;
static const struct hid_ops *registered_ops;
static const uint8_t *registered_descriptor;
static size_t registered_descriptor_size;
static const struct device *last_write_device;
static const uint8_t *last_write_data_pointer;
static uint8_t last_write_data[32];
static uint32_t last_write_length;
static bool reset_after_online_check;
static struct device hid1 = {.name = "HID_1", .ready = true};

#include "../../src/runtime_macro_auth.c"
#include "../../src/runtime_macro.c"
#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro_protocol.c"
#include "../../src/runtime_macro_usb_hid.c"

void runtime_macro_usb_hid_test_after_online_check(void) {
  if (reset_after_online_check) {
    reset_after_online_check = false;
    usb_status = USB_DC_RESET;
    runtime_macro_usb_hid_transport_reset(ZMK_USB_CONN_POWERED,
                                          USB_DC_RESET);
    usb_status = USB_DC_CONFIGURED;
  }
}

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

int settings_save_one(const char *name, const void *value, size_t length) {
  (void)name;
  (void)value;
  (void)length;
  save_calls++;
  return save_result;
}

int settings_delete(const char *name) {
  (void)name;
  delete_calls++;
  return delete_result;
}

const struct device *device_get_binding(const char *name) {
  EXPECT_TRUE(strcmp(name, CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE) == 0);
  return bound_device;
}

bool device_is_ready(const struct device *dev) {
  return dev != NULL && dev->ready;
}

void usb_hid_register_device(const struct device *dev, const uint8_t *desc,
                             size_t size, const struct hid_ops *ops) {
  hid_register_calls++;
  registered_device = dev;
  registered_descriptor = desc;
  registered_descriptor_size = size;
  registered_ops = ops;
}

int usb_hid_init(const struct device *dev) {
  hid_init_calls++;
  EXPECT_TRUE(dev == &hid1);
  return hid_init_result;
}

int hid_int_ep_write(const struct device *dev, const uint8_t *data,
                     uint32_t data_len, uint32_t *bytes_ret) {
  (void)bytes_ret;
  hid_write_calls++;
  last_write_device = dev;
  last_write_data_pointer = data;
  last_write_length = data_len;
  if (data_len <= sizeof(last_write_data)) {
    memcpy(last_write_data, data, data_len);
  }
  return hid_write_result;
}

int host_work_submit(struct k_work *work) {
  unsigned int call = work_submit_calls++;
  if (call == (unsigned int)work_submit_failure_call) {
    return -EIO;
  }

  work->submitted = true;
  return 0;
}

static size_t k_msgq_used(const struct k_msgq *msgq) { return msgq->used_msgs; }

static void reset_backend(void) {
  save_result = 0;
  delete_result = 0;
  save_calls = 0;
  delete_calls = 0;
}

static void reset_transport(void) {
  runtime_macro_auth_test_reset();
  host_uptime = 0;
  reset_backend();
  usb_status = USB_DC_CONFIGURED;
  usb_status_change_on_next_read = false;
  usb_status_next_read = USB_DC_CONFIGURED;
  work_submit_calls = 0;
  work_submit_failure_call = -1;
  hid_register_calls = 0;
  hid_init_calls = 0;
  hid_write_calls = 0;
  hid_init_result = 0;
  hid_write_result = 0;
  bound_device = &hid1;
  registered_device = NULL;
  registered_ops = NULL;
  registered_descriptor = NULL;
  registered_descriptor_size = 0;
  last_write_device = NULL;
  last_write_data_pointer = NULL;
  last_write_length = 0;
  reset_after_online_check = false;
  memset(last_write_data, 0, sizeof(last_write_data));
  memset(runtime_macro_usb_hid_tx_response, 0,
         sizeof(runtime_macro_usb_hid_tx_response));
  k_msgq_purge(&runtime_macro_usb_hid_msgq);
  atomic_set(&runtime_macro_usb_hid_generation, 0);
  atomic_set(&runtime_macro_usb_hid_in_flight, 0);
  atomic_set(&runtime_macro_usb_hid_online, 1);
  k_sem_reset(&runtime_macro_usb_hid_in_sem);
  k_sem_give(&runtime_macro_usb_hid_in_sem);
  runtime_macro_usb_hid_work.submitted = false;
  runtime_macro_usb_hid_dev = NULL;
  zmk_runtime_macro_protocol_init(&runtime_macro_usb_hid_protocol);
}

static void run_work_once(void) {
  EXPECT_TRUE(runtime_macro_usb_hid_work.submitted);
  runtime_macro_usb_hid_work.submitted = false;
  runtime_macro_usb_hid_work.handler(&runtime_macro_usb_hid_work);
}

static void make_list_request(uint8_t *frame, uint8_t request_id) {
  memset(frame, 0, ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE);
  frame[0] = ZMK_RUNTIME_MACRO_PROTOCOL_VERSION;
  frame[1] = ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST;
  frame[2] = request_id;
  frame[4] = ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT;
}

static void test_descriptor(void) {
  static const uint8_t expected[] = {
      0x06, 0x60, 0xff, 0x09, 0x61, 0xa1, 0x01, 0x09, 0x62,
      0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x20,
      0x81, 0x02, 0x09, 0x63, 0x91, 0x02, 0xc0,
  };

  reset_transport();
  EXPECT_EQ(0, runtime_macro_usb_hid_init());
  EXPECT_EQ(1, hid_register_calls);
  EXPECT_EQ(1, hid_init_calls);
  EXPECT_TRUE(registered_device == &hid1);
  EXPECT_TRUE(registered_ops == &runtime_macro_usb_hid_ops);
  EXPECT_EQ(sizeof(expected), registered_descriptor_size);
  EXPECT_TRUE(memcmp(expected, registered_descriptor, sizeof(expected)) == 0);
  EXPECT_TRUE(runtime_macro_usb_hid_ops.int_in_ready != NULL);
  EXPECT_TRUE(runtime_macro_usb_hid_ops.set_report != NULL);
}

static void test_set_report_validation_and_copy(void) {
  uint8_t frame[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  uint8_t *data = frame;
  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE,
  };
  int32_t len = ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE;

  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;
  make_list_request(frame, 11);

  setup.wValue = 0x0100;
  EXPECT_EQ(-EINVAL,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  setup.wValue = 0x0201;
  EXPECT_EQ(-EINVAL,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  setup.wValue = 0x0200;
  len = 31;
  EXPECT_EQ(-EINVAL,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  len = 32;
  data = NULL;
  EXPECT_EQ(-EINVAL,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  data = frame;
  setup.bRequest = 0;
  EXPECT_EQ(-EINVAL,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  setup.bRequest = USB_HID_SET_REPORT;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  frame[2] = 99;
  EXPECT_EQ(0, save_calls);
  EXPECT_EQ(0, hid_write_calls);
  EXPECT_EQ(1, k_msgq_used(&runtime_macro_usb_hid_msgq));

  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(32, last_write_length);
  EXPECT_TRUE(last_write_device == &hid1);
  EXPECT_TRUE(last_write_data_pointer == runtime_macro_usb_hid_tx_response);
  EXPECT_TRUE(memcmp(last_write_data, runtime_macro_usb_hid_tx_response,
                     sizeof(last_write_data)) == 0);
  EXPECT_EQ(11, last_write_data_pointer[2]);
  EXPECT_EQ(11, runtime_macro_usb_hid_tx_response[2]);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK,
            runtime_macro_usb_hid_tx_response[3]);
}

static void test_queue_full_does_not_overwrite(void) {
  uint8_t frames[RUNTIME_MACRO_USB_HID_QUEUE_DEPTH][32];
  uint8_t *data;
  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = 32,
  };
  int32_t len = 32;

  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;
  for (unsigned int i = 0; i < RUNTIME_MACRO_USB_HID_QUEUE_DEPTH; i++) {
    make_list_request(frames[i], (uint8_t)(20U + i));
    data = frames[i];
    EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  }

  uint8_t extra[32];
  make_list_request(extra, 99);
  data = extra;
  EXPECT_EQ(-ENOSPC,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  EXPECT_EQ(RUNTIME_MACRO_USB_HID_QUEUE_DEPTH,
            k_msgq_used(&runtime_macro_usb_hid_msgq));

  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(20, last_write_data[2]);
}

static void test_work_serialization_and_completion(void) {
  uint8_t first[32];
  uint8_t second[32];
  uint8_t *data;
  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = 32,
  };
  int32_t len = 32;

  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;
  make_list_request(first, 30);
  make_list_request(second, 31);
  data = first;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  data = second;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));

  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_TRUE(last_write_data_pointer == runtime_macro_usb_hid_tx_response);
  EXPECT_EQ(30, runtime_macro_usb_hid_tx_response[2]);
  uint8_t first_response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  memcpy(first_response, runtime_macro_usb_hid_tx_response,
         sizeof(first_response));
  EXPECT_EQ(1, k_msgq_used(&runtime_macro_usb_hid_msgq));

  /* A busy work invocation must neither consume nor overwrite the DMA buffer.
   */
  runtime_macro_usb_hid_work.submitted = true;
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(1, k_msgq_used(&runtime_macro_usb_hid_msgq));
  EXPECT_TRUE(memcmp(first_response, runtime_macro_usb_hid_tx_response,
                     sizeof(first_response)) == 0);
  EXPECT_EQ(30, last_write_data_pointer[2]);
  EXPECT_EQ(30, runtime_macro_usb_hid_tx_response[2]);

  /* Only completion releases the permit and allows the next response to update
   * it. */
  runtime_macro_usb_hid_int_in_ready(&hid1);
  run_work_once();
  EXPECT_EQ(2, hid_write_calls);
  EXPECT_TRUE(last_write_data_pointer == runtime_macro_usb_hid_tx_response);
  EXPECT_EQ(31, last_write_data_pointer[2]);
  EXPECT_EQ(31, runtime_macro_usb_hid_tx_response[2]);
  EXPECT_EQ(31, last_write_data[2]);
  EXPECT_EQ(0, k_msgq_used(&runtime_macro_usb_hid_msgq));
}

static void test_write_failure_releases_permit(void) {
  uint8_t frame[32];
  uint8_t *data = frame;
  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = 32,
  };
  int32_t len = 32;

  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;
  hid_write_result = -EIO;
  make_list_request(frame, 40);
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(0, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  k_sem_give(&runtime_macro_usb_hid_in_sem);

  hid_write_result = 0;
  make_list_request(frame, 41);
  data = frame;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(2, hid_write_calls);
}

static void test_generation_snapshot_race_and_reconnect(void) {
  uint8_t request[32];
  uint8_t *data;
  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = 32,
  };
  int32_t len = 32;

  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;
  make_list_request(request, 60);
  data = request;

  /* Force RESET after the callback sampled both generation and online, but
   * before it can enqueue. The resulting frame must retain the old epoch and
   * be discarded after reconnection rather than being executed. */
  reset_after_online_check = true;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  EXPECT_EQ(1, k_msgq_used(&runtime_macro_usb_hid_msgq));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));

  /* The real HID listener now performs another complete reset. Publish the
   * reconnect directly in this test-only path so that the queued old-epoch
   * frame cannot be hidden by that second purge; work must reject it by
   * generation instead. */
  atomic_set(&runtime_macro_usb_hid_online, 1);
  run_work_once();
  EXPECT_EQ(0, hid_write_calls);
  EXPECT_EQ(0, k_msgq_used(&runtime_macro_usb_hid_msgq));

  /* A late callback for the canceled transfer is ignored after reconnect and
   * cannot consume the permit for a later transfer. */
  runtime_macro_usb_hid_int_in_ready(&hid1);
  EXPECT_EQ(0, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  k_sem_give(&runtime_macro_usb_hid_in_sem);

  /* A request from the new connection executes normally. */
  make_list_request(request, 61);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(61, last_write_data[2]);
}

static void test_transport_reset_purges_state_and_recovers(void) {
  uint8_t request[32];
  uint8_t response[32];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  struct zmk_runtime_macro_auth_credential credential = {
      .iterations = ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT,
  };
  memset(credential.salt, 0x31, sizeof(credential.salt));
  memset(credential.key, 0x32, sizeof(credential.key));
  memset(proof, 0x5a, sizeof(proof));

  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));

  /* Leave both protocol transactions incomplete before the connection-state
   * notification. */
  memset(request, 0, sizeof(request));
  request[0] = ZMK_RUNTIME_MACRO_PROTOCOL_VERSION;
  request[1] = ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET;
  request[4] = 0;
  request[5] = 2;
  request[8] = 4;
  request[10] = 'a';
  request[11] = 'b';
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(
                  &runtime_macro_usb_hid_protocol, request, response));
  EXPECT_TRUE(runtime_macro_usb_hid_protocol.set_active);

  memset(request, 0, sizeof(request));
  request[0] = ZMK_RUNTIME_MACRO_PROTOCOL_VERSION;
  request[1] = ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET;
  request[4] = ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT;
  request[5] = 22;
  request[8] = 52;
  for (size_t i = 0; i < 22; i++) {
    request[10 + i] = (uint8_t)(i + 1U);
  }
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(
                  &runtime_macro_usb_hid_protocol, request, response));
  EXPECT_TRUE(runtime_macro_usb_hid_protocol.password_set_active);
  EXPECT_TRUE(zmk_runtime_macro_auth_is_authenticated());

  /* Keep a fresh challenge active as well as the authenticated session. */
  memset(request, 0, sizeof(request));
  request[0] = ZMK_RUNTIME_MACRO_PROTOCOL_VERSION;
  request[1] = ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE;
  request[4] = ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT;
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(
                  &runtime_macro_usb_hid_protocol, request, response));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK, response[3]);

  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = 32,
  };
  int32_t len = 32;
  uint8_t *data = request;

  /* Establish an endpoint-owned response, then queue an older request behind
   * it. A HID event reporting SUSPEND must clear logical state without
   * releasing the still-owned static response buffer. */
  make_list_request(request, 79);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(-EBUSY, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  uint8_t response_snapshot[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
  memcpy(response_snapshot, runtime_macro_usb_hid_tx_response,
         sizeof(response_snapshot));

  make_list_request(request, 80);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  EXPECT_EQ(1, k_msgq_used(&runtime_macro_usb_hid_msgq));

  struct zmk_usb_conn_state_changed event = {
      .conn_state = ZMK_USB_CONN_HID,
  };
  usb_status = USB_DC_SUSPEND;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_online));
  EXPECT_EQ(0, k_msgq_used(&runtime_macro_usb_hid_msgq));
  EXPECT_TRUE(!runtime_macro_usb_hid_protocol.set_active);
  EXPECT_TRUE(!runtime_macro_usb_hid_protocol.password_set_active);
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(-EBUSY, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));

  /* The queued request cannot run or overwrite the endpoint-owned response. */
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_TRUE(memcmp(response_snapshot, runtime_macro_usb_hid_tx_response,
                     sizeof(response_snapshot)) == 0);

  /* RESUME and CLEAR_HALT are also logical reset boundaries, but neither
   * proves that this legacy endpoint stopped owning the response. */
  usb_status = USB_DC_RESUME;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(-EBUSY, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  usb_status = USB_DC_CLEAR_HALT;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(-EBUSY, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));

  /* Only the normal DATA_IN completion may release this transfer. */
  runtime_macro_usb_hid_int_in_ready(&hid1);
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(0, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  k_sem_give(&runtime_macro_usb_hid_in_sem);
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);

  /* The active challenge was cleared along with the session. */
  memset(request, 0, sizeof(request));
  request[0] = ZMK_RUNTIME_MACRO_PROTOCOL_VERSION;
  request[1] = ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE;
  request[4] = ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT;
  request[5] = ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE;
  request[8] = ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE;
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(
                  &runtime_macro_usb_hid_protocol, request, response));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NO_CHALLENGE,
            response[3]);

  /* A request after the completion is allowed to use the recycled buffer. */
  make_list_request(request, 81);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(2, hid_write_calls);
  EXPECT_EQ(81, last_write_data[2]);
}

static void test_configured_and_disconnect_reclaim_in(void) {
  uint8_t request[32];
  uint8_t *data;
  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = 32,
  };
  int32_t len = 32;
  struct zmk_usb_conn_state_changed event = {
      .conn_state = ZMK_USB_CONN_HID,
  };

  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;

  /* CONFIGURED is a safe ownership boundary even when it is the only
   * notification observed after a coalesced RESET/DISCONNECTED sequence. */
  make_list_request(request, 90);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(-EBUSY, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));

  make_list_request(request, 91);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  EXPECT_EQ(1, k_msgq_used(&runtime_macro_usb_hid_msgq));

  /* If the raw status changes while the asynchronous listener enters the
   * boundary, fail closed and retain endpoint ownership rather than recycling
   * the permit from the first sample. */
  const int work_submits_before_unstable = work_submit_calls;
  usb_status = USB_DC_CONFIGURED;
  usb_status_next_read = USB_DC_SUSPEND;
  usb_status_change_on_next_read = true;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(-EBUSY, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  EXPECT_EQ(0, k_msgq_used(&runtime_macro_usb_hid_msgq));
  make_list_request(request, 92);
  data = request;
  EXPECT_EQ(-ENODEV,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  EXPECT_EQ(0, k_msgq_used(&runtime_macro_usb_hid_msgq));
  runtime_macro_usb_hid_int_in_ready(&hid1);
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(0, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  k_sem_give(&runtime_macro_usb_hid_in_sem);
  EXPECT_EQ(work_submits_before_unstable, work_submit_calls);
  run_work_once();

  usb_status = USB_DC_CONFIGURED;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_online));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(0, k_msgq_used(&runtime_macro_usb_hid_msgq));
  EXPECT_EQ(0, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  k_sem_give(&runtime_macro_usb_hid_in_sem);

  make_list_request(request, 93);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(2, hid_write_calls);
  EXPECT_EQ(93, last_write_data[2]);
  runtime_macro_usb_hid_int_in_ready(&hid1);
  run_work_once();

  /* A non-HID logical boundary such as INTERFACE cannot prove endpoint loss.
   * A real DATA_IN may still complete while offline, so it must release the
   * old permit without scheduling work. */
  make_list_request(request, 94);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  const int work_submits_before_interface = work_submit_calls;
  event.conn_state = ZMK_USB_CONN_POWERED;
  usb_status = USB_DC_INTERFACE;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(-EBUSY, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  runtime_macro_usb_hid_int_in_ready(&hid1);
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(0, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  k_sem_give(&runtime_macro_usb_hid_in_sem);
  EXPECT_EQ(work_submits_before_interface, work_submit_calls);
  EXPECT_EQ(3, hid_write_calls);

  /* A stable CONFIGURED/HID notification reopens the transport and permits
   * a new request after the offline DATA_IN completion. */
  usb_status = USB_DC_CONFIGURED;
  event.conn_state = ZMK_USB_CONN_HID;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_online));
  make_list_request(request, 95);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(4, hid_write_calls);
  EXPECT_EQ(95, last_write_data[2]);

  /* RESET and DISCONNECTED are non-HID boundaries that prove endpoint loss
   * and leave the transport offline until a later HID/CONFIGURED event. */
  make_list_request(request, 96);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_in_flight));
  usb_status = USB_DC_RESET;
  event.conn_state = ZMK_USB_CONN_POWERED;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_in_flight));
  EXPECT_EQ(0, k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT));
  k_sem_give(&runtime_macro_usb_hid_in_sem);

  usb_status = USB_DC_DISCONNECTED;
  event.conn_state = ZMK_USB_CONN_NONE;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_in_flight));

  usb_status = USB_DC_CONFIGURED;
  event.conn_state = ZMK_USB_CONN_HID;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_online));
  make_list_request(request, 97);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(5, hid_write_calls);
  EXPECT_EQ(97, last_write_data[2]);
}

static void test_event_raw_mapping_consistency(void) {
  uint8_t request[32];
  uint8_t response[32];
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  uint8_t *data;
  struct zmk_runtime_macro_auth_credential credential = {
      .iterations = ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT,
  };
  struct usb_setup_packet setup = {
      .bRequest = USB_HID_SET_REPORT,
      .wValue = 0x0200,
      .wLength = 32,
  };
  int32_t len = 32;
  struct zmk_usb_conn_state_changed event = {
      .conn_state = ZMK_USB_CONN_HID,
  };

  memset(credential.salt, 0x51, sizeof(credential.salt));
  memset(credential.key, 0x52, sizeof(credential.key));
  memset(proof, 0x5a, sizeof(proof));
  reset_transport();
  runtime_macro_usb_hid_dev = &hid1;
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(
                   nonce, sizeof(nonce)));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));

  /* Leave both protocol transactions and an old request live. */
  memset(request, 0, sizeof(request));
  request[0] = ZMK_RUNTIME_MACRO_PROTOCOL_VERSION;
  request[1] = ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET;
  request[4] = 0;
  request[5] = 2;
  request[8] = 4;
  request[10] = 'a';
  request[11] = 'b';
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(
                  &runtime_macro_usb_hid_protocol, request, response));
  EXPECT_TRUE(runtime_macro_usb_hid_protocol.set_active);

  memset(request, 0, sizeof(request));
  request[0] = ZMK_RUNTIME_MACRO_PROTOCOL_VERSION;
  request[1] = ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET;
  request[4] = ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT;
  request[5] = 22;
  request[8] = 52;
  for (size_t i = 0; i < 22; i++) {
    request[10 + i] = (uint8_t)(i + 1U);
  }
  EXPECT_EQ(0, zmk_runtime_macro_protocol_process(
                  &runtime_macro_usb_hid_protocol, request, response));
  EXPECT_TRUE(runtime_macro_usb_hid_protocol.password_set_active);
  make_list_request(request, 100);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  EXPECT_EQ(1, k_msgq_used(&runtime_macro_usb_hid_msgq));

  /* A stale HID event must not reopen the transport when ERROR maps to
   * POWERED. The logical reset still clears every old operation. */
  usb_status = USB_DC_ERROR;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));
  EXPECT_EQ(0, k_msgq_used(&runtime_macro_usb_hid_msgq));
  EXPECT_TRUE(!runtime_macro_usb_hid_protocol.set_active);
  EXPECT_TRUE(!runtime_macro_usb_hid_protocol.password_set_active);
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());
  data = request;
  EXPECT_EQ(-ENODEV,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));

  /* CONNECTED has the same POWERED mapping and must remain fail-closed. */
  usb_status = USB_DC_CONNECTED;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));
  data = request;
  EXPECT_EQ(-ENODEV,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));

  /* A non-HID event is also rejected when the raw status is CONFIGURED,
   * despite CONFIGURED being a safe endpoint ownership boundary. */
  usb_status = USB_DC_CONFIGURED;
  event.conn_state = ZMK_USB_CONN_POWERED;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(0, atomic_get(&runtime_macro_usb_hid_online));
  data = request;
  EXPECT_EQ(-ENODEV,
            runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));

  /* Only a matching stable CONFIGURED/HID event publishes the transport. */
  event.conn_state = ZMK_USB_CONN_HID;
  EXPECT_EQ(0, runtime_macro_usb_hid_conn_state_listener(
                   (const zmk_event_t *)&event));
  EXPECT_EQ(1, atomic_get(&runtime_macro_usb_hid_online));
  make_list_request(request, 101);
  data = request;
  EXPECT_EQ(0, runtime_macro_usb_hid_set_report(&hid1, &setup, &len, &data));
  run_work_once();
  EXPECT_EQ(1, hid_write_calls);
  EXPECT_EQ(101, last_write_data[2]);
}

static void test_init_failures(void) {
  reset_transport();
  bound_device = NULL;
  EXPECT_EQ(-ENODEV, runtime_macro_usb_hid_init());
  EXPECT_EQ(0, hid_register_calls);

  reset_transport();
  hid1.ready = false;
  EXPECT_EQ(-ENODEV, runtime_macro_usb_hid_init());
  EXPECT_EQ(0, hid_register_calls);
  hid1.ready = true;

  reset_transport();
  hid_init_result = -EIO;
  EXPECT_EQ(-EIO, runtime_macro_usb_hid_init());
  EXPECT_EQ(1, hid_register_calls);
  EXPECT_EQ(1, hid_init_calls);
  EXPECT_TRUE(runtime_macro_usb_hid_dev == NULL);

  reset_transport();
  EXPECT_EQ(0, runtime_macro_usb_hid_init());
  EXPECT_TRUE(runtime_macro_usb_hid_dev == &hid1);
}

int main(void) {
  test_descriptor();
  test_set_report_validation_and_copy();
  test_queue_full_does_not_overwrite();
  test_work_serialization_and_completion();
  test_write_failure_releases_permit();
  test_generation_snapshot_race_and_reconnect();
  test_transport_reset_purges_state_and_recovers();
  test_configured_and_disconnect_reclaim_in();
  test_event_raw_mapping_consistency();
  test_init_failures();

  if (failures != 0) {
    fprintf(stderr, "%d test assertion(s) failed\n", failures);
    return 1;
  }

  puts("runtime macro USB HID tests: PASS");
  return 0;
}
