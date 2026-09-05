#pragma once

/* Keep the public ZMK USB status values aligned with Zephyr 4.1. */
enum usb_dc_status_code {
  USB_DC_ERROR,
  USB_DC_RESET,
  USB_DC_CONNECTED,
  USB_DC_CONFIGURED,
  USB_DC_DISCONNECTED,
  USB_DC_SUSPEND,
  USB_DC_RESUME,
  USB_DC_INTERFACE,
  USB_DC_SET_HALT,
  USB_DC_CLEAR_HALT,
  USB_DC_SOF,
  USB_DC_UNKNOWN,
};

enum zmk_usb_conn_state {
  ZMK_USB_CONN_NONE,
  ZMK_USB_CONN_POWERED,
  ZMK_USB_CONN_HID,
};

enum usb_dc_status_code zmk_usb_get_status(void);
