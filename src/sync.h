#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>
#include "task.h"

/* Every blocking object starts with this header. A blocked task stores a
   `void *blocked_on` pointing at it, and the inheritance walk needs to ask
   "is the thing this task is waiting for a mutex, and if so who owns it?".
   The tag is what makes that question answerable from a void pointer. */
typedef enum {
    SYNC_TYPE_SEM,
    SYNC_TYPE_MUTEX
} sync_type_t;

typedef struct {
    sync_type_t type;
} sync_object_t;

/* --- Binary semaphore ----------------------------------------------------
 * A pure signal. It has a count but no owner, so it cannot participate in
 * priority inheritance -- there is nobody to boost. That is the whole reason
 * a mutex has to be a different type. */
typedef struct {
    sync_object_t obj;          /* must be first member */
    volatile int32_t count;
    int32_t max_count;
} semaphore_t;

/* --- Mutex ---------------------------------------------------------------
 * Ownership is the entire difference from a semaphore. It buys three things:
 *   1. only the owner may unlock                    (correctness)
 *   2. recursive self-lock is detectable            (deadlock avoidance)
 *   3. there is a task to boost on contention       (priority inheritance)
 */
typedef struct {
    sync_object_t obj;          /* must be first member */
    TCB *owner;                 /* NULL == unlocked */
} mutex_t;

void sem_init(semaphore_t *s, int32_t initial_count);
void sem_take(semaphore_t *s);
void sem_give(semaphore_t *s);

void mutex_init(mutex_t *m);

/* 0 on success, -1 if the caller already owns it (recursive lock). */
int  mutex_lock(mutex_t *m);

/* 0 on success, -1 if the caller is not the owner. */
int  mutex_unlock(mutex_t *m);

/* Runtime toggle for the milestone. Flipping this changes nothing about the
   scheduler -- only whether effective_priority is ever raised. */
void sync_set_inheritance(int enabled);
int  sync_get_inheritance(void);

/* Counters for anything the demo did that should be impossible. */
extern volatile uint32_t sync_error_recursive_lock;
extern volatile uint32_t sync_error_bad_unlock;
extern volatile uint32_t sync_inherit_depth_exceeded;

#endif
