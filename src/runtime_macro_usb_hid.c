/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/runtime_macro_protocol.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if CONFIG_USB_HID_DEVICE_COUNT < 2
#error "Runtime Macro USB HID requires CONFIG_USB_HID_DEVICE_COUNT >= 2"
#endif

#if CONFIG_HID_INTERRUPT_EP_MPS < ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE
#error "Runtime Macro USB HID requires CONFIG_HID_INTERRUPT_EP_MPS >= 32"
#endif

#if defined(CONFIG_ENABLE_HID_INT_OUT_EP) && CONFIG_ENABLE_HID_INT_OUT_EP
#error "Runtime Macro USB HID must not enable CONFIG_ENABLE_HID_INT_OUT_EP"
#endif

#define RUNTIME_MACRO_USB_HID_QUEUE_DEPTH 4U
#define RUNTIME_MACRO_USB_HID_REPORT_TYPE_OUTPUT 0x0200U
#define RUNTIME_MACRO_USB_HID_REPORT_ID 0U

struct runtime_macro_usb_hid_request {
  uint8_t frame[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];
};

/*
 * The descriptor intentionally uses raw bytes. This makes the vendor usage
 * page, report sizes, and absence of a Report ID part of the reviewed wire
 * contract rather than an accidental consequence of a helper macro.
 */
static const uint8_t runtime_macro_usb_hid_report_desc[] = {
    0x06, 0x60, 0xff, /* Usage Page (Vendor 0xff60) */
    0x09, 0x61,       /* Usage (Runtime Macro application) */
    0xa1, 0x01,       /* Collection (Application) */
    0x09, 0x62,       /* Usage (Input data) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x26, 0xff, 0x00, /* Logical Maximum (255) */
    0x75, 0x08,       /* Report Size (8 bits) */
    0x95, 0x20,       /* Report Count (32 bytes) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0x09, 0x63,       /* Usage (Output data) */
    0x91, 0x02,       /* Output (Data, Variable, Absolute) */
    0xc0,             /* End Collection */
};

static const struct device *runtime_macro_usb_hid_dev;
static struct zmk_runtime_macro_protocol runtime_macro_usb_hid_protocol;

K_MSGQ_DEFINE(runtime_macro_usb_hid_msgq,
              sizeof(struct runtime_macro_usb_hid_request),
              RUNTIME_MACRO_USB_HID_QUEUE_DEPTH, 4);
static K_SEM_DEFINE(runtime_macro_usb_hid_in_sem, 1, 1);

/*
 * hid_int_ep_write() may retain this pointer until int_in_ready is called.
 * Keep the response in static storage and hold the IN semaphore for the whole
 * transfer so a later work invocation cannot overwrite an in-flight report.
 */
static uint8_t
    runtime_macro_usb_hid_tx_response[ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE];

static void runtime_macro_usb_hid_work_handler(struct k_work *work);
K_WORK_DEFINE(runtime_macro_usb_hid_work, runtime_macro_usb_hid_work_handler);

static int runtime_macro_usb_hid_set_report(const struct device *dev,
                                            struct usb_setup_packet *setup,
                                            int32_t *len, uint8_t **data) {
  ARG_UNUSED(dev);

  if (setup == NULL || len == NULL || data == NULL || *data == NULL ||
      setup->bRequest != USB_HID_SET_REPORT ||
      (setup->wValue & 0xff00U) != RUNTIME_MACRO_USB_HID_REPORT_TYPE_OUTPUT ||
      (setup->wValue & 0x00ffU) != RUNTIME_MACRO_USB_HID_REPORT_ID ||
      *len != (int32_t)ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE) {
    return -EINVAL;
  }

  struct runtime_macro_usb_hid_request request;
  memcpy(request.frame, *data, sizeof(request.frame));

  if (k_msgq_put(&runtime_macro_usb_hid_msgq, &request, K_NO_WAIT) != 0) {
    return -ENOSPC;
  }

  int err = k_work_submit(&runtime_macro_usb_hid_work);
  if (err < 0) {
    LOG_ERR("Failed to schedule runtime macro USB HID work (err %d)", err);
    return err;
  }

  return 0;
}

static void runtime_macro_usb_hid_int_in_ready(const struct device *dev) {
  ARG_UNUSED(dev);

  k_sem_give(&runtime_macro_usb_hid_in_sem);
  int err = k_work_submit(&runtime_macro_usb_hid_work);
  if (err < 0) {
    LOG_ERR("Failed to reschedule runtime macro USB HID work (err %d)", err);
  }
}

static void runtime_macro_usb_hid_init_error_response(const uint8_t *request,
                                                      uint8_t *response) {
  memset(response, 0, ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE);
  response[ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET];
  response[ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET];
  response[ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET];
  response[ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET] =
      ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL;
  response[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET];
}

static void runtime_macro_usb_hid_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  /* Never wait on the system workqueue for an endpoint transfer. */
  if (k_sem_take(&runtime_macro_usb_hid_in_sem, K_NO_WAIT) != 0) {
    return;
  }

  struct runtime_macro_usb_hid_request request;
  if (k_msgq_get(&runtime_macro_usb_hid_msgq, &request, K_NO_WAIT) != 0) {
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    return;
  }

  if (runtime_macro_usb_hid_dev == NULL) {
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    LOG_ERR("Runtime macro USB HID device is unavailable");
    return;
  }

  int err = zmk_runtime_macro_protocol_process(
      &runtime_macro_usb_hid_protocol, request.frame,
      runtime_macro_usb_hid_tx_response);
  if (err != 0) {
    runtime_macro_usb_hid_init_error_response(
        request.frame, runtime_macro_usb_hid_tx_response);
    LOG_ERR("Runtime macro protocol processing failed (err %d)", err);
  }

  err = hid_int_ep_write(runtime_macro_usb_hid_dev,
                         runtime_macro_usb_hid_tx_response,
                         sizeof(runtime_macro_usb_hid_tx_response), NULL);
  if (err != 0) {
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    LOG_ERR("Runtime macro USB HID IN write failed (err %d)", err);
    return;
  }

  /* The IN-ready callback returns this permit after the transfer completes. */
}

static const struct hid_ops runtime_macro_usb_hid_ops = {
    .set_report = runtime_macro_usb_hid_set_report,
    .int_in_ready = runtime_macro_usb_hid_int_in_ready,
};

static int runtime_macro_usb_hid_init(void) {
  const struct device *hid_dev =
      device_get_binding(CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE);
  if (hid_dev == NULL) {
    LOG_ERR("Unable to locate runtime macro USB HID device %s",
            CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE);
    return -ENODEV;
  }

  if (!device_is_ready(hid_dev)) {
    LOG_ERR("Runtime macro USB HID device %s is not ready",
            CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE);
    return -ENODEV;
  }

  zmk_runtime_macro_protocol_init(&runtime_macro_usb_hid_protocol);
  usb_hid_register_device(hid_dev, runtime_macro_usb_hid_report_desc,
                          sizeof(runtime_macro_usb_hid_report_desc),
                          &runtime_macro_usb_hid_ops);

  int err = usb_hid_init(hid_dev);
  if (err != 0) {
    LOG_ERR("Unable to initialize runtime macro USB HID (err %d)", err);
    return err;
  }

  runtime_macro_usb_hid_dev = hid_dev;
  return 0;
}

SYS_INIT(runtime_macro_usb_hid_init, APPLICATION,
         CONFIG_ZMK_USB_HID_INIT_PRIORITY);
