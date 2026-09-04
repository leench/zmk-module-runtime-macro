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

#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT 2
#define CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN 8
#define CONFIG_ZMK_MACRO_DEFAULT_TAP_MS 10
#define CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS 20
#define CONFIG_USB_HID_DEVICE_COUNT 2
#define CONFIG_HID_INTERRUPT_EP_MPS 32
#define CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE "HID_1"
#define CONFIG_ZMK_USB_HID_INIT_PRIORITY 95

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
static struct device hid1 = {.name = "HID_1", .ready = true};

#include "../../src/runtime_macro.c"
#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro_protocol.c"
#include "../../src/runtime_macro_usb_hid.c"

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
  reset_backend();
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
  memset(last_write_data, 0, sizeof(last_write_data));
  memset(runtime_macro_usb_hid_tx_response, 0,
         sizeof(runtime_macro_usb_hid_tx_response));
  k_msgq_purge(&runtime_macro_usb_hid_msgq);
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
  test_init_failures();

  if (failures != 0) {
    fprintf(stderr, "%d test assertion(s) failed\n", failures);
    return 1;
  }

  puts("runtime macro USB HID tests: PASS");
  return 0;
}
