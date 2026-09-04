#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef int64_t k_timeout_t;

typedef int32_t atomic_t;
typedef int32_t atomic_val_t;

#define K_FOREVER ((k_timeout_t) - 1)
#define K_NO_WAIT ((k_timeout_t)0)
#define K_MSEC(ms) ((k_timeout_t)(ms))

static inline bool atomic_cas(atomic_t *target, atomic_val_t old_value, atomic_val_t new_value) {
    if (*target != old_value) {
        return false;
    }

    *target = new_value;
    return true;
}

static inline atomic_val_t atomic_get(const atomic_t *target) { return *target; }
static inline atomic_val_t atomic_set(atomic_t *target, atomic_val_t value) {
    atomic_val_t old_value = *target;
    *target = value;
    return old_value;
}

struct k_mutex {
    pthread_mutex_t native;
};

#define K_MUTEX_DEFINE(name) struct k_mutex name = {.native = PTHREAD_MUTEX_INITIALIZER}

static inline int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout) {
    (void)timeout;
    return pthread_mutex_lock(&mutex->native);
}

static inline int k_mutex_unlock(struct k_mutex *mutex) {
    return pthread_mutex_unlock(&mutex->native);
}

struct k_work {
    void (*handler)(struct k_work *work);
};

struct k_work_delayable {
    struct k_work work;
    bool scheduled;
    k_timeout_t delay;
};

extern int host_work_schedule(struct k_work_delayable *work, k_timeout_t delay, bool reschedule);

#define K_WORK_DELAYABLE_DEFINE(name, work_handler)                                                \
    struct k_work_delayable name = {.work = {.handler = work_handler}}

static inline int k_work_schedule(struct k_work_delayable *work, k_timeout_t delay) {
    return host_work_schedule(work, delay, false);
}

static inline int k_work_reschedule(struct k_work_delayable *work, k_timeout_t delay) {
    return host_work_schedule(work, delay, true);
}

extern int64_t host_uptime;
static inline int64_t k_uptime_get(void) { return host_uptime; }

#define ARG_UNUSED(x) (void)(x)
