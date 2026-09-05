/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_VERSION 1U
#define ZMK_RUNTIME_MACRO_AUTH_KDF_ID 1U
#define ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE 53U
#define ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT 600000U
#define ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_MIN 100000U
#define ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_MAX 5000000U
#define ZMK_RUNTIME_MACRO_AUTH_SALT_SIZE 16U
#define ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE 32U
#define ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE 16U
#define ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE 16U

/**
 * @brief Persistent authentication state.
 *
 * ERROR_LOCKED is intentionally distinct from OPEN. It is entered when a
 * credential setting exists but cannot be validated and can only be cleared by
 * clearing Settings (for example, with a ZMK settings-reset firmware).
 */
enum zmk_runtime_macro_auth_state {
  ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN = 0,
  ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED = 1,
  ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED = 2,
};

/**
 * @brief Complete in-memory credential supplied by the desktop client.
 *
 * The key is the 32-byte PBKDF2-HMAC-SHA256 output. The firmware does not run
 * PBKDF2 and never receives the user's password.
 */
struct zmk_runtime_macro_auth_credential {
  uint32_t iterations;
  uint8_t salt[ZMK_RUNTIME_MACRO_AUTH_SALT_SIZE];
  uint8_t key[ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE];
};

/**
 * @brief Public authentication information.
 *
 * This deliberately omits the credential key. kdf_id identifies the fixed
 * PBKDF2-HMAC-SHA256 derivation used by the client. In ERROR_LOCKED state,
 * kdf_id, iterations, and salt are zeroed and state reports the lockout
 * condition.
 */
struct zmk_runtime_macro_auth_info {
  enum zmk_runtime_macro_auth_state state;
  bool session_authenticated;
  uint8_t kdf_id;
  uint32_t iterations;
  uint8_t salt[ZMK_RUNTIME_MACRO_AUTH_SALT_SIZE];
};

/**
 * @brief Return the current persistent authentication state.
 *
 * Checking the state also expires an elapsed authentication session.
 */
enum zmk_runtime_macro_auth_state zmk_runtime_macro_auth_get_state(void);

/**
 * @brief Read public credential metadata and session state.
 *
 * The output never contains the credential key. OPEN returns the default KDF
 * iteration count and an all-zero salt; PROTECTED returns the persisted values.
 */
int zmk_runtime_macro_auth_get_info(struct zmk_runtime_macro_auth_info *info);

/**
 * @brief Generate a one-shot challenge nonce.
 *
 * The nonce is generated with sys_csrand_get(), expires according to the
 * challenge timeout Kconfig value, and is consumed by the next proof attempt.
 *
 * @retval 0 Challenge generated.
 * @retval -EINVAL Invalid output pointer or size.
 * @retval -ENODATA Authentication is not configured (OPEN).
 * @retval -EPERM Credential data is invalid (ERROR_LOCKED).
 * @retval -EAGAIN Authentication failure cooldown is active.
 * @retval -EIO The CSPRNG failed; no old nonce is reused.
 */
int zmk_runtime_macro_auth_generate_challenge(uint8_t *nonce, size_t nonce_size);

/**
 * @brief Verify and consume the current one-shot proof.
 *
 * The expected proof is the first 16 bytes of
 * HMAC-SHA256(key, "ZMK-RUNTIME-MACRO-AUTH-V2" || nonce). The nonce is
 * consumed before verification, including when the proof is incorrect.
 *
 * @retval 0 Proof accepted and the inactivity session opened.
 * @retval -EINVAL Invalid proof pointer or size.
 * @retval -ENODATA Authentication is not configured (OPEN).
 * @retval -EPERM Credential data is invalid (ERROR_LOCKED).
 * @retval -ENOENT No unexpired challenge exists.
 * @retval -EIO HMAC computation failed.
 * @retval -EACCES Proof did not match (the session is locked).
 */
int zmk_runtime_macro_auth_verify_proof(const uint8_t *proof, size_t proof_size);

/**
 * @brief Return whether a protected management session is currently valid.
 *
 * An expired session is cleared before returning false. This call does not
 * refresh the inactivity deadline.
 */
bool zmk_runtime_macro_auth_is_authenticated(void);

/**
 * @brief Refresh a valid protected management session.
 *
 * This is intended for successful protected management operations. It returns
 * -EACCES for OPEN, ERROR_LOCKED, or an expired/missing session.
 */
int zmk_runtime_macro_auth_refresh_session(void);

/**
 * @brief Persist a complete credential and activate it after storage succeeds.
 *
 * OPEN permits the first credential. PROTECTED requires a valid authenticated
 * session. ERROR_LOCKED never permits this operation. There is deliberately no
 * password-clear API.
 */
int zmk_runtime_macro_auth_set_credential(
    const struct zmk_runtime_macro_auth_credential *credential);

/**
 * @brief Clear the session and challenge without changing rate limiting.
 *
 * The persisted credential, failure count, and cooldown deadline are unchanged.
 * This is idempotent and intentionally cannot bypass authentication backoff.
 */
void zmk_runtime_macro_auth_lock(void);

/**
 * @brief Clear authentication and transport-local transient state.
 *
 * This is the lifecycle hook for USB disconnect/reset. It also clears the
 * failure count and cooldown; the persisted credential is unchanged.
 */
void zmk_runtime_macro_auth_transport_reset(void);

