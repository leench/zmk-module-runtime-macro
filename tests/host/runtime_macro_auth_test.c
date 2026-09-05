/*
 * Host-side tests for the runtime macro authentication core.
 *
 * The authentication source and the existing Settings handler are included
 * directly so persistence, lifecycle, and crypto seams can be exercised
 * without a Zephyr device.
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define CONFIG_SETTINGS 1
#define CONFIG_ZMK_LOG_LEVEL 0
#define CONFIG_ZMK_RUNTIME_MACRO_USB_HID 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_TEST 1
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT 30
#define CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT 300
#define CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT 2
#define CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN 8

#define ZMK_RUNTIME_MACRO_AUTH_TEST 1

int64_t host_uptime;

#include "../../src/runtime_macro_auth.c"
#include "../../src/runtime_macro_ascii.c"
#include "../../src/runtime_macro.c"

static int failures;

#define EXPECT_TRUE(condition)                                                 \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
      failures++;                                                              \
    }                                                                          \
  } while (false)

#define EXPECT_EQ(expected, actual)                                            \
  do {                                                                         \
    long long expected_value = (long long)(expected);                          \
    long long actual_value = (long long)(actual);                              \
    if (expected_value != actual_value) {                                      \
      fprintf(stderr, "FAIL: %s:%d: expected %lld, got %lld\n", __FILE__,    \
              __LINE__, expected_value, actual_value);                         \
      failures++;                                                              \
    }                                                                          \
  } while (false)

static int random_result;
static unsigned int random_calls;
static uint8_t random_bytes[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];

int64_t runtime_macro_auth_test_now_ms(void) { return host_uptime; }

int runtime_macro_auth_test_random(void *destination, size_t length) {
  random_calls++;
  if (random_result != 0) {
    return random_result;
  }

  memcpy(destination, random_bytes, length);
  return 0;
}

static int hmac_result;
static unsigned int hmac_calls;
static uint8_t hmac_output[32];
static uint8_t hmac_last_key[ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE];
static uint8_t hmac_last_input[64];
static size_t hmac_last_input_length;

int runtime_macro_auth_test_hmac(const uint8_t *key, const uint8_t *input, size_t input_length,
                                 uint8_t *output) {
  hmac_calls++;
  memcpy(hmac_last_key, key, sizeof(hmac_last_key));
  memcpy(hmac_last_input, input, input_length);
  hmac_last_input_length = input_length;
  if (hmac_result != 0) {
    return hmac_result;
  }

  memcpy(output, hmac_output, 32U);
  return 0;
}

static int save_result;
static unsigned int save_calls;
static char saved_name[64];
static uint8_t saved_value[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE];
static size_t saved_length;

static pthread_mutex_t backend_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t backend_condition = PTHREAD_COND_INITIALIZER;
static bool block_first_save;
static bool first_save_entered;
static bool release_first_save;

int settings_save_one(const char *name, const void *value, size_t length) {
  pthread_mutex_lock(&backend_mutex);
  unsigned int call = ++save_calls;
  snprintf(saved_name, sizeof(saved_name), "%s", name);
  saved_length = length;
  if (length <= sizeof(saved_value)) {
    memcpy(saved_value, value, length);
  }

  int result = save_result;
  if (block_first_save && call == 1U) {
    first_save_entered = true;
    pthread_cond_broadcast(&backend_condition);
    while (!release_first_save) {
      pthread_cond_wait(&backend_condition, &backend_mutex);
    }
  }

  pthread_mutex_unlock(&backend_mutex);
  return result;
}

int settings_delete(const char *name) {
  (void)name;
  return 0;
}

static void reset_backend(void) {
  pthread_mutex_lock(&backend_mutex);
  save_result = 0;
  save_calls = 0;
  saved_name[0] = '\0';
  memset(saved_value, 0, sizeof(saved_value));
  saved_length = 0;
  block_first_save = false;
  first_save_entered = false;
  release_first_save = false;
  pthread_mutex_unlock(&backend_mutex);
}

static void reset_crypto_seams(void) {
  random_result = 0;
  random_calls = 0;
  memset(random_bytes, 0, sizeof(random_bytes));
  hmac_result = 0;
  hmac_calls = 0;
  memset(hmac_output, 0, sizeof(hmac_output));
  memset(hmac_last_key, 0, sizeof(hmac_last_key));
  memset(hmac_last_input, 0, sizeof(hmac_last_input));
  hmac_last_input_length = 0;
}

static void reset_auth(void) {
  runtime_macro_auth_test_reset();
  reset_backend();
  reset_crypto_seams();
  host_uptime = 0;
}

static struct zmk_runtime_macro_auth_credential make_credential(uint8_t marker) {
  struct zmk_runtime_macro_auth_credential credential = {
      .iterations = ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT,
  };

  memset(credential.salt, (int)(marker + 1U), sizeof(credential.salt));
  memset(credential.key, marker, sizeof(credential.key));
  return credential;
}

static void serialize_credential(
    const struct zmk_runtime_macro_auth_credential *credential,
    uint8_t serialized[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE]) {
  runtime_macro_auth_serialize_credential(credential, serialized);
}

struct settings_reader {
  const uint8_t *data;
  size_t length;
  int error;
  bool short_read;
};

static ssize_t read_settings(void *arg, void *data, size_t length) {
  struct settings_reader *reader = arg;
  if (reader->error != 0) {
    return reader->error;
  }

  size_t copy_length = reader->length < length ? reader->length : length;
  memcpy(data, reader->data, copy_length);
  if (reader->short_read && copy_length > 0U) {
    copy_length--;
  }
  return (ssize_t)copy_length;
}

static int load_credential_named(
    const char *name, const struct zmk_runtime_macro_auth_credential *credential) {
  uint8_t serialized[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE];
  serialize_credential(credential, serialized);
  struct settings_reader reader = {
      .data = serialized,
      .length = sizeof(serialized),
  };

  int err = runtime_macro_settings_set(name, sizeof(serialized), read_settings, &reader);
  memset(serialized, 0, sizeof(serialized));
  if (err != 0) {
    return err;
  }

  return zmk_runtime_macro_auth_settings_commit();
}

static int load_credential(const struct zmk_runtime_macro_auth_credential *credential) {
  return load_credential_named("auth/credential", credential);
}

static void set_test_proof(uint8_t value) {
  for (size_t i = 0; i < sizeof(hmac_output); i++) {
    hmac_output[i] = (uint8_t)(value + i);
  }
}

static void authenticate(void) {
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];

  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  memcpy(proof, hmac_output, sizeof(proof));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  EXPECT_TRUE(zmk_runtime_macro_auth_is_authenticated());
}

static void test_open_and_credential_validation(void) {
  reset_auth();
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN, zmk_runtime_macro_auth_get_state());

  struct zmk_runtime_macro_auth_info info;
  EXPECT_EQ(0, zmk_runtime_macro_auth_get_info(&info));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN, info.state);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_KDF_ID, info.kdf_id);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_DEFAULT, info.iterations);
  EXPECT_TRUE(runtime_macro_auth_all_zero(info.salt, sizeof(info.salt)));
  EXPECT_EQ(-ENODATA, zmk_runtime_macro_auth_generate_challenge(random_bytes, sizeof(random_bytes)));
  EXPECT_EQ(-ENODATA, zmk_runtime_macro_auth_verify_proof(random_bytes, sizeof(random_bytes)));

  struct zmk_runtime_macro_auth_credential credential = make_credential(0x11U);
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_set_credential(NULL));
  credential.iterations = ZMK_RUNTIME_MACRO_AUTH_ITERATIONS_MIN - 1U;
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_set_credential(&credential));
  credential = make_credential(0x11U);
  memset(credential.salt, 0, sizeof(credential.salt));
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_set_credential(&credential));
  credential = make_credential(0x11U);
  memset(credential.key, 0, sizeof(credential.key));
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_set_credential(&credential));

  credential = make_credential(0x11U);
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
  EXPECT_EQ(1, save_calls);
  EXPECT_TRUE(strcmp(saved_name, "runtime_macro/auth/credential") == 0);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE, saved_length);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_VERSION, saved_value[0]);
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED, zmk_runtime_macro_auth_get_state());
  struct zmk_runtime_macro_auth_credential replacement = make_credential(0x22U);
  EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_set_credential(&replacement));
}

static void test_settings_load_and_fail_closed(void) {
  struct zmk_runtime_macro_auth_credential credential = make_credential(0x22U);
  struct zmk_runtime_macro_auth_info info;

  reset_auth();
  EXPECT_EQ(0, load_credential(&credential));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED, zmk_runtime_macro_auth_get_state());
  EXPECT_EQ(0, zmk_runtime_macro_auth_get_info(&info));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_KDF_ID, info.kdf_id);
  EXPECT_EQ(credential.iterations, info.iterations);
  EXPECT_TRUE(memcmp(info.salt, credential.salt, sizeof(info.salt)) == 0);

  reset_auth();
  const char *unknown_names[] = {
      "auth",
      "auth/credential/extra",
      "auth/credentialish",
      "authentic/credential",
  };
  for (size_t i = 0; i < sizeof(unknown_names) / sizeof(unknown_names[0]); i++) {
    EXPECT_EQ(-ENOENT, load_credential_named(unknown_names[i], &credential));
    EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN, zmk_runtime_macro_auth_get_state());
  }
  EXPECT_EQ(0, load_credential(&credential));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_PROTECTED, zmk_runtime_macro_auth_get_state());

  reset_auth();
  uint8_t serialized[ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_STORAGE_SIZE];
  serialize_credential(&credential, serialized);
  serialized[0]++;
  struct settings_reader reader = {
      .data = serialized,
      .length = sizeof(serialized),
  };
  EXPECT_EQ(-EINVAL, runtime_macro_settings_set("auth/credential", sizeof(serialized),
                                                 read_settings, &reader));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED, zmk_runtime_macro_auth_get_state());
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED, zmk_runtime_macro_auth_get_state());
  EXPECT_EQ(-EPERM, zmk_runtime_macro_auth_set_credential(&credential));

  reset_auth();
  reader.data = serialized;
  serialized[0] = ZMK_RUNTIME_MACRO_AUTH_CREDENTIAL_VERSION;
  memset(serialized + 5U + ZMK_RUNTIME_MACRO_AUTH_SALT_SIZE, 0,
         ZMK_RUNTIME_MACRO_AUTH_KEY_SIZE);
  EXPECT_EQ(-EINVAL, runtime_macro_settings_set("auth/credential", sizeof(serialized),
                                                 read_settings, &reader));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED, zmk_runtime_macro_auth_get_state());

  reset_auth();
  reader.data = serialized;
  reader.error = 0;
  reader.length = sizeof(serialized) - 1U;
  EXPECT_EQ(-EINVAL, runtime_macro_settings_set("auth/credential", sizeof(serialized),
                                                 read_settings, &reader));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED, zmk_runtime_macro_auth_get_state());

  reset_auth();
  reader.data = serialized;
  reader.length = sizeof(serialized);
  reader.error = -EIO;
  EXPECT_EQ(-EIO, runtime_macro_settings_set("auth/credential", sizeof(serialized),
                                             read_settings, &reader));
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_ERROR_LOCKED, zmk_runtime_macro_auth_get_state());

  reset_auth();
  EXPECT_EQ(0, zmk_runtime_macro_auth_settings_commit());
  EXPECT_EQ(ZMK_RUNTIME_MACRO_AUTH_STATE_OPEN, zmk_runtime_macro_auth_get_state());
}

static void test_challenge_proof_and_cooldown(void) {
  reset_auth();
  struct zmk_runtime_macro_auth_credential credential = make_credential(0x33U);
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
  for (size_t i = 0; i < sizeof(random_bytes); i++) {
    random_bytes[i] = (uint8_t)(0xa0U + i);
  }
  set_test_proof(0x40U);

  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_generate_challenge(NULL, sizeof(nonce)));
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce) - 1U));
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_TRUE(memcmp(nonce, random_bytes, sizeof(nonce)) == 0);
  EXPECT_EQ(0, hmac_calls);

  memcpy(proof, hmac_output, sizeof(proof));
  EXPECT_EQ(-EINVAL, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof) - 1U));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  EXPECT_EQ(1, hmac_calls);
  EXPECT_EQ(strlen("ZMK-RUNTIME-MACRO-AUTH-V2") + sizeof(nonce), hmac_last_input_length);
  EXPECT_TRUE(memcmp(hmac_last_input, "ZMK-RUNTIME-MACRO-AUTH-V2",
                     strlen("ZMK-RUNTIME-MACRO-AUTH-V2")) == 0);
  EXPECT_TRUE(memcmp(hmac_last_input + strlen("ZMK-RUNTIME-MACRO-AUTH-V2"), nonce,
                     sizeof(nonce)) == 0);
  EXPECT_TRUE(zmk_runtime_macro_auth_is_authenticated());
  EXPECT_EQ(-ENOENT, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));

  zmk_runtime_macro_auth_lock();
  proof[0] ^= 0x01U;
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  zmk_runtime_macro_auth_lock();
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());
  EXPECT_EQ(-EAGAIN, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  host_uptime += 1000;
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  proof[0] ^= 0x01U;
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));

  zmk_runtime_macro_auth_lock();
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  host_uptime += CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT * 1000;
  unsigned int hmac_calls_before_expiry = hmac_calls;
  EXPECT_EQ(-ENOENT, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  EXPECT_EQ(hmac_calls_before_expiry, hmac_calls);
}

static void test_cooldown_schedule_and_hmac_error(void) {
  reset_auth();
  struct zmk_runtime_macro_auth_credential credential = make_credential(0x77U);
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
  set_test_proof(0x80U);

  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  memcpy(proof, hmac_output, sizeof(proof));
  for (uint8_t failure = 1U; failure <= 4U; failure++) {
    EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
    proof[failure == 1U ? 0U : ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE - 1U] ^= 0x01U;
    int64_t expected_deadline = host_uptime + ((int64_t)1U << (failure - 1U)) * 1000;
    EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
    EXPECT_EQ(failure, runtime_macro_auth.failure_count);
    EXPECT_EQ(expected_deadline, runtime_macro_auth.cooldown_deadline_ms);
    EXPECT_EQ(-EAGAIN, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
    proof[failure == 1U ? 0U : ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE - 1U] ^= 0x01U;
    host_uptime = expected_deadline;
  }

  hmac_result = -EIO;
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(-EIO, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  EXPECT_EQ(-ENOENT, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  hmac_result = 0;

  zmk_runtime_macro_auth_lock();
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE - 1U] ^= 0x01U;
  EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  EXPECT_EQ(-EAGAIN, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  zmk_runtime_macro_auth_transport_reset();
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
}

static void test_rng_failure_and_session_lifecycle(void) {
  reset_auth();
  struct zmk_runtime_macro_auth_credential credential = make_credential(0x44U);
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&credential));
  set_test_proof(0x50U);
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  random_result = -EIO;
  EXPECT_EQ(-EIO, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_TRUE(runtime_macro_auth_all_zero(nonce, sizeof(nonce)));
  memcpy(proof, hmac_output, sizeof(proof));
  EXPECT_EQ(-ENOENT, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));

  random_result = 0;
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  EXPECT_TRUE(zmk_runtime_macro_auth_is_authenticated());
  host_uptime += (CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT - 1) * 1000;
  EXPECT_EQ(0, zmk_runtime_macro_auth_refresh_session());
  host_uptime += CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT * 1000;
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());
  EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_refresh_session());

  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(proof, sizeof(proof)));
  zmk_runtime_macro_auth_transport_reset();
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
}

static void test_storage_first_and_key_change(void) {
  reset_auth();
  struct zmk_runtime_macro_auth_credential old_credential = make_credential(0x11U);
  struct zmk_runtime_macro_auth_credential new_credential = make_credential(0x22U);
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&old_credential));
  set_test_proof(0x60U);
  authenticate();
  uint8_t nonce[ZMK_RUNTIME_MACRO_AUTH_NONCE_SIZE];
  uint8_t old_proof[ZMK_RUNTIME_MACRO_AUTH_PROOF_SIZE];
  memcpy(old_proof, hmac_output, sizeof(old_proof));

  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  save_result = -EIO;
  EXPECT_EQ(-EIO, zmk_runtime_macro_auth_set_credential(&new_credential));
  EXPECT_TRUE(zmk_runtime_macro_auth_is_authenticated());
  EXPECT_TRUE(memcmp(hmac_last_key, old_credential.key, sizeof(hmac_last_key)) == 0);

  save_result = 0;
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&new_credential));
  set_test_proof(0x70U);
  EXPECT_TRUE(!zmk_runtime_macro_auth_is_authenticated());
  EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_set_credential(&old_credential));

  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(-EACCES, zmk_runtime_macro_auth_verify_proof(old_proof, sizeof(old_proof)));
  host_uptime += 1000;
  EXPECT_EQ(0, zmk_runtime_macro_auth_generate_challenge(nonce, sizeof(nonce)));
  EXPECT_EQ(0, zmk_runtime_macro_auth_verify_proof(hmac_output, sizeof(old_proof)));
  EXPECT_TRUE(memcmp(hmac_last_key, new_credential.key, sizeof(hmac_last_key)) == 0);
}

struct credential_thread_arg {
  struct zmk_runtime_macro_auth_credential credential;
  int result;
};

static void *set_credential_thread(void *arg) {
  struct credential_thread_arg *thread_arg = arg;
  thread_arg->result = zmk_runtime_macro_auth_set_credential(&thread_arg->credential);
  return NULL;
}

static bool backend_first_entered(void) {
  pthread_mutex_lock(&backend_mutex);
  bool entered = first_save_entered;
  pthread_mutex_unlock(&backend_mutex);
  return entered;
}

static unsigned int backend_call_count(void) {
  pthread_mutex_lock(&backend_mutex);
  unsigned int calls = save_calls;
  pthread_mutex_unlock(&backend_mutex);
  return calls;
}

static void test_concurrent_credential_updates_are_serialized(void) {
  reset_auth();
  struct zmk_runtime_macro_auth_credential original = make_credential(0x44U);
  EXPECT_EQ(0, zmk_runtime_macro_auth_set_credential(&original));
  reset_backend();
  set_test_proof(0x50U);
  authenticate();

  struct credential_thread_arg first = {.credential = make_credential(0x55U)};
  struct credential_thread_arg second = {.credential = make_credential(0x66U)};
  pthread_t first_thread;
  pthread_t second_thread;

  pthread_mutex_lock(&backend_mutex);
  block_first_save = true;
  save_result = -EIO;
  pthread_mutex_unlock(&backend_mutex);
  EXPECT_EQ(0, pthread_create(&first_thread, NULL, set_credential_thread, &first));
  while (!backend_first_entered()) {
    sched_yield();
  }
  EXPECT_EQ(0, pthread_create(&second_thread, NULL, set_credential_thread, &second));
  for (unsigned int i = 0; i < 1000U; i++) {
    if (backend_call_count() != 1U) {
      break;
    }
    sched_yield();
  }
  EXPECT_EQ(1, backend_call_count());

  pthread_mutex_lock(&backend_mutex);
  save_result = 0;
  release_first_save = true;
  pthread_cond_broadcast(&backend_condition);
  pthread_mutex_unlock(&backend_mutex);
  EXPECT_EQ(0, pthread_join(first_thread, NULL));
  EXPECT_EQ(0, pthread_join(second_thread, NULL));
  EXPECT_EQ(-EIO, first.result);
  EXPECT_EQ(0, second.result);
  EXPECT_EQ(2, backend_call_count());
  struct zmk_runtime_macro_auth_info info;
  EXPECT_EQ(0, zmk_runtime_macro_auth_get_info(&info));
  EXPECT_TRUE(memcmp(info.salt, second.credential.salt, sizeof(info.salt)) == 0);
}

int main(void) {
  test_open_and_credential_validation();
  test_settings_load_and_fail_closed();
  test_challenge_proof_and_cooldown();
  test_cooldown_schedule_and_hmac_error();
  test_rng_failure_and_session_lifecycle();
  test_storage_first_and_key_change();
  test_concurrent_credential_updates_are_serialized();

  if (failures != 0) {
    fprintf(stderr, "runtime macro authentication tests: %d failure(s)\n", failures);
    return 1;
  }

  printf("runtime macro authentication tests: PASS\n");
  return 0;
}
