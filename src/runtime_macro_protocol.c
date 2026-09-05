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
#include <zmk/runtime_macro_auth.h>
#include <zmk/runtime_macro_protocol.h>

#define RUNTIME_MACRO_PROTOCOL_AUTH_INFO_LENGTH 22U
#define RUNTIME_MACRO_PROTOCOL_AUTH_FLAGS_CONFIGURED (1U << 0)
#define RUNTIME_MACRO_PROTOCOL_AUTH_FLAGS_SESSION (1U << 1)

static uint16_t runtime_macro_protocol_get_u16(const uint8_t *frame,
                                               size_t offset) {
  return (uint16_t)frame[offset] | ((uint16_t)frame[offset + 1U] << 8);
}

static void runtime_macro_protocol_put_u16(uint8_t *frame, size_t offset,
                                           uint16_t value) {
  frame[offset] = (uint8_t)value;
  frame[offset + 1U] = (uint8_t)(value >> 8);
}

static uint32_t runtime_macro_protocol_get_u32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void runtime_macro_protocol_zeroize(void *data, size_t length) {
  volatile uint8_t *bytes = data;

  while (length > 0U) {
    *bytes++ = 0U;
    length--;
  }
}

static void
runtime_macro_protocol_clear_set(struct zmk_runtime_macro_protocol *protocol) {
  protocol->set_active = false;
  protocol->set_request_id = 0U;
  protocol->set_slot = 0U;
  protocol->set_total_length = 0U;
  protocol->set_received_length = 0U;
  runtime_macro_protocol_zeroize(protocol->set_staging,
                                 sizeof(protocol->set_staging));
}

static void runtime_macro_protocol_clear_password_set(
    struct zmk_runtime_macro_protocol *protocol) {
  protocol->password_set_active = false;
  protocol->password_set_request_id = 0U;
  protocol->password_set_total_length = 0U;
  protocol->password_set_received_length = 0U;
  runtime_macro_protocol_zeroize(protocol->password_set_staging,
                                 sizeof(protocol->password_set_staging));
}

void zmk_runtime_macro_protocol_discard(
    struct zmk_runtime_macro_protocol *protocol) {
  if (protocol == NULL) {
    return;
  }

  runtime_macro_protocol_zeroize(protocol, sizeof(*protocol));
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

static bool runtime_macro_protocol_opcode_is_known(uint8_t opcode) {
  switch (opcode) {
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK:
    return true;
  default:
    return false;
  }
}

static bool runtime_macro_protocol_is_set_opcode(uint8_t opcode) {
  return opcode == ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET;
}

static bool runtime_macro_protocol_is_password_set_opcode(uint8_t opcode) {
  return opcode == ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET;
}

static void runtime_macro_protocol_discard_for_opcode(
    struct zmk_runtime_macro_protocol *protocol, uint8_t opcode) {
  if (protocol == NULL) {
    return;
  }

  if (runtime_macro_protocol_is_set_opcode(opcode)) {
    runtime_macro_protocol_clear_set(protocol);
  } else if (runtime_macro_protocol_is_password_set_opcode(opcode)) {
    runtime_macro_protocol_clear_password_set(protocol);
  }
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
  runtime_macro_protocol_put_u16(
      response, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET, 0U);
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
  runtime_macro_protocol_set_status(
      response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK);
  runtime_macro_protocol_set_chunk_response(response, offset, total_length);
}

static bool runtime_macro_protocol_request_has_empty_object(
    const uint8_t *request) {
  return request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] == 0U &&
         runtime_macro_protocol_get_u16(
             request, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET) == 0U &&
         runtime_macro_protocol_get_u16(
             request, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET) == 0U;
}

/*
 * This is deliberately called before opcode-specific validation. It is the
 * common gate for every protected management command, so an unauthenticated
 * request cannot learn slot, offset, or text validation details.
 */
static enum zmk_runtime_macro_protocol_status
runtime_macro_protocol_management_access(
    struct zmk_runtime_macro_protocol *protocol) {
  enum zmk_runtime_macro_auth_state state =
      zmk_runtime_macro_auth_get_state();

  if (state == ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED) {
    zmk_runtime_macro_protocol_discard(protocol);
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID;
  }

  if (state == ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED &&
      !zmk_runtime_macro_auth_is_authenticated()) {
    zmk_runtime_macro_protocol_discard(protocol);
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED;
  }

  return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK;
}

static bool runtime_macro_protocol_refresh_protected_session(
    struct zmk_runtime_macro_protocol *protocol, uint8_t *response) {
  if (zmk_runtime_macro_auth_get_state() !=
      ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED) {
    return true;
  }

  if (zmk_runtime_macro_auth_refresh_session() != 0) {
    zmk_runtime_macro_protocol_discard(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);
    return false;
  }

  return true;
}

static void runtime_macro_protocol_sync_auth_state(
    struct zmk_runtime_macro_protocol *protocol) {
  enum zmk_runtime_macro_auth_state state =
      zmk_runtime_macro_auth_get_state();
  bool session_authenticated =
      state == ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED &&
      zmk_runtime_macro_auth_is_authenticated();

  if (protocol->auth_state_known &&
      (protocol->auth_state != (uint8_t)state ||
       protocol->auth_session_authenticated != session_authenticated)) {
    /* This covers a lazy session timeout, AUTH_PROVE success, credential
     * replacement, settings reload, and ERROR_LOCKED transition. */
    runtime_macro_protocol_clear_set(protocol);
    runtime_macro_protocol_clear_password_set(protocol);
  }

  protocol->auth_state_known = true;
  protocol->auth_state = (uint8_t)state;
  protocol->auth_session_authenticated = session_authenticated;
}

static int runtime_macro_protocol_process_list(const uint8_t *request,
                                               uint8_t *response) {
  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] !=
      ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] != 0U ||
      runtime_macro_protocol_get_u16(
          request, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET) != 0U) {
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

  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] != 0U ||
      runtime_macro_protocol_get_u16(
          request, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET) != 0U) {
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

  if (!runtime_macro_protocol_request_has_empty_object(request) ||
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

static int runtime_macro_protocol_process_auth_info(const uint8_t *request,
                                                    uint8_t *response) {
  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] !=
          ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT ||
      !runtime_macro_protocol_request_has_empty_object(request)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  struct zmk_runtime_macro_auth_info info;
  if (zmk_runtime_macro_auth_get_info(&info) != 0) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL);
    return 0;
  }

  if (info.state == ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
    return 0;
  }

  uint8_t *payload =
      response + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET;
  payload[0] = info.state == ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED
                   ? RUNTIME_MACRO_PROTOCOL_AUTH_FLAGS_CONFIGURED
                   : 0U;
  if (info.session_authenticated) {
    payload[0] |= RUNTIME_MACRO_PROTOCOL_AUTH_FLAGS_SESSION;
  }
  payload[1] = info.kdf_id;
  payload[2] = (uint8_t)info.iterations;
  payload[3] = (uint8_t)(info.iterations >> 8);
  payload[4] = (uint8_t)(info.iterations >> 16);
  payload[5] = (uint8_t)(info.iterations >> 24);
  memcpy(payload + 6U, info.salt, sizeof(info.salt));
  response[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] =
      RUNTIME_MACRO_PROTOCOL_AUTH_INFO_LENGTH;
  runtime_macro_protocol_set_success(
      response, 0U, RUNTIME_MACRO_PROTOCOL_AUTH_INFO_LENGTH);
  return 0;
}

static enum zmk_runtime_macro_protocol_status
runtime_macro_protocol_map_challenge_error(int err) {
  switch (err) {
  case -ENODATA:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NOT_CONFIGURED;
  case -EAGAIN:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_RATE_LIMITED;
  case -EPERM:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID;
  case -EIO:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL;
  default:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL;
  }
}

static int runtime_macro_protocol_process_auth_challenge(
    const uint8_t *request, uint8_t *response) {
  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] !=
          ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT ||
      !runtime_macro_protocol_request_has_empty_object(request)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  int err = zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce));
  if (err != 0) {
    runtime_macro_protocol_set_error(
        response, runtime_macro_protocol_map_challenge_error(err));
    runtime_macro_protocol_zeroize(nonce, sizeof(nonce));
    return 0;
  }

  memcpy(response + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET, nonce,
         sizeof(nonce));
  runtime_macro_protocol_zeroize(nonce, sizeof(nonce));
  response[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] =
      ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE;
  runtime_macro_protocol_set_success(
      response, 0U, ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE);
  return 0;
}

static enum zmk_runtime_macro_protocol_status
runtime_macro_protocol_map_proof_error(int err) {
  switch (err) {
  case -ENODATA:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NOT_CONFIGURED;
  case -ENOENT:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NO_CHALLENGE;
  case -EAGAIN:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_RATE_LIMITED;
  case -EACCES:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_FAILED;
  case -EPERM:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID;
  case -EIO:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL;
  default:
    return ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL;
  }
}

static int runtime_macro_protocol_process_auth_prove(
    struct zmk_runtime_macro_protocol *protocol, const uint8_t *request,
    uint8_t *response) {
  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] !=
      ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT) {
    zmk_runtime_macro_protocol_discard(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  if (runtime_macro_protocol_get_u16(
          request, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET) != 0U ||
      runtime_macro_protocol_get_u16(
          request, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET) !=
          ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE ||
      request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET] !=
          ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE) {
    zmk_runtime_macro_protocol_discard(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  int err = zmk_runtime_macro_auth_verify_proof(
      request + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
      ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE);
  if (err != 0) {
    zmk_runtime_macro_protocol_discard(protocol);
    runtime_macro_protocol_set_error(
        response, runtime_macro_protocol_map_proof_error(err));
    return 0;
  }

  /* A successful login invalidates transactions begun before authentication. */
  zmk_runtime_macro_protocol_discard(protocol);
  runtime_macro_protocol_set_success(response, 0U, 0U);
  return 0;
}

static int runtime_macro_protocol_process_password_set(
    struct zmk_runtime_macro_protocol *protocol, const uint8_t *request,
    uint8_t *response) {
  const uint8_t request_id =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET];
  const uint8_t payload_length =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET];
  const uint16_t offset = runtime_macro_protocol_get_u16(
      request, ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET);
  const uint16_t total_length = runtime_macro_protocol_get_u16(
      request, ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET);

  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] !=
      ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT) {
    runtime_macro_protocol_clear_password_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  if (total_length != ZMK_RUNTIME_MACRO_PROTOCOL_PASSWORD_SET_LENGTH) {
    runtime_macro_protocol_clear_password_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (offset > total_length) {
    runtime_macro_protocol_clear_password_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
    return 0;
  }

  if (payload_length == 0U ||
      payload_length > (uint16_t)(total_length - offset)) {
    runtime_macro_protocol_clear_password_set(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (offset == 0U) {
    runtime_macro_protocol_clear_password_set(protocol);
    protocol->password_set_active = true;
    protocol->password_set_request_id = request_id;
    protocol->password_set_total_length = total_length;
    protocol->password_set_received_length = payload_length;
    memcpy(protocol->password_set_staging,
           request + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
           payload_length);
  } else {
    if (!protocol->password_set_active ||
        protocol->password_set_request_id != request_id ||
        protocol->password_set_total_length != total_length) {
      runtime_macro_protocol_clear_password_set(protocol);
      runtime_macro_protocol_set_error(
          response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
      return 0;
    }

    if (offset != protocol->password_set_received_length) {
      runtime_macro_protocol_clear_password_set(protocol);
      runtime_macro_protocol_set_error(
          response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET);
      return 0;
    }

    memcpy(protocol->password_set_staging + offset,
           request + ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET,
           payload_length);
    protocol->password_set_received_length += payload_length;
  }

  if (protocol->password_set_received_length !=
      protocol->password_set_total_length) {
    runtime_macro_protocol_set_success(
        response, protocol->password_set_received_length,
        protocol->password_set_total_length);
    /* Every successful protected chunk, not only macro operations, keeps the
     * inactivity window alive. OPEN is a no-op here; the final chunk below
     * intentionally clears the session after storage-first replacement. */
    if (!runtime_macro_protocol_refresh_protected_session(protocol, response)) {
      return 0;
    }
    return 0;
  }

  struct zmk_runtime_macro_auth_credential credential = {
      .iterations = runtime_macro_protocol_get_u32(
          protocol->password_set_staging),
  };
  memcpy(credential.salt, protocol->password_set_staging + 4U,
         sizeof(credential.salt));
  memcpy(credential.key,
         protocol->password_set_staging + 4U + sizeof(credential.salt),
         sizeof(credential.key));

  int err = zmk_runtime_macro_auth_set_credential(&credential);
  runtime_macro_protocol_zeroize(&credential, sizeof(credential));
  runtime_macro_protocol_clear_password_set(protocol);

  if (err == 0) {
    zmk_runtime_macro_protocol_discard(protocol);
    runtime_macro_protocol_set_success(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_PASSWORD_SET_LENGTH,
        ZMK_RUNTIME_MACRO_PROTOCOL_PASSWORD_SET_LENGTH);
  } else if (err == -EINVAL || err == -EPERM) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID);
  } else if (err == -EACCES) {
    zmk_runtime_macro_protocol_discard(protocol);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED);
  } else {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR);
  }

  return 0;
}

static int runtime_macro_protocol_process_lock(
    struct zmk_runtime_macro_protocol *protocol, const uint8_t *request,
    uint8_t *response) {
  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET] !=
          ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT ||
      !runtime_macro_protocol_request_has_empty_object(request)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  zmk_runtime_macro_auth_lock();
  zmk_runtime_macro_protocol_discard(protocol);
  runtime_macro_protocol_set_success(response, 0U, 0U);
  return 0;
}

void zmk_runtime_macro_protocol_init(
    struct zmk_runtime_macro_protocol *protocol) {
  zmk_runtime_macro_protocol_discard(protocol);
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

  runtime_macro_protocol_sync_auth_state(protocol);

  const uint8_t opcode = request[ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET];
  const uint8_t payload_length =
      request[ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET];

  /* Base frame validation order is part of the information-leak boundary. */
  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET] !=
      ZMK_RUNTIME_MACRO_PROTOCOL_VERSION) {
    runtime_macro_protocol_discard_for_opcode(protocol, opcode);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION);
    return 0;
  }

  if (!runtime_macro_protocol_opcode_is_known(opcode)) {
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OPCODE);
    return 0;
  }

  if (request[ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET] != 0U) {
    runtime_macro_protocol_discard_for_opcode(protocol, opcode);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  if (payload_length > ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE) {
    runtime_macro_protocol_discard_for_opcode(protocol, opcode);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH);
    return 0;
  }

  if (!runtime_macro_protocol_payload_tail_is_zero(request, payload_length)) {
    runtime_macro_protocol_discard_for_opcode(protocol, opcode);
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST);
    return 0;
  }

  switch (opcode) {
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO:
    return runtime_macro_protocol_process_auth_info(request, response);
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE:
    return runtime_macro_protocol_process_auth_challenge(request, response);
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE:
    return runtime_macro_protocol_process_auth_prove(protocol, request,
                                                      response);
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK:
    return runtime_macro_protocol_process_lock(protocol, request, response);
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET:
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR: {
    enum zmk_runtime_macro_protocol_status access =
        runtime_macro_protocol_management_access(protocol);
    if (access != ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK) {
      runtime_macro_protocol_set_error(response, access);
      return 0;
    }

    int err;
    switch (opcode) {
    case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST:
      err = runtime_macro_protocol_process_list(request, response);
      break;
    case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET:
      err = runtime_macro_protocol_process_get(request, response);
      break;
    case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET:
      err = runtime_macro_protocol_process_set(protocol, request, response);
      break;
    default:
      err = runtime_macro_protocol_process_clear(request, response);
      break;
    }

    if (err == 0 &&
        response[ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET] ==
            ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK) {
      (void)runtime_macro_protocol_refresh_protected_session(protocol,
                                                              response);
    }
    return err;
  }
  case ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET: {
    enum zmk_runtime_macro_protocol_status access =
        runtime_macro_protocol_management_access(protocol);
    if (access != ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK) {
      runtime_macro_protocol_set_error(response, access);
      return 0;
    }

    return runtime_macro_protocol_process_password_set(protocol, request,
                                                       response);
  }
  default:
    /* The opcode was checked above; keep a defensive default for compilers. */
    runtime_macro_protocol_set_error(
        response, ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OPCODE);
    return 0;
  }
}
