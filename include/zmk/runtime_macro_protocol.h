/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/runtime_macro_auth.h>

#define ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE 32U
#define ZMK_RUNTIME_MACRO_PROTOCOL_HEADER_SIZE 10U
#define ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_SIZE 22U
#define ZMK_RUNTIME_MACRO_PROTOCOL_VERSION 2U

#define ZMK_RUNTIME_MACRO_PROTOCOL_VERSION_OFFSET 0U
#define ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_OFFSET 1U
#define ZMK_RUNTIME_MACRO_PROTOCOL_REQUEST_ID_OFFSET 2U
#define ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OFFSET 3U
#define ZMK_RUNTIME_MACRO_PROTOCOL_SLOT_OFFSET 4U
#define ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_LENGTH_OFFSET 5U
#define ZMK_RUNTIME_MACRO_PROTOCOL_OFFSET_OFFSET 6U
#define ZMK_RUNTIME_MACRO_PROTOCOL_TOTAL_LENGTH_OFFSET 8U
#define ZMK_RUNTIME_MACRO_PROTOCOL_PAYLOAD_OFFSET 10U

#define ZMK_RUNTIME_MACRO_PROTOCOL_LIST_SLOT 0xffU

#define ZMK_RUNTIME_MACRO_PROTOCOL_PASSWORD_SET_LENGTH \
  (sizeof(uint32_t) + ZMK_RUNTIME_MACRO_AUTH_SALT_SIZE + \
   ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE)

enum zmk_runtime_macro_protocol_opcode {
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LIST = 1,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_GET = 2,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_SET = 3,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_CLEAR = 4,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_INFO = 0x10,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_CHALLENGE = 0x11,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_AUTH_PROVE = 0x12,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_PASSWORD_SET = 0x13,
  ZMK_RUNTIME_MACRO_PROTOCOL_OPCODE_LOCK = 0x14,
};

enum zmk_runtime_macro_protocol_status {
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_OK = 0,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_VERSION = 1,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OPCODE = 2,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_REQUEST = 3,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_SLOT = 4,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_OFFSET = 5,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_BAD_LENGTH = 6,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INVALID_TEXT = 7,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_STORAGE_ERROR = 8,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_INTERNAL = 9,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_REQUIRED = 10,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_FAILED = 11,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NOT_CONFIGURED = 12,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_RATE_LIMITED = 13,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_AUTH_NO_CHALLENGE = 14,
  ZMK_RUNTIME_MACRO_PROTOCOL_STATUS_CREDENTIAL_INVALID = 15,
};

/**
 * @brief Per-consumer state for the transport-independent runtime macro
 * protocol.
 *
 * The context contains one SET transaction and one PASSWORD_SET transaction.
 * A transport must keep one context per serialized protocol consumer.
 */
struct zmk_runtime_macro_protocol {
  bool set_active;
  uint8_t set_request_id;
  uint8_t set_slot;
  uint16_t set_total_length;
  uint16_t set_received_length;
  char set_staging[CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN];

  bool password_set_active;
  uint8_t password_set_request_id;
  uint16_t password_set_total_length;
  uint16_t password_set_received_length;
  uint8_t password_set_staging[
      ZMK_RUNTIME_MACRO_PROTOCOL_PASSWORD_SET_LENGTH];

  /* Used to discard transactions when the auth session lazily expires or
   * changes state between protocol requests. */
  bool auth_state_known;
  uint8_t auth_state;
  bool auth_session_authenticated;
};

/**
 * @brief Initialize a protocol context and discard incomplete transactions.
 */
void zmk_runtime_macro_protocol_init(
    struct zmk_runtime_macro_protocol *protocol);

/**
 * @brief Discard all protocol-local transactions and authentication tracking.
 *
 * This is the transport lifecycle hook. It does not alter the persistent
 * credential or authentication core; callers handling a USB reset/disconnect
 * must also call zmk_runtime_macro_auth_transport_reset().
 */
void zmk_runtime_macro_protocol_discard(
    struct zmk_runtime_macro_protocol *protocol);

/**
 * @brief Process one complete request frame and produce one complete response.
 *
 * The request and response buffers each contain exactly
 * ZMK_RUNTIME_MACRO_PROTOCOL_FRAME_SIZE bytes. The request is never modified.
 * A non-NULL response is fully initialized on every call. Protocol errors are
 * reported in the response status field. A NULL request is invalid and returns
 * -EINVAL after zero-initializing the response; it does not produce a protocol
 * error response. A NULL context produces an INTERNAL response.
 *
 * Calls for one context must be serialized by the transport. The protocol
 * implementation does not provide a work queue or other concurrency layer.
 *
 * @param protocol Per-consumer transaction context, or NULL.
 * @param request Complete 32-byte request frame, or NULL.
 * @param response Destination for the complete 32-byte response frame.
 * @retval 0 A response frame was produced (including protocol errors).
 * @retval -EINVAL response is NULL or request is NULL; a non-NULL response is
 *             zero-initialized when request is NULL.
 */
int zmk_runtime_macro_protocol_process(
    struct zmk_runtime_macro_protocol *protocol, const uint8_t *request,
    uint8_t *response);
