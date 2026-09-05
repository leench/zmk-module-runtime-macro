#pragma once

#include <zmk/event_manager.h>
#include <zmk/usb.h>

struct zmk_usb_conn_state_changed {
  enum zmk_usb_conn_state conn_state;
};

static inline struct zmk_usb_conn_state_changed *
as_zmk_usb_conn_state_changed(const zmk_event_t *eh) {
  return (struct zmk_usb_conn_state_changed *)eh;
}
