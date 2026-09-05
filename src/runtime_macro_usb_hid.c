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
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/class/hid.h>
#include <zephyr/usb/class/usb_hid.h>

#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/runtime_macro_auth.h>
#include <zmk/runtime_macro_protocol.h>
#include <zmk/usb.h>

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
  atomic_val_t generation;
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
static K_MUTEX_DEFINE(runtime_macro_usb_hid_transport_mutex);
static atomic_t runtime_macro_usb_hid_generation = ATOMIC_INIT(0);
static atomic_t runtime_macro_usb_hid_in_flight = ATOMIC_INIT(0);
static atomic_t runtime_macro_usb_hid_online = ATOMIC_INIT(1);

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

/*
 * Zephyr 4.1's legacy HID path is hid_int_ep_write() -> usb_write() ->
 * usb_dc_ep_write(), not the generic usb_transfer API. In usb_device.c, the
 * bus-status forwarder calls the generic usb_cancel_transfers() and then
 * disables each configured endpoint before invoking the user status callback;
 * endpoint disable, not generic transfer ownership, invalidates this legacy
 * write. ZMK then raises usb_conn_state_changed asynchronously.
 */
/* Keep this mapping in lockstep with zmk_usb_get_conn_state() in ZMK's
 * app/src/usb.c. The listener event is asynchronous, so its state must agree
 * with the raw status sampled while processing the event before HID is
 * published online. */
static enum zmk_usb_conn_state runtime_macro_usb_hid_conn_state_for_status(
    enum usb_dc_status_code status) {
  switch (status) {
  case USB_DC_SUSPEND:
  case USB_DC_CONFIGURED:
  case USB_DC_RESUME:
  case USB_DC_CLEAR_HALT:
  case USB_DC_SOF:
    return ZMK_USB_CONN_HID;
  case USB_DC_DISCONNECTED:
  case USB_DC_UNKNOWN:
    return ZMK_USB_CONN_NONE;
  default:
    return ZMK_USB_CONN_POWERED;
  }
}

static bool runtime_macro_usb_hid_status_reclaims_in(
    enum usb_dc_status_code status) {
  switch (status) {
  case USB_DC_RESET:
  case USB_DC_DISCONNECTED:
  case USB_DC_CONFIGURED:
    /* RESET/DISCONNECTED disable the old endpoint. CONFIGURED is the
     * completion of a new configuration, after the old endpoint teardown. */
    return true;
  case USB_DC_SUSPEND:
  case USB_DC_RESUME:
  case USB_DC_CLEAR_HALT:
  case USB_DC_CONNECTED:
  case USB_DC_INTERFACE:
  case USB_DC_SET_HALT:
  case USB_DC_ERROR:
  case USB_DC_UNKNOWN:
  case USB_DC_SOF:
  default:
    /* These statuses do not prove that the legacy IN endpoint stopped owning
     * the static response buffer. Wait for DATA_IN or a later safe boundary. */
    return false;
  }
}

static void runtime_macro_usb_hid_reclaim_in_locked(void) {
  atomic_set(&runtime_macro_usb_hid_in_flight, 0);
  k_sem_reset(&runtime_macro_usb_hid_in_sem);
  k_sem_give(&runtime_macro_usb_hid_in_sem);
}

static void runtime_macro_usb_hid_transport_reset(
    enum zmk_usb_conn_state event_conn_state,
    enum usb_dc_status_code status_before) {
  k_mutex_lock(&runtime_macro_usb_hid_transport_mutex, K_FOREVER);

  /* Keep the transport offline throughout the logical reset. In particular,
   * do not publish HID before auth/protocol state and old requests are gone. */
  atomic_set(&runtime_macro_usb_hid_online, 0);
  atomic_inc(&runtime_macro_usb_hid_generation);
  zmk_runtime_macro_auth_transport_reset();
  zmk_runtime_macro_protocol_discard(&runtime_macro_usb_hid_protocol);
  k_msgq_purge(&runtime_macro_usb_hid_msgq);

  /* usb_status is updated outside this mutex by ZMK's USB status callback.
   * Require the status to be unchanged while entering this boundary. If the
   * asynchronous notification raced another status update, do not reuse a
   * potentially endpoint-owned response buffer early or publish HID. */
  const enum usb_dc_status_code status_after = zmk_usb_get_status();
  const bool status_stable = status_before == status_after;
  const bool event_matches_raw =
      status_stable &&
      runtime_macro_usb_hid_conn_state_for_status(status_after) ==
          event_conn_state;
  if (status_stable &&
      runtime_macro_usb_hid_status_reclaims_in(status_after)) {
    runtime_macro_usb_hid_reclaim_in_locked();
  }

  /* A stable raw status is not enough to publish HID: an asynchronous event
   * may describe an older status. Require the event/raw mapping to agree and
   * fail closed on either instability or a stale event. */
  if (event_matches_raw && event_conn_state == ZMK_USB_CONN_HID) {
    atomic_set(&runtime_macro_usb_hid_online, 1);
  }

  k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);
}

static int runtime_macro_usb_hid_conn_state_listener(const zmk_event_t *eh) {
  struct zmk_usb_conn_state_changed *event =
      as_zmk_usb_conn_state_changed(eh);
  if (event == NULL) {
    return -ENOTSUP;
  }

  /* ZMK coalesces USB status notifications in one work item. Treat every
   * notification as a conservative logical boundary, including HID, so a
   * reset/disconnect followed quickly by CONFIGURED cannot preserve a stale
   * management session or request. Endpoint ownership is handled separately
   * from that logical reset using the raw USB status snapshot. HID is
   * published online only after a stable status sample and complete reset
   * while holding the mutex. */
  const enum usb_dc_status_code status_before = zmk_usb_get_status();
  runtime_macro_usb_hid_transport_reset(event->conn_state, status_before);

  return 0;
}

ZMK_LISTENER(runtime_macro_usb_hid, runtime_macro_usb_hid_conn_state_listener);
ZMK_SUBSCRIPTION(runtime_macro_usb_hid, zmk_usb_conn_state_changed);

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

  /* Sample the epoch before checking online. The lifecycle reset stores
   * online=0, increments the epoch, and purges the queue in that order. If a
   * reset interleaves after this sample, this request carries the old epoch
   * and the work consumer drops it; if it interleaves before the online check,
   * this callback is normally rejected without blocking on the lifecycle
   * mutex. Even if a reconnect publishes HID before that check, the sampled
   * epoch is still stale and the work consumer drops the request. */
  const atomic_val_t generation =
      atomic_get(&runtime_macro_usb_hid_generation);
  if (!atomic_get(&runtime_macro_usb_hid_online)) {
    return -ENODEV;
  }

#if defined(ZMK_RUNTIME_MACRO_USB_HID_TEST)
  /* Test-only deterministic interleaving point; absent from production. */
  extern void runtime_macro_usb_hid_test_after_online_check(void);
  runtime_macro_usb_hid_test_after_online_check();
#endif

  struct runtime_macro_usb_hid_request request;
  memcpy(request.frame, *data, sizeof(request.frame));
  request.generation = generation;

  /* The queue is thread-safe; do not block a USB control callback on the
   * lifecycle mutex. The work consumer drops anything queued while offline. */
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

  /* The legacy HID callback has no transfer-generation argument. In Zephyr
   * 4.1, usb_device.c disables the endpoint before forwarding RESET or
   * DISCONNECTED, and hid_int_in forwards only DATA_IN completions. The
   * lifecycle listener recycles the permit only for those boundaries (and
   * CONFIGURED); SUSPEND/RESUME/CLEAR_HALT retain it until DATA_IN. While
   * offline, a real DATA_IN still releases the old buffer permit, but must
   * not schedule work. Endpoint-disable boundaries already set in_flight=0,
   * so their cancellation/late callback does not pass this CAS. The CAS also
   * ignores callbacks when no transfer is in flight. */
  if (!atomic_cas(&runtime_macro_usb_hid_in_flight, 1, 0)) {
    return;
  }

  k_sem_give(&runtime_macro_usb_hid_in_sem);
  if (!atomic_get(&runtime_macro_usb_hid_online)) {
    return;
  }

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

  k_mutex_lock(&runtime_macro_usb_hid_transport_mutex, K_FOREVER);
  const atomic_val_t generation =
      atomic_get(&runtime_macro_usb_hid_generation);

  if (!atomic_get(&runtime_macro_usb_hid_online)) {
    k_msgq_purge(&runtime_macro_usb_hid_msgq);
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);
    return;
  }

  struct runtime_macro_usb_hid_request request;
  if (k_msgq_get(&runtime_macro_usb_hid_msgq, &request, K_NO_WAIT) != 0) {
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);
    return;
  }

  if (request.generation != generation) {
    /* A stale control callback can race the lifecycle purge. Drop its frame
     * even if it reached the queue after reset and before reconnect. */
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);
    (void)k_work_submit(&runtime_macro_usb_hid_work);
    return;
  }

  if (runtime_macro_usb_hid_dev == NULL) {
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);
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

  /* The lifecycle reset takes this mutex, so the protocol context and queue
   * cannot be discarded while a request is being processed or submitted.
   * Mark the transfer before calling into USB so an implementation that
   * completes synchronously cannot race the completion callback. */
  atomic_set(&runtime_macro_usb_hid_in_flight, 1);
  err = hid_int_ep_write(runtime_macro_usb_hid_dev,
                         runtime_macro_usb_hid_tx_response,
                         sizeof(runtime_macro_usb_hid_tx_response), NULL);
  if (err != 0) {
    atomic_set(&runtime_macro_usb_hid_in_flight, 0);
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);
    LOG_ERR("Runtime macro USB HID IN write failed (err %d)", err);
    return;
  }

  if (atomic_get(&runtime_macro_usb_hid_generation) != generation) {
    /* Defensive: a future transport implementation may reset without taking
     * the mutex. Never publish a response from an older connection. */
    atomic_set(&runtime_macro_usb_hid_in_flight, 0);
    k_sem_give(&runtime_macro_usb_hid_in_sem);
    k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);
    return;
  }

  k_mutex_unlock(&runtime_macro_usb_hid_transport_mutex);

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

  k_msgq_purge(&runtime_macro_usb_hid_msgq);
  atomic_set(&runtime_macro_usb_hid_generation, 0);
  atomic_set(&runtime_macro_usb_hid_in_flight, 0);
  atomic_set(&runtime_macro_usb_hid_online, 1);
  k_sem_reset(&runtime_macro_usb_hid_in_sem);
  k_sem_give(&runtime_macro_usb_hid_in_sem);
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
