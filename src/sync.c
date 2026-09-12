#include "sync.h"
#include "task.h"
#include "port.h"
#include "gpio.h"
#include <stddef.h>

/* Guard against a cycle of mutex owners (which would be a deadlock the demo
   has no business creating). Bounds the walk so a bug cannot hang the tick. */
#define INHERIT_MAX_DEPTH 8

/* CALLER CONTRACT for sem_take() and mutex_lock():
 *
 * Do not call them from inside a critical section you opened yourself.
 * Both block by dropping the critical section so that PendSV can fire. If the
 * caller already held one, the nesting count never reaches zero, interrupts
 * stay masked, PendSV never runs -- and the task spins forever inside the
 * retry loop while marked TASK_BLOCKED. It is a hang, not a crash, which makes
 * it unpleasant to find.
 *
 * This is the general rule that critical sections must be short and must not
 * contain anything that can block, stated in terms of this kernel. */

static volatile int inheritance_enabled = 1;

volatile uint32_t sync_error_recursive_lock   = 0;
volatile uint32_t sync_error_bad_unlock       = 0;
volatile uint32_t sync_inherit_depth_exceeded = 0;

void sync_set_inheritance(int enabled)
{
    critical_section_enter();
    inheritance_enabled = enabled;
    if (enabled) {
        trace_pin_set(TRACE_PIN_INHERIT);
    } else {
        trace_pin_clear(TRACE_PIN_INHERIT);
    }
    critical_section_exit();
}

int sync_get_inheritance(void)
{
    return inheritance_enabled;
}

/* ------------------------------------------------------------------------
 * Wait-queue operations.
 *
 * There is no queue. "Who is waiting on X" is answered by scanning task_list
 * for blocked_on == X. At MAX_TASKS == 8 that is cheaper than maintaining
 * linked lists, and it removes a whole class of list-surgery bugs -- notably
 * the one where a waiter's priority is boosted and a priority-ordered list
 * silently stops being ordered.
 * ---------------------------------------------------------------------- */

/* Highest-effective-priority waiter, or NULL. Picking by priority rather than
   FIFO matters: a FIFO wake can leave a high-priority task queued behind a
   low-priority one, which is its own flavour of inversion. */
static TCB *highest_priority_waiter(sync_object_t *obj)
{
    TCB *best = NULL;
    uint32_t n = task_get_count();

    for (uint32_t i = 0; i < n; i++) {
        TCB *t = task_get_by_index(i);
        if (t == NULL) continue;
        if (t->state != TASK_BLOCKED) continue;
        if (t->blocked_on != (void *)obj) continue;

        if (best == NULL || t->effective_priority > best->effective_priority) {
            best = t;
        }
    }
    return best;
}

static void wake_waiter(TCB *t)
{
    if (t == NULL) return;

    /* Deferred wake: clear the block and mark READY, but do NOT switch. The
       next SysTick runs the scheduler and picks it up. Costs up to one tick
       of latency; see the asymmetry note on the blocking path below. */
    t->blocked_on = NULL;
    t->state      = TASK_READY;
}

/* ------------------------------------------------------------------------
 * Priority inheritance
 * ---------------------------------------------------------------------- */

/* A task just blocked on a mutex. Push its priority down the ownership chain.
 *
 * Transitive case: High blocks on M1 owned by Mid, and Mid is itself blocked
 * on M2 owned by Low. Boosting only Mid achieves nothing -- Mid is BLOCKED and
 * cannot run, so it cannot release M1. The boost has to reach Low, the task
 * that is actually runnable. */
static void inherit_propagate(TCB *blocked)
{
    TCB *waiter = blocked;

    for (uint32_t depth = 0; depth < INHERIT_MAX_DEPTH; depth++) {

        if (waiter->blocked_on == NULL) return;

        sync_object_t *obj = (sync_object_t *)waiter->blocked_on;

        /* Semaphores have no owner, so the chain simply ends here. This is
           exactly why unbounded inversion is unfixable on a plain semaphore:
           there is no task to boost. */
        if (obj->type != SYNC_TYPE_MUTEX) return;

        TCB *owner = ((mutex_t *)obj)->owner;
        if (owner == NULL) return;

        /* Owner already runs at least this high -- so does everything further
           up the chain, since each link was raised to at least its waiter. */
        if (owner->effective_priority >= waiter->effective_priority) return;

        owner->effective_priority = waiter->effective_priority;

        waiter = owner;   /* follow the chain one more link */
    }

    sync_inherit_depth_exceeded++;
}

/* Recompute a task's effective priority from scratch after it releases a mutex.
 *
 * The naive version of this is `t->effective_priority = t->base_priority`, and
 * it is wrong whenever a task holds more than one mutex: dropping straight back
 * to base re-exposes the task to preemption by a medium task while a high task
 * is still waiting on a DIFFERENT mutex it still holds -- recreating the exact
 * inversion inheritance was meant to remove.
 *
 * Correct rule:
 *   effective = max(base, max priority of every task still blocked on a mutex
 *                          this task still owns)
 */
static void inherit_recompute(TCB *t)
{
    uint32_t p = t->base_priority;
    uint32_t n = task_get_count();

    for (uint32_t i = 0; i < n; i++) {
        TCB *w = task_get_by_index(i);
        if (w == NULL || w == t) continue;
        if (w->blocked_on == NULL) continue;

        sync_object_t *obj = (sync_object_t *)w->blocked_on;
        if (obj->type != SYNC_TYPE_MUTEX) continue;
        if (((mutex_t *)obj)->owner != t) continue;   /* not one of mine */

        if (w->effective_priority > p) {
            p = w->effective_priority;
        }
    }

    t->effective_priority = p;
}

/* ------------------------------------------------------------------------
 * Binary semaphore
 * ---------------------------------------------------------------------- */

void sem_init(semaphore_t *s, int32_t initial_count)
{
    s->obj.type  = SYNC_TYPE_SEM;
    s->max_count = 1;                              /* binary */
    s->count     = (initial_count > 0) ? 1 : 0;
}

void sem_take(semaphore_t *s)
{
    critical_section_enter();

    /* `while`, not `if`. Two reasons, both real:
       - the wake is deferred, so between sem_give() marking us READY and us
         actually running, another task can consume the count;
       - re-checking the predicate on wake-up is the only thing that makes a
         spurious wake harmless rather than a silent double-entry. */
    while (s->count == 0) {

        current_task->blocked_on = &s->obj;
        current_task->state      = TASK_BLOCKED;

        /* The blocking path MUST switch immediately. If we merely marked
           ourselves BLOCKED and returned, control would carry on into the
           critical section this semaphore is supposed to be guarding. */
        commit_switch_to(scheduler());

        critical_section_exit();   /* PendSV fires here; we lose the CPU */
        /* ... later, someone gave the semaphore and the tick rescheduled us */
        critical_section_enter();
    }

    s->count--;

    critical_section_exit();
}

void sem_give(semaphore_t *s)
{
    critical_section_enter();

    if (s->count < s->max_count) {
        s->count++;
    }
    /* else: saturate. Giving a binary semaphore twice is not an error, but the
       second give cannot be banked -- that is what makes it binary. */

    wake_waiter(highest_priority_waiter(&s->obj));

    critical_section_exit();
}

/* ------------------------------------------------------------------------
 * Mutex
 * ---------------------------------------------------------------------- */

void mutex_init(mutex_t *m)
{
    m->obj.type = SYNC_TYPE_MUTEX;
    m->owner    = NULL;
}

int mutex_lock(mutex_t *m)
{
    critical_section_enter();

    /* Recursive lock. Not supported: we would have to count acquisitions, and
       unlock would have to know not to release until the count hit zero. We
       detect it instead of deadlocking on ourselves. */
    if (m->owner == current_task) {
        sync_error_recursive_lock++;
        critical_section_exit();
        return -1;
    }

    while (m->owner != NULL) {

        current_task->blocked_on = &m->obj;
        current_task->state      = TASK_BLOCKED;

        /* Boost AFTER blocked_on is set: inherit_propagate() walks the chain
           by following blocked_on pointers, so this task must already be
           linked into the chain for the walk to see it. */
        if (inheritance_enabled) {
            inherit_propagate(current_task);
        }

        commit_switch_to(scheduler());

        critical_section_exit();
        critical_section_enter();
    }

    m->owner = current_task;
    trace_pin_set(TRACE_PIN_MUTEX);

    critical_section_exit();
    return 0;
}

int mutex_unlock(mutex_t *m)
{
    critical_section_enter();

    /* Ownership check. A semaphore cannot do this -- any task may give a
       semaphore it never took, and that is a legitimate use (ISR signalling a
       task). A mutex protects data, so releasing one you do not hold is always
       a bug and is worth catching. */
    if (m->owner != current_task) {
        sync_error_bad_unlock++;
        critical_section_exit();
        return -1;
    }

    TCB *me = current_task;

    /* Order matters. Release ownership first so that inherit_recompute() does
       not count waiters on THIS mutex -- we are handing it over, their claim
       on our priority ends now. */
    m->owner = NULL;
    trace_pin_clear(TRACE_PIN_MUTEX);

    wake_waiter(highest_priority_waiter(&m->obj));

    /* Then drop back down -- to whatever our remaining obligations require,
       which is not necessarily base_priority. */
    if (inheritance_enabled) {
        inherit_recompute(me);
    }

    /* We are now possibly below a task that just became READY. We keep running
       until the next tick notices. Bounded by one tick (1 ms) by construction,
       which is the price paid for the deferred wake. */

    critical_section_exit();
    return 0;
}
