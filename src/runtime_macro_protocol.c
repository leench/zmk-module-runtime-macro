/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zmk/runtime_macro.h>
#include <zmk/runtime_macro_protocol.h>

static uint16_t runtime_macro_protocol_get_u16(const uint8_t *frame,
                                               size_t offset) {
  return (uint16_t)frame[offset] | ((uint16_t)frame[offset + 1U] << 8);
}

static void runtime_macro_protocol_put_u16(uint8_t *frame, size_t offset,
                                           uint16_t value) {
  frame[offset] = (uint8_t)value;
  frame[offset + 1U] = (uint8_t)(value >> 8);
}

static void
runtime_macro_protocol_clear_set(struct zmk_runtime_macro_protocol *protocol) {
  protocol->set_active = false;
  protocol->set_request_id = 0;
  protocol->set_slot = 0;
  protocol->set_total_length = 0;
  protocol->set_received_length = 0;
  memset(protocol->set_staging, 0, sizeof(protocol->set_staging));
}

static void runtime_macro_protocol_init_response(const uint8_t *request,
                                                 uint8_t *response) {
  memset(response, 0, ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE);

  if (request == NULL) {
    return;
  }

  response[ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET];
  response[ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET];
  response[ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET];
  response[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET];
}

static void runtime_macro_protocol_set_status(
    uint8_t *response, enum zmk_runtime_macro_protocol_status status) {
  response[ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET] = (uint8_t)status;
}

static bool
runtime_macro_protocol_payload_tail_is_zero(const uint8_t *request,
                                            uint8_t payload_length) {
  for (size_t i =
           (size_t)ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET + payload_length;
       i < ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE; i++) {
    if (request[i] != 0U) {
      return false;
    }
  }

  return true;
}

static bool runtime_macro_protocol_slot_is_valid(uint8_t slot) {
  return slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT;
}

static bool
runtime_macro_protocol_request_has_empty_payload(const uint8_t *request) {
  return request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] == 0U &&
         runtime_macro_protocol_get_u16(
             request, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET) == 0U;
}

static void runtime_macro_protocol_set_chunk_response(uint8_t *response,
                                                      uint16_t offset,
                                                      uint16_t total_length) {
  runtime_macro_protocol_put_u16(
      response, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET, offset);
  runtime_macro_protocol_put_u16(
      response, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET, total_length);
}

static void runtime_macro_protocol_set_error(
    uint8_t *response, enum zmk_runtime_macro_protocol_status status) {
  runtime_macro_protocol_set_status(response, status);
  runtime_macro_protocol_put_u16(response,
                                 ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET, 0U);
  runtime_macro_protocol_put_u16(
      response, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET, 0U);
  response[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] = 0U;
  memset(response + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET, 0,
         ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE -
             ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET);
}

static void runtime_macro_protocol_set_success(uint8_t *response,
                                               uint16_t offset,
                                               uint16_t total_length) {
  runtime_macro_protocol_set_status(response,
                                    ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK);
  runtime_macro_protocol_set_chunk_response(response, offset, total_length);
}

static int runtime_macro_protocol_process_list(const uint8_t *request,
                                               uint8_t *response) {
  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] !=
      ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  if (!runtime_macro_protocol_request_has_empty_payload(request)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (!runtime_macro_protocol_payload_tail_is_zero(request, 0U)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  uint8_t list_data[1U + (2U * CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT)];
  memset(list_data, 0, sizeof(list_data));
  list_data[0] = CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT;

  for (uint8_t slot = 0; slot < CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT; slot++) {
    size_t length;
    if (zmk_runtime_macro_slot_get_length(slot, &length) != 0 ||
        length > UINT16_MAX) {
      runtime_macro_protocol_set_error(
          response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL);
      return 0;
    }

    runtime_macro_protocol_put_u16(list_data, 1U + (2U * slot),
                                   (uint16_t)length);
  }

  const uint16_t total_length = (uint16_t)sizeof(list_data);
  const uint16_t offset = runtime_macro_protocol_get_u16(
      request, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET);
  if (offset > total_length) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
    return 0;
  }

  const uint16_t remaining = total_length - offset;
  const uint8_t payload_length =
      remaining > ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE
          ? ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE
          : (uint8_t)remaining;
  if (payload_length > 0U) {
    memcpy(response + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
           list_data + offset, payload_length);
  }
  response[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] = payload_length;
  runtime_macro_protocol_set_success(response, offset, total_length);
  return 0;
}

static int runtime_macro_protocol_process_get(const uint8_t *request,
                                              uint8_t *response) {
  const uint8_t slot = request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET];
  if (!runtime_macro_protocol_slot_is_valid(slot)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT);
    return 0;
  }

  if (!runtime_macro_protocol_request_has_empty_payload(request)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (!runtime_macro_protocol_payload_tail_is_zero(request, 0U)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  char snapshot[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN + 1U];
  size_t length;
  if (zmk_runtime_macro_slot_copy(slot, snapshot, sizeof(snapshot), &length) !=
          0 ||
      length > UINT16_MAX) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL);
    return 0;
  }

  const uint16_t total_length = (uint16_t)length;
  const uint16_t offset = runtime_macro_protocol_get_u16(
      request, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET);
  if (offset > total_length) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
    return 0;
  }

  const uint16_t remaining = total_length - offset;
  const uint8_t payload_length =
      remaining > ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE
          ? ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE
          : (uint8_t)remaining;
  if (payload_length > 0U) {
    memcpy(response + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
           snapshot + offset, payload_length);
  }
  response[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] = payload_length;
  runtime_macro_protocol_set_success(response, offset, total_length);
  return 0;
}

static bool
runtime_macro_protocol_set_payload_is_valid(const uint8_t *request,
                                            uint8_t payload_length) {
  for (uint8_t i = 0; i < payload_length; i++) {
    uint32_t encoded;
    if (zmk_runtime_macro_ascii_to_keycode(
            request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET + i], &encoded) !=
        0) {
      return false;
    }
  }

  return true;
}

static int
runtime_macro_protocol_commit_set(struct zmk_runtime_macro_protocol *protocol,
                                  uint8_t *response) {
  int err = zmk_runtime_macro_slot_set(
      protocol->set_slot, protocol->set_staging, protocol->set_total_length);
  const uint16_t total_length = protocol->set_total_length;
  const uint16_t next_offset = protocol->set_received_length;
  runtime_macro_protocol_clear_set(protocol);

  if (err == 0) {
    runtime_macro_protocol_set_success(response, next_offset, total_length);
  } else if (err == -EINVAL) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INVALID_TEXT);
  } else {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR);
  }

  return 0;
}

static int
runtime_macro_protocol_process_set(struct zmk_runtime_macro_protocol *protocol,
                                   const uint8_t *request, uint8_t *response) {
  const uint8_t slot = request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET];
  const uint8_t request_id =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET];
  const uint8_t payload_length =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET];
  const uint16_t offset = runtime_macro_protocol_get_u16(
      request, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET);
  const uint16_t total_length = runtime_macro_protocol_get_u16(
      request, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET);

  /* Any invalid SET request starts with no usable transaction. */
  if (payload_length > ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE ||
      !runtime_macro_protocol_payload_tail_is_zero(request, payload_length)) {
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_set_error(
        response, payload_length > ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE
                      ? ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH
                      : ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  if (!runtime_macro_protocol_slot_is_valid(slot)) {
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT);
    return 0;
  }

  if (total_length > CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN) {
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (offset > total_length) {
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
    return 0;
  }

  if (payload_length > (uint16_t)(total_length - offset)) {
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (total_length != 0U && payload_length == 0U) {
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (!runtime_macro_protocol_set_payload_is_valid(request, payload_length)) {
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INVALID_TEXT);
    return 0;
  }

  if (offset == 0U) {
    runtime_macro_protocol_clear_set(protocol);
    protocol->set_active = true;
    protocol->set_request_id = request_id;
    protocol->set_slot = slot;
    protocol->set_total_length = total_length;
    protocol->set_received_length = payload_length;
    if (payload_length > 0U) {
      memcpy(protocol->set_staging,
             request + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
             payload_length);
    }
  } else {
    if (!protocol->set_active || protocol->set_request_id != request_id ||
        protocol->set_slot != slot ||
        protocol->set_total_length != total_length) {
      runtime_macro_protocol_clear_set(protocol);
      runtime_macro_protocol_set_error(
          response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
      return 0;
    }

    if (offset != protocol->set_received_length) {
      runtime_macro_protocol_clear_set(protocol);
      runtime_macro_protocol_set_error(
          response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
      return 0;
    }

    if (payload_length > 0U) {
      memcpy(protocol->set_staging + offset,
             request + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
             payload_length);
    }
    protocol->set_received_length += payload_length;
  }

  if (protocol->set_received_length == protocol->set_total_length) {
    return runtime_macro_protocol_commit_set(protocol, response);
  }

  runtime_macro_protocol_set_success(response, protocol->set_received_length,
                                     protocol->set_total_length);
  return 0;
}

static int runtime_macro_protocol_process_clear(const uint8_t *request,
                                                uint8_t *response) {
  const uint8_t slot = request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET];
  if (!runtime_macro_protocol_slot_is_valid(slot)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT);
    return 0;
  }

  if (!runtime_macro_protocol_request_has_empty_payload(request) ||
      runtime_macro_protocol_get_u16(
          request, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET) != 0U ||
      !runtime_macro_protocol_payload_tail_is_zero(request, 0U)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  if (zmk_runtime_macro_slot_clear(slot) != 0) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR);
    return 0;
  }

  runtime_macro_protocol_set_success(response, 0U, 0U);
  return 0;
}

void zmk_runtime_macro_protocol_init(
    struct zmk_runtime_macro_protocol *protocol) {
  if (protocol != NULL) {
    runtime_macro_protocol_clear_set(protocol);
  }
}

int zmk_runtime_macro_protocol_process(
    struct zmk_runtime_macro_protocol *protocol, const uint8_t *request,
    uint8_t *response) {
  if (response == NULL) {
    return -EINVAL;
  }

  runtime_macro_protocol_init_response(request, response);
  if (request == NULL) {
    return -EINVAL;
  }

  if (protocol == NULL) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL);
    return 0;
  }

  const uint8_t opcode = request[ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET];
  if (opcode == ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET) {
    /* A malformed SET must not leave an earlier transaction usable. */
    if (request[ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET] !=
            ZMK_RUNTIME_MACRO_PROTOCOL_VERSION ||
        request[ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET] != 0U) {
      runtime_macro_protocol_clear_set(protocol);
    }
  }

  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET] !=
      ZMK_RUNTIME_MACRO_PROTOCOL_VERSION) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
    return 0;
  }

  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET] != 0U) {
    if (opcode == ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET) {
      runtime_macro_protocol_clear_set(protocol);
    }
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  switch (opcode) {
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST:
    return runtime_macro_protocol_process_list(request, response);
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET:
    return runtime_macro_protocol_process_get(request, response);
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET:
    return runtime_macro_protocol_process_set(protocol, request, response);
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR:
    return runtime_macro_protocol_process_clear(request, response);
  default:
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OPCODE);
    return 0;
  }
}
