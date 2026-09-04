#pragma once

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

typedef int64_t k_timeout_t;

typedef int32_t atomic_t;
typedef int32_t atomic_val_t;

#define K_FOREVER ((k_timeout_t) - 1)
#define K_NO_WAIT ((k_timeout_t)0)
#define K_MSEC(ms) ((k_timeout_t)(ms))

static inline bool atomic_cas(atomic_t *target, atomic_val_t old_value,
                              atomic_val_t new_value) {
  if (*target != old_value) {
    return false;
  }

  *target = new_value;
  return true;
}

static inline atomic_val_t atomic_get(const atomic_t *target) {
  return *target;
}
static inline atomic_val_t atomic_set(atomic_t *target, atomic_val_t value) {
  atomic_val_t old_value = *target;
  *target = value;
  return old_value;
}

struct k_mutex {
  pthread_mutex_t native;
};

#define K_MUTEX_DEFINE(name)                                                   \
  struct k_mutex name = {.native = PTHREAD_MUTEX_INITIALIZER}

static inline int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout) {
  (void)timeout;
  return pthread_mutex_lock(&mutex->native);
}

static inline int k_mutex_unlock(struct k_mutex *mutex) {
  return pthread_mutex_unlock(&mutex->native);
}

struct k_sem {
  unsigned int count;
  unsigned int limit;
};

#define K_SEM_DEFINE(name, initial_count, count_limit)                         \
  struct k_sem name = {.count = (initial_count), .limit = (count_limit)}

static inline int k_sem_take(struct k_sem *sem, k_timeout_t timeout) {
  (void)timeout;
  if (sem->count == 0U) {
    return -EBUSY;
  }

  sem->count--;
  return 0;
}

static inline void k_sem_give(struct k_sem *sem) {
  if (sem->count < sem->limit) {
    sem->count++;
  }
}

static inline void k_sem_reset(struct k_sem *sem) { sem->count = 0U; }

struct k_msgq {
  uint8_t *buffer;
  size_t msg_size;
  size_t max_msgs;
  size_t used_msgs;
  size_t head;
  size_t tail;
};

#define K_MSGQ_DEFINE(name, message_size, max_messages, alignment)             \
  static uint8_t name##_buffer[(message_size) * (max_messages)];               \
  struct k_msgq name = {.buffer = name##_buffer,                               \
                        .msg_size = (message_size),                            \
                        .max_msgs = (max_messages)}

static inline int k_msgq_put(struct k_msgq *msgq, const void *data,
                             k_timeout_t timeout) {
  (void)timeout;
  if (msgq->used_msgs >= msgq->max_msgs) {
    return -ENOMSG;
  }

  memcpy(msgq->buffer + msgq->tail * msgq->msg_size, data, msgq->msg_size);
  msgq->tail = (msgq->tail + 1U) % msgq->max_msgs;
  msgq->used_msgs++;
  return 0;
}

static inline int k_msgq_get(struct k_msgq *msgq, void *data,
                             k_timeout_t timeout) {
  (void)timeout;
  if (msgq->used_msgs == 0U) {
    return -ENOMSG;
  }

  memcpy(data, msgq->buffer + msgq->head * msgq->msg_size, msgq->msg_size);
  msgq->head = (msgq->head + 1U) % msgq->max_msgs;
  msgq->used_msgs--;
  return 0;
}

static inline void k_msgq_purge(struct k_msgq *msgq) {
  msgq->used_msgs = 0U;
  msgq->head = 0U;
  msgq->tail = 0U;
}

struct k_work {
  void (*handler)(struct k_work *work);
  bool submitted;
};

extern int host_work_submit(struct k_work *work);

#define K_WORK_DEFINE(name, work_handler)                                      \
  struct k_work name = {.handler = (work_handler)}

static inline int k_work_submit(struct k_work *work) {
  return host_work_submit(work);
}

struct k_work_delayable {
  struct k_work work;
  bool scheduled;
  k_timeout_t delay;
};

extern int host_work_schedule(struct k_work_delayable *work, k_timeout_t delay,
                              bool reschedule);

#define K_WORK_DELAYABLE_DEFINE(name, work_handler)                            \
  struct k_work_delayable name = {.work = {.handler = work_handler}}

static inline int k_work_schedule(struct k_work_delayable *work,
                                  k_timeout_t delay) {
  return host_work_schedule(work, delay, false);
}

static inline int k_work_reschedule(struct k_work_delayable *work,
                                    k_timeout_t delay) {
  return host_work_schedule(work, delay, true);
}

extern int64_t host_uptime;
static inline int64_t k_uptime_get(void) { return host_uptime; }

#define ARG_UNUSED(x) (void)(x)
