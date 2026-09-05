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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include <zmk/runtime_macro_auth.h>
#include "runtime_macro_auth_internal.h"

#if !defined(ZMK_RUNTIME_MACRO_AUTH_TEST)
#include <mbedtls/constant_time.h>
#include <mbedtls/md.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RUNTIME_MACRO_AUTH_SETTING_NAME "auth/credential"
#define RUNTIME_MACRO_AUTH_SETTING_PATH "runtime_macro/" RUNTIME_MACRO_AUTH_SETTING_NAME
#define RUNTIME_MACRO_AUTH_DOMAIN "ZMK-RUNTIME-MACRO-AUTH-V2"
#define RUNTIME_MACRO_AUTH_DOMAIN_SIZE (sizeof(RUNTIME_MACRO_AUTH_DOMAIN) - 1U)
#define RUNTIME_MACRO_AUTH_FAILURE_COUNT_MAX 4U
#define RUNTIME_MACRO_AUTH_COOLDOWN_MAX_SECONDS 8U

struct runtime_macro_auth_state_data {
  enum zmk_runtime_macro_auth_state state;
  struct zmk_runtime_macro_auth_credential credential;
  bool session_active;
  int64_t session_deadline_ms;
  bool challenge_active;
  uint8_t challenge[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  int64_t challenge_deadline_ms;
  uint8_t failure_count;
  int64_t cooldown_deadline_ms;
};

static struct runtime_macro_auth_state_data runtime_macro_auth = {
    .state = ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN,
};
static K_MUTEX_DEFINE(runtime_macro_auth_mutex);
static K_MUTEX_DEFINE(runtime_macro_auth_update_mutex);

/* Settings callbacks run during boot, before any protocol consumer exists. */
static bool runtime_macro_auth_settings_seen;
static bool runtime_macro_auth_settings_invalid;
static uint32_t runtime_macro_auth_settings_generation;

static int64_t runtime_macro_auth_now_ms(void) {
#if defined(ZMK_RUNTIME_MACRO_AUTH_TEST)
  extern int64_t runtime_macro_auth_test_now_ms(void);

  return runtime_macro_auth_test_now_ms();
#else
  return k_uptime_get();
#endif
}

static int runtime_macro_auth_random(void *destination, size_t length) {
#if defined(ZMK_RUNTIME_MACRO_AUTH_TEST)
  extern int runtime_macro_auth_test_random(void *destination, size_t length);

  return runtime_macro_auth_test_random(destination, length);
#else
  return sys_csrand_get(destination, length);
#endif
}

static int runtime_macro_auth_hmac(const uint8_t *key, const uint8_t *input, size_t input_length,
                                   uint8_t *output) {
#if defined(ZMK_RUNTIME_MACRO_AUTH_TEST)
  extern int runtime_macro_auth_test_hmac(const uint8_t *key, const uint8_t *input,
                                          size_t input_length, uint8_t *output);

  return runtime_macro_auth_test_hmac(key, input, input_length, output);
#else
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  if (md_info == NULL) {
    return -EIO;
  }

  return mbedtls_md_hmac(md_info, key, ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE, input, input_length,
                         output) == 0
             ? 0
             : -EIO;
#endif
}

static bool runtime_macro_auth_constant_time_equal(const uint8_t *left, const uint8_t *right,
                                                   size_t length) {
#if defined(ZMK_RUNTIME_MACRO_AUTH_TEST)
  uint8_t difference = 0U;

  for (size_t i = 0; i < length; i++) {
    difference |= left[i] ^ right[i];
  }

  return difference == 0U;
#else
  return mbedtls_ct_memcmp(left, right, length) == 0;
#endif
}

static void runtime_macro_auth_zeroize(void *data, size_t length) {
  volatile uint8_t *bytes = data;

  while (length > 0U) {
    *bytes++ = 0U;
    length--;
  }
}

static bool runtime_macro_auth_all_zero(const uint8_t *data, size_t length) {
  uint8_t difference = 0U;

  for (size_t i = 0; i < length; i++) {
    difference |= data[i];
  }

  return difference == 0U;
}

static bool runtime_macro_auth_credential_is_valid(
    const struct zmk_runtime_macro_auth_credential *credential) {
  if (credential == NULL || credential->iterations < ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_MIN ||
      credential->iterations > ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_MAX ||
      runtime_macro_auth_all_zero(credential->salt, sizeof(credential->salt)) ||
      runtime_macro_auth_all_zero(credential->key, sizeof(credential->key))) {
    return false;
  }

  return true;
}

static void runtime_macro_auth_put_le32(uint8_t *destination, uint32_t value) {
  destination[0] = (uint8_t)value;
  destination[1] = (uint8_t)(value >> 8);
  destination[2] = (uint8_t)(value >> 16);
  destination[3] = (uint8_t)(value >> 24);
}

static uint32_t runtime_macro_auth_get_le32(const uint8_t *source) {
  return (uint32_t)source[0] | ((uint32_t)source[1] << 8) | ((uint32_t)source[2] << 16) |
         ((uint32_t)source[3] << 24);
}

static void runtime_macro_auth_serialize_credential(
    const struct zmk_runtime_macro_auth_credential *credential,
    uint8_t serialized[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE]) {
  serialized[0] = ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_VERSION;
  runtime_macro_auth_put_le32(serialized + 1U, credential->iterations);
  memcpy(serialized + 5U, credential->salt, sizeof(credential->salt));
  memcpy(serialized + 5U + sizeof(credential->salt), credential->key, sizeof(credential->key));
}

static void runtime_macro_auth_deserialize_credential(
    const uint8_t serialized[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE],
    struct zmk_runtime_macro_auth_credential *credential) {
  credential->iterations = runtime_macro_auth_get_le32(serialized + 1U);
  memcpy(credential->salt, serialized + 5U, sizeof(credential->salt));
  memcpy(credential->key, serialized + 5U + sizeof(credential->salt), sizeof(credential->key));
}

static void runtime_macro_auth_clear_challenge_locked(void) {
  runtime_macro_auth.challenge_active = false;
  runtime_macro_auth.challenge_deadline_ms = 0;
  runtime_macro_auth_zeroize(runtime_macro_auth.challenge, sizeof(runtime_macro_auth.challenge));
}

static void runtime_macro_auth_clear_session_locked(void) {
  runtime_macro_auth.session_active = false;
  runtime_macro_auth.session_deadline_ms = 0;
}

static void runtime_macro_auth_clear_failure_locked(void) {
  runtime_macro_auth.failure_count = 0U;
  runtime_macro_auth.cooldown_deadline_ms = 0;
}

static void runtime_macro_auth_clear_transient_locked(bool clear_failure) {
  runtime_macro_auth_clear_session_locked();
  runtime_macro_auth_clear_challenge_locked();
  if (clear_failure) {
    runtime_macro_auth_clear_failure_locked();
  }
}

static void runtime_macro_auth_set_error_locked(void) {
  runtime_macro_auth.state = ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED;
  runtime_macro_auth_zeroize(&runtime_macro_auth.credential,
                             sizeof(runtime_macro_auth.credential));
  runtime_macro_auth_clear_transient_locked(true);
}

static void runtime_macro_auth_expire_session_locked(int64_t now_ms) {
  if (!runtime_macro_auth.session_active || now_ms < runtime_macro_auth.session_deadline_ms) {
    return;
  }

  runtime_macro_auth_clear_session_locked();
  runtime_macro_auth_clear_challenge_locked();
}

static bool runtime_macro_auth_cooldown_active_locked(int64_t now_ms) {
  return runtime_macro_auth.failure_count != 0U && now_ms < runtime_macro_auth.cooldown_deadline_ms;
}

static void runtime_macro_auth_record_failure_locked(int64_t now_ms) {
  if (runtime_macro_auth.failure_count < RUNTIME_MACRO_AUTH_FAILURE_COUNT_MAX) {
    runtime_macro_auth.failure_count++;
  }

  uint32_t cooldown_seconds = 1U << (runtime_macro_auth.failure_count - 1U);
  if (cooldown_seconds > RUNTIME_MACRO_AUTH_COOLDOWN_MAX_SECONDS) {
    cooldown_seconds = RUNTIME_MACRO_AUTH_COOLDOWN_MAX_SECONDS;
  }

  runtime_macro_auth.cooldown_deadline_ms = now_ms + ((int64_t)cooldown_seconds * 1000);
}

enum zmk_runtime_macro_auth_state zmk_runtime_macro_auth_get_state(void) {
  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth_expire_session_locked(runtime_macro_auth_now_ms());
  enum zmk_runtime_macro_auth_state state = runtime_macro_auth.state;
  k_mutex_unlock(&runtime_macro_auth_mutex);

  return state;
}

int zmk_runtime_macro_auth_get_info(struct zmk_runtime_macro_auth_info *info) {
  if (info == NULL) {
    return -EINVAL;
  }

  memset(info, 0, sizeof(*info));

  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth_expire_session_locked(runtime_macro_auth_now_ms());
  info->state = runtime_macro_auth.state;
  info->session_authenticated = runtime_macro_auth.session_active;

  if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN) {
    info->kdf_id = ZMK_RUNTIME_MACRO_AUTH_KDF_ID;
    info->iterations = ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT;
  } else if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED) {
    info->kdf_id = ZMK_RUNTIME_MACRO_AUTH_KDF_ID;
    info->iterations = runtime_macro_auth.credential.iterations;
    memcpy(info->salt, runtime_macro_auth.credential.salt, sizeof(info->salt));
  }

  k_mutex_unlock(&runtime_macro_auth_mutex);
  return 0;
}

int zmk_runtime_macro_auth_generate_challenge(uint8_t *nonce, size_t nonce_size) {
  if (nonce == NULL || nonce_size != ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE) {
    return -EINVAL;
  }

  memset(nonce, 0, nonce_size);

  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  int64_t now_ms = runtime_macro_auth_now_ms();
  runtime_macro_auth_expire_session_locked(now_ms);

  if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -ENODATA;
  }
  if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -EPERM;
  }
  if (runtime_macro_auth_cooldown_active_locked(now_ms)) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -EAGAIN;
  }

  /* A failed CSPRNG call must not leave an older nonce usable. */
  runtime_macro_auth_clear_challenge_locked();
  uint8_t generated_nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  int err = runtime_macro_auth_random(generated_nonce, sizeof(generated_nonce));
  if (err != 0) {
    runtime_macro_auth_zeroize(generated_nonce, sizeof(generated_nonce));
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -EIO;
  }

  memcpy(runtime_macro_auth.challenge, generated_nonce, sizeof(generated_nonce));
  runtime_macro_auth_zeroize(generated_nonce, sizeof(generated_nonce));
  runtime_macro_auth.challenge_active = true;
  runtime_macro_auth.challenge_deadline_ms =
      now_ms + ((int64_t)CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT * 1000);
  memcpy(nonce, runtime_macro_auth.challenge, nonce_size);
  k_mutex_unlock(&runtime_macro_auth_mutex);

  return 0;
}

int zmk_runtime_macro_auth_verify_proof(const uint8_t *proof, size_t proof_size) {
  if (proof == NULL || proof_size != ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE) {
    return -EINVAL;
  }

  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  int64_t now_ms = runtime_macro_auth_now_ms();
  runtime_macro_auth_expire_session_locked(now_ms);

  if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -ENODATA;
  }
  if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -EPERM;
  }
  if (!runtime_macro_auth.challenge_active || now_ms >= runtime_macro_auth.challenge_deadline_ms) {
    runtime_macro_auth_clear_challenge_locked();
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -ENOENT;
  }

  uint8_t key[ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE];
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t message[RUNTIME_MACRO_AUTH_DOMAIN_SIZE + ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t expected[32];

  memcpy(key, runtime_macro_auth.credential.key, sizeof(key));
  memcpy(nonce, runtime_macro_auth.challenge, sizeof(nonce));
  runtime_macro_auth_clear_challenge_locked();
  runtime_macro_auth_clear_session_locked();
  memcpy(message, RUNTIME_MACRO_AUTH_DOMAIN, RUNTIME_MACRO_AUTH_DOMAIN_SIZE);
  memcpy(message + RUNTIME_MACRO_AUTH_DOMAIN_SIZE, nonce, sizeof(nonce));

  int err = runtime_macro_auth_hmac(key, message, sizeof(message), expected);
  if (err != 0) {
    runtime_macro_auth_zeroize(key, sizeof(key));
    runtime_macro_auth_zeroize(nonce, sizeof(nonce));
    runtime_macro_auth_zeroize(message, sizeof(message));
    runtime_macro_auth_zeroize(expected, sizeof(expected));
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -EIO;
  }

  bool matches = runtime_macro_auth_constant_time_equal(expected, proof,
                                                        ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE);
  runtime_macro_auth_zeroize(key, sizeof(key));
  runtime_macro_auth_zeroize(nonce, sizeof(nonce));
  runtime_macro_auth_zeroize(message, sizeof(message));
  runtime_macro_auth_zeroize(expected, sizeof(expected));

  if (!matches) {
    runtime_macro_auth_record_failure_locked(now_ms);
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -EACCES;
  }

  runtime_macro_auth.session_active = true;
  runtime_macro_auth.session_deadline_ms =
      now_ms + ((int64_t)CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT * 1000);
  runtime_macro_auth_clear_failure_locked();
  k_mutex_unlock(&runtime_macro_auth_mutex);

  return 0;
}

bool zmk_runtime_macro_auth_is_authenticated(void) {
  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth_expire_session_locked(runtime_macro_auth_now_ms());
  bool authenticated = runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED &&
                       runtime_macro_auth.session_active;
  k_mutex_unlock(&runtime_macro_auth_mutex);

  return authenticated;
}

int zmk_runtime_macro_auth_refresh_session(void) {
  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  int64_t now_ms = runtime_macro_auth_now_ms();
  runtime_macro_auth_expire_session_locked(now_ms);

  if (runtime_macro_auth.state != ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED ||
      !runtime_macro_auth.session_active) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    return -EACCES;
  }

  runtime_macro_auth.session_deadline_ms =
      now_ms + ((int64_t)CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT * 1000);
  k_mutex_unlock(&runtime_macro_auth_mutex);
  return 0;
}

int zmk_runtime_macro_auth_set_credential(
    const struct zmk_runtime_macro_auth_credential *credential) {
  if (!runtime_macro_auth_credential_is_valid(credential)) {
    return -EINVAL;
  }

  struct zmk_runtime_macro_auth_credential requested_credential = *credential;
  uint8_t serialized[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE];
  runtime_macro_auth_serialize_credential(&requested_credential, serialized);

  k_mutex_lock(&runtime_macro_auth_update_mutex, K_FOREVER);

  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth_expire_session_locked(runtime_macro_auth_now_ms());
  if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    runtime_macro_auth_zeroize(serialized, sizeof(serialized));
    runtime_macro_auth_zeroize(&requested_credential, sizeof(requested_credential));
    k_mutex_unlock(&runtime_macro_auth_update_mutex);
    return -EPERM;
  }
  if (runtime_macro_auth.state == ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED &&
      !runtime_macro_auth.session_active) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    runtime_macro_auth_zeroize(serialized, sizeof(serialized));
    runtime_macro_auth_zeroize(&requested_credential, sizeof(requested_credential));
    k_mutex_unlock(&runtime_macro_auth_update_mutex);
    return -EACCES;
  }
  k_mutex_unlock(&runtime_macro_auth_mutex);

  int err = settings_save_one(RUNTIME_MACRO_AUTH_SETTING_PATH, serialized, sizeof(serialized));
  runtime_macro_auth_zeroize(serialized, sizeof(serialized));
  if (err != 0) {
    LOG_ERR("Failed to save runtime macro authentication credential (err %d)", err);
    runtime_macro_auth_zeroize(&requested_credential, sizeof(requested_credential));
    k_mutex_unlock(&runtime_macro_auth_update_mutex);
    return err;
  }

  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth.credential = requested_credential;
  runtime_macro_auth.state = ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED;
  runtime_macro_auth_settings_seen = true;
  runtime_macro_auth_settings_invalid = false;
  runtime_macro_auth_clear_transient_locked(true);
  k_mutex_unlock(&runtime_macro_auth_mutex);
  runtime_macro_auth_zeroize(&requested_credential, sizeof(requested_credential));
  k_mutex_unlock(&runtime_macro_auth_update_mutex);

  return 0;
}

void zmk_runtime_macro_auth_lock(void) {
  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth_clear_transient_locked(false);
  k_mutex_unlock(&runtime_macro_auth_mutex);
}

void zmk_runtime_macro_auth_transport_reset(void) {
  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth_clear_transient_locked(true);
  k_mutex_unlock(&runtime_macro_auth_mutex);
}

int zmk_runtime_macro_auth_settings_set(const char *name, size_t length, settings_read_cb read_cb,
                                        void *cb_arg) {
  const char *next;
  const char *tail;

  if (name == NULL || !settings_name_steq(name, "auth", &next) || next == NULL ||
      !settings_name_steq(next, "credential", &tail) || tail != NULL) {
    return -ENOENT;
  }

  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  bool duplicate = runtime_macro_auth_settings_seen;
  runtime_macro_auth_settings_seen = true;
  runtime_macro_auth_settings_invalid = true;
  uint32_t generation = ++runtime_macro_auth_settings_generation;
  runtime_macro_auth_set_error_locked();
  k_mutex_unlock(&runtime_macro_auth_mutex);

  if (duplicate || length != ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE || read_cb == NULL) {
    return -EINVAL;
  }

  uint8_t serialized[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE];
  ssize_t read_length = read_cb(cb_arg, serialized, sizeof(serialized));
  if (read_length < 0) {
    runtime_macro_auth_zeroize(serialized, sizeof(serialized));
    return (int)read_length;
  }
  if ((size_t)read_length != sizeof(serialized) ||
      serialized[0] != ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_VERSION) {
    runtime_macro_auth_zeroize(serialized, sizeof(serialized));
    return -EINVAL;
  }

  struct zmk_runtime_macro_auth_credential credential;
  runtime_macro_auth_deserialize_credential(serialized, &credential);
  runtime_macro_auth_zeroize(serialized, sizeof(serialized));
  if (!runtime_macro_auth_credential_is_valid(&credential)) {
    runtime_macro_auth_zeroize(&credential, sizeof(credential));
    return -EINVAL;
  }

  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  if (runtime_macro_auth_settings_generation != generation ||
      !runtime_macro_auth_settings_invalid) {
    k_mutex_unlock(&runtime_macro_auth_mutex);
    runtime_macro_auth_zeroize(&credential, sizeof(credential));
    return -EINVAL;
  }

  runtime_macro_auth.credential = credential;
  runtime_macro_auth.state = ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED;
  runtime_macro_auth_settings_invalid = false;
  runtime_macro_auth_clear_transient_locked(true);
  k_mutex_unlock(&runtime_macro_auth_mutex);
  runtime_macro_auth_zeroize(&credential, sizeof(credential));
  return 0;
}

int zmk_runtime_macro_auth_settings_commit(void) {
  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  if (!runtime_macro_auth_settings_seen) {
    runtime_macro_auth.state = ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN;
    runtime_macro_auth_zeroize(&runtime_macro_auth.credential,
                               sizeof(runtime_macro_auth.credential));
    runtime_macro_auth_clear_transient_locked(true);
  } else if (runtime_macro_auth_settings_invalid) {
    runtime_macro_auth_set_error_locked();
  }
  k_mutex_unlock(&runtime_macro_auth_mutex);
  return 0;
}

#if defined(ZMK_RUNTIME_MACRO_AUTH_TEST)
void runtime_macro_auth_test_reset(void) {
  k_mutex_lock(&runtime_macro_auth_mutex, K_FOREVER);
  runtime_macro_auth_zeroize(&runtime_macro_auth, sizeof(runtime_macro_auth));
  runtime_macro_auth.state = ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN;
  runtime_macro_auth_settings_seen = false;
  runtime_macro_auth_settings_invalid = false;
  runtime_macro_auth_settings_generation = 0U;
  k_mutex_unlock(&runtime_macro_auth_mutex);
}
#endif
