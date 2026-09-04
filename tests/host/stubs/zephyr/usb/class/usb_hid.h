#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/usb/usb_ch9.h>

typedef int (*hid_cb_t)(const struct device *dev,
                        struct usb_setup_packet *setup, int32_t *len,
                        uint8_t **data);
typedef void (*hid_int_ready_callback)(const struct device *dev);

struct hid_ops {
  hid_cb_t get_report;
  hid_cb_t set_report;
  void *protocol_change;
  void *on_idle;
  hid_int_ready_callback int_in_ready;
  hid_int_ready_callback int_out_ready;
};

extern void usb_hid_register_device(const struct device *dev,
                                    const uint8_t *desc, size_t size,
                                    const struct hid_ops *ops);
extern int usb_hid_init(const struct device *dev);
extern int hid_int_ep_write(const struct device *dev, const uint8_t *data,
                            uint32_t data_len, uint32_t *bytes_ret);
