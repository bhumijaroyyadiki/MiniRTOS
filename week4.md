# Week 4 — Synchronization, Priority Inversion, and Priority Inheritance

Full explanation of everything added in week 4: what the code does, why each
design decision was made, what was rejected, and how to demonstrate it.

> **Build status:** the week 4 code has been written but **not yet compiled or
> run** — the development machine had no `arm-none-eabi-gcc`. All timing figures
> in this document are derived by hand from the scheduler's rules, not measured.
> Calibrate and confirm on hardware before quoting them.

---

## Table of contents

1. [What week 4 adds](#1-what-week-4-adds)
2. [Critical sections](#2-critical-sections)
3. [The blocking-state problem](#3-the-blocking-state-problem)
4. [Binary semaphore](#4-binary-semaphore)
5. [Mutex and ownership](#5-mutex-and-ownership)
6. [Priority inversion](#6-priority-inversion)
7. [Priority inheritance](#7-priority-inheritance)
8. [The demo scenario](#8-the-demo-scenario)
9. [Instrumentation and tracing](#9-instrumentation-and-tracing)
10. [Bugs fixed from weeks 1–3](#10-bugs-fixed-from-weeks-13)
11. [Design decision register](#11-design-decision-register)
12. [Tick-by-tick flow traces](#12-tick-by-tick-flow-traces)
13. [Known limitations](#13-known-limitations)
14. [Build and run](#14-build-and-run)
15. [Interview questions](#15-interview-questions)

---

## 1. What week 4 adds

Weeks 1–3 produced a preemptive priority scheduler: tasks, a PendSV context
switch, a 1 ms SysTick, `rtos_delay()`, and round-robin within a priority level.
Everything a task did was independent of every other task.

Week 4 introduces **sharing**, and sharing introduces three new problems in
sequence:

| Problem | Mechanism |
|---|---|
| Two tasks touching the same data concurrently | Critical sections |
| A task needing to wait for an event that hasn't happened | Binary semaphore |
| A task needing exclusive use of a resource, safely | Mutex with ownership |
| A high-priority task starved by an unrelated medium task | **Priority inheritance** |

The last one is the headline. The first three exist to make it possible.

### Files

| File | Status | Contents |
|---|---|---|
| `src/sync.h` / `src/sync.c` | new | Semaphore, mutex, priority inheritance |
| `src/demo.h` / `src/demo.c` | new | The three-task inversion scenario |
| `src/task.h` / `src/task.c` | modified | Split priority, `blocked_on`, scheduler change |
| `src/port.h` / `src/port.c` | modified | Interrupt intrinsics, critical section fixes |
| `src/gpio.h` / `src/gpio.c` | modified | Atomic BSRR trace pins |
| `src/main.c` | modified | Init-order fixes |
| `src/svc.S` | modified | Bootstrap from `current_task` |

---

## 2. Critical sections

### The problem

The kernel has data structures that more than one context touches:
`task_list[]`, each TCB's `state`, `next_task`, a semaphore's `count`, a mutex's
`owner`. Two contexts can reach them:

- **Thread context** — a task calling `sem_take()`, `mutex_lock()`, `yield()`.
- **Handler context** — `SysTick_Handler()` running `task_check_wakeups()` and
  `scheduler()`.

A tick can land between any two instructions of a task. If a task is halfway
through updating `s->count` when SysTick fires and the scheduler runs, the
kernel's view of the world is inconsistent.

### The mechanism

On Cortex-M, the cheapest mutual exclusion against interrupts is to mask them:

```c
static inline void __disable_irq(void) { __asm volatile ("cpsid i" : : : "memory"); }
static inline void __enable_irq(void)  { __asm volatile ("cpsie i" : : : "memory"); }
```
*(`port.h`)*

`cpsid i` sets PRIMASK, which blocks every configurable-priority interrupt
(SysTick and PendSV included; NMI and HardFault are unmaskable).

**The `"memory"` clobber is not decoration.** Without it the compiler is free to
move a load or a store across the `asm` statement, which would put access to
shared state *outside* the window where interrupts were actually off. The
clobber tells GCC "this instruction may read or write any memory," forcing it to
commit pending stores before the barrier and reload after it.

### Nesting

A naive implementation is wrong:

```c
void enter(void) { __disable_irq(); }
void exit(void)  { __enable_irq(); }      // WRONG
```

If `mutex_lock()` opens a critical section and calls a helper that opens and
closes its own, the helper's `exit` re-enables interrupts while the outer
section is still open. The fix is a depth counter:

```c
static volatile uint32_t critical_nesting = 0;

void critical_section_enter(void)
{
    __disable_irq();          /* unconditionally, BEFORE touching the counter */
    critical_nesting++;
}

void critical_section_exit(void)
{
    if (critical_nesting > 0) {
        critical_nesting--;
    }
    if (critical_nesting == 0) {
        __enable_irq();       /* only on the outermost exit */
    }
}
```
*(`port.c`)*

Order matters in `enter`: disable *first*, then increment. Incrementing first
would leave a window where an interrupt could observe a non-zero nesting count
with interrupts still enabled.

### Why one global counter is correct

`critical_nesting` is a single global shared by every task, which looks like a
bug — surely each task needs its own depth?

It is correct because of one invariant:

> **A context switch can only occur when `critical_nesting == 0`.**

Switches happen via PendSV. PendSV is an interrupt. Interrupts are masked
whenever `critical_nesting > 0`. Therefore every switch point observes the
counter at zero, and no task can ever inherit or observe another task's depth.

The blocking primitives in `sync.c` uphold this deliberately: they call
`critical_section_exit()` *before* giving up the CPU, precisely so the counter
is zero at the handover.

If you ever add a path that switches with nesting > 0, this counter becomes
per-task state and must move into the TCB. That is exactly what FreeRTOS does
with `uxCriticalNesting`.

### Keeping them short

Every instruction inside a critical section is a instruction during which the
system cannot respond to *anything*. Interrupt latency for the whole system is
bounded below by the longest critical section in the codebase.

The rules this codebase follows:

- No loops of unbounded length inside one.
- No blocking calls inside one (see the caller contract in `sync.c`).
- The longest one in the kernel is `highest_priority_waiter()`, bounded by
  `MAX_TASKS` = 8 iterations.

---

## 3. The blocking-state problem

This is the single most important design decision in week 4, and it is a
prerequisite for the semaphore working at all.

### The latent bug

Weeks 1–3 had exactly one reason to block: `rtos_delay()`. So the TCB carried a
`waketick`, and the tick handler released anything whose deadline had passed:

```c
/* week 3 version */
if (tcb->state == TASK_BLOCKED && tcb->waketick <= tick_count)
    tcb->state = TASK_READY;
```

Week 4 introduces a second, categorically different reason to block: *waiting
indefinitely for an event*. A task blocked on a semaphore is also
`TASK_BLOCKED`, and its `waketick` holds a stale value from whenever it last
called `rtos_delay()` — a tick already in the past.

So `waketick <= tick_count` is **true**, and the tick handler releases a
semaphore waiter on the very next tick, with nobody having given the semaphore.
`sem_take()` returns, the task walks into the critical section it was supposed
to be excluded from, and two tasks corrupt the shared data.

The failure looks like an application race. It is a kernel bug.

### The fix

The TCB gains one field:

```c
/* NULL  -> runnable, or sleeping on a deadline (see waketick).
   !NULL -> blocked indefinitely on this sync object; waketick is meaningless. */
void *blocked_on;
```
*(`task.h`)*

and the tick handler gains one clause:

```c
if (tcb->state == TASK_BLOCKED &&
    tcb->blocked_on == NULL &&                          /* <- the fix */
    (int32_t)(tcb->waketick - tick_count) <= 0)
{
    tcb->state = TASK_READY;
}
```
*(`task.c`, `task_check_wakeups`)*

Two orthogonal axes now exist:

| | `blocked_on == NULL` | `blocked_on != NULL` |
|---|---|---|
| **state READY/RUNNING** | runnable | *(impossible)* |
| **state BLOCKED** | sleeping until `waketick` | waiting on an object, no deadline |

The empty cell is why a single `void *` is enough rather than a new state enum
value — and the reason it was chosen over `TASK_BLOCKED_SEM` is that it scales:
adding "wait on this object, but give up after N ticks" later means using both
fields at once, which a state enum cannot express without a combinatorial
explosion of states.

`blocked_on` is `void *` rather than a typed pointer because it must hold either
a `semaphore_t *` or a `mutex_t *`. Recovering the type is handled in §5.

### Signed tick comparison

Both `task_check_wakeups()` and `rtos_delay_until()` compare deadlines like this:

```c
(int32_t)(tcb->waketick - tick_count) <= 0
```

not `waketick <= tick_count`. `tick_count` is a 32-bit counter incrementing at
1 kHz; it wraps after ~49.7 days. With a plain `<=`, a deadline of `0x00000005`
set just before a wrap would compare as *enormously far in the future* against a
`tick_count` of `0xFFFFFFF0`, and the task would sleep for 49 days.

Subtracting first and casting to signed makes the comparison relative: the
unsigned difference wraps correctly, and the signed cast interprets "small
positive" as future and "small negative" as past. This is correct for any
deadline within ±2^31 ticks (~24 days) of now, which is the standard bound.

---

## 4. Binary semaphore

### What it is

A signal with a count of 0 or 1. `take` consumes it, blocking if it's zero;
`give` produces it, saturating at one.

```c
typedef struct {
    sync_object_t obj;          /* must be first member */
    volatile int32_t count;
    int32_t max_count;
} semaphore_t;
```
*(`sync.h`)*

### `sem_take`

```c
void sem_take(semaphore_t *s)
{
    critical_section_enter();

    while (s->count == 0) {

        current_task->blocked_on = &s->obj;
        current_task->state      = TASK_BLOCKED;

        commit_switch_to(scheduler());      /* MUST switch immediately */

        critical_section_exit();            /* PendSV fires here */
        /* ... we lose the CPU; later someone gave it and the tick woke us */
        critical_section_enter();
    }

    s->count--;

    critical_section_exit();
}
```
*(`sync.c`)*

Three things in this function are worth defending:

**(a) `while`, not `if`.** Re-checking the predicate after waking is mandatory
here, for two independent reasons:

1. The wake is *deferred* (§4.3). Between `sem_give()` marking us READY and us
   actually being scheduled — up to one tick later — another task can run and
   consume the count. We must re-check or we'd decrement it to −1.
2. Re-checking makes a spurious wake harmless. Any future code path that sets a
   task READY by mistake causes a wasted context switch instead of a silent
   double-entry into a protected region.

This is the same reason `pthread_cond_wait` must be called in a loop.

**(b) The blocking path switches immediately.** `commit_switch_to()` is called
*inside* `sem_take`, before returning. If instead the function merely set
`TASK_BLOCKED` and returned, control would carry on into the critical section
the semaphore was guarding — while notionally blocked. The blocking task must
not execute another instruction of its own code.

**(c) The critical-section dance.** `exit` then `enter` around the switch point
looks odd, but it is the mechanism: `commit_switch_to()` only *pends* PendSV.
PendSV cannot fire while interrupts are masked. `critical_section_exit()` drops
the mask, PendSV fires on that instruction, and we lose the CPU exactly there.
When we are eventually restored, execution resumes at `critical_section_enter()`
and the loop re-tests.

This is also why blocking inside a caller-held critical section hangs: the
nesting count never reaches zero, PendSV never fires, and the task spins forever
marked `TASK_BLOCKED`. That contract is documented at the top of `sync.c`.

### `sem_give`

```c
void sem_give(semaphore_t *s)
{
    critical_section_enter();

    if (s->count < s->max_count) {
        s->count++;
    }
    /* else: saturate — a second give cannot be banked. That is what makes it binary. */

    wake_waiter(highest_priority_waiter(&s->obj));

    critical_section_exit();
}
```

`give` is deliberately not symmetric with `take`:

- It never blocks. Giving a full binary semaphore is not an error, it just has
  no additional effect — which is what you want when an ISR signals a task that
  may already be signalled.
- It has no ownership check. Any task (or ISR) may give a semaphore it never
  took. That asymmetry is the *legitimate* use of a semaphore and is precisely
  what distinguishes it from a mutex (§5).

### Deferred wake

`wake_waiter()` clears the block and marks READY, but does **not** switch:

```c
static void wake_waiter(TCB *t)
{
    if (t == NULL) return;
    t->blocked_on = NULL;
    t->state      = TASK_READY;
}
```

The next SysTick runs the scheduler and picks the woken task up. **Cost: up to
one tick (1 ms) of latency on every handover.**

There is a real consequence: for up to 1 ms after a `give`, the kernel is
running a lower-priority task while a higher-priority task is READY. That is a
genuine violation of the scheduling invariant — but it is **bounded by one
tick**, which is categorically different from the unbounded inversion this whole
week exists to fix.

The alternative (run `scheduler()` inside `give` and pend PendSV if the woken
task outranks the current one) removes the latency at the cost of more work on
the give path. It was rejected here for simplicity; the demo's critical sections
are 200 ms, so 1 ms of jitter is noise. **If you shorten the critical section
below ~10 ms, revisit this** — the jitter would start to dominate the
measurement.

### Waiter selection

```c
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
```

**There is no wait queue.** "Who is waiting on X" is answered by scanning
`task_list[]` for `blocked_on == X`.

Why this beats a linked list at this scale:

- `MAX_TASKS` is 8. An 8-iteration scan is cheaper than the pointer surgery of
  maintaining an intrusive list, and it happens only on give/unlock.
- No memory overhead in either the TCB or the sync object.
- **It removes a bug class.** A priority-ordered intrusive list stops being
  ordered the moment a waiter's priority is boosted by inheritance — you would
  have to re-sort the list on every boost. Scanning and taking the max is
  immune: it reads the current priority every time.

Selection is by **highest effective priority, not FIFO**. FIFO wake can leave a
high-priority task queued behind a low-priority one, which is its own flavour of
priority inversion.

---

## 5. Mutex and ownership

### Why it is a separate type

The question "why not just use a binary semaphore initialised to 1 as a lock?"
has a precise answer. The mutex stores one extra field:

```c
typedef struct {
    sync_object_t obj;          /* must be first member */
    TCB *owner;                 /* NULL == unlocked */
} mutex_t;
```

That one pointer buys three things a semaphore structurally cannot have:

| Property | Enabled by `owner` |
|---|---|
| **Only the owner may unlock** | `if (m->owner != current_task) return -1;` — releasing a lock you don't hold is always a bug. A semaphore cannot check this, because giving one you never took is a *valid* use. |
| **Recursive self-lock is detectable** | `if (m->owner == current_task)` — otherwise a task deadlocks against itself with no diagnostic. |
| **Priority inheritance is possible at all** | There is a *specific task* to boost. |

The third is the structural one. **A semaphore can never support priority
inheritance, no matter how it is implemented, because there is nobody to
boost** — a semaphore is not held by anyone, so when a high-priority task blocks
on it there is no identifiable task whose priority you could raise to get it
released faster. That sentence is the whole answer to "why is a mutex not just a
binary semaphore?"

You can see this directly in the inheritance walk (`sync.c`):

```c
if (obj->type != SYNC_TYPE_MUTEX) return;   /* semaphores have no owner */
```

### Recovering the type from `void *`

`blocked_on` is a `void *` that may point at either object. The inheritance walk
needs to ask "is this a mutex, and if so who owns it?" — so both structs begin
with a common tagged header:

```c
typedef enum { SYNC_TYPE_SEM, SYNC_TYPE_MUTEX } sync_type_t;

typedef struct { sync_type_t type; } sync_object_t;

typedef struct { sync_object_t obj; /* first! */ ... } semaphore_t;
typedef struct { sync_object_t obj; /* first! */ TCB *owner; } mutex_t;
```

Because `obj` is the first member of both, a `void *` to either can be safely
read as a `sync_object_t *` to get the tag, then cast to the concrete type. This
is the C "common initial sequence" idiom — the same trick `struct sockaddr` uses.

### `mutex_lock`

```c
int mutex_lock(mutex_t *m)
{
    critical_section_enter();

    if (m->owner == current_task) {         /* recursive lock: detect, don't hang */
        sync_error_recursive_lock++;
        critical_section_exit();
        return -1;
    }

    while (m->owner != NULL) {

        current_task->blocked_on = &m->obj;
        current_task->state      = TASK_BLOCKED;

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
```

**The boost happens here, on the blocking path.** Not at unlock. The moment the
high-priority task *discovers* the mutex is taken is the moment the owner needs
to be accelerated.

**`blocked_on` is set before `inherit_propagate()` is called.** This ordering is
load-bearing for the transitive case: the walk traverses the chain by following
`blocked_on` pointers, so this task must already be linked into the chain before
the walk starts, or the first hop finds nothing.

Recursive locking returns an error rather than being supported. Supporting it
would mean a recursion count and an unlock that only releases at zero — extra
state for a case the demo doesn't need. Detecting it is a two-line safety net.

### `mutex_unlock`

```c
int mutex_unlock(mutex_t *m)
{
    critical_section_enter();

    if (m->owner != current_task) {
        sync_error_bad_unlock++;
        critical_section_exit();
        return -1;
    }

    TCB *me = current_task;

    m->owner = NULL;                                   /* 1. release */
    trace_pin_clear(TRACE_PIN_MUTEX);

    wake_waiter(highest_priority_waiter(&m->obj));     /* 2. wake */

    if (inheritance_enabled) {
        inherit_recompute(me);                         /* 3. drop back down */
    }

    critical_section_exit();
    return 0;
}
```

**Unlock does two jobs, and the order of the three steps matters.**

Clearing `m->owner` *first* means `inherit_recompute()` will not count tasks
still waiting on *this* mutex — we are handing it over, so their claim on our
priority ends now. If you called `inherit_recompute()` before clearing the
owner, it would find the waiters still blocked on a mutex owned by us, and
would keep us boosted forever.

Note the common misconception this code refutes: **the restore does not cause
the high-priority task to run.** The *wake* does. The restore is bookkeeping
that happens in the same function.

---

## 6. Priority inversion

### Definition

> Priority inversion is when a task is delayed by a **lower-priority task it
> shares no resource with**.

The precision matters. Consider the classic three tasks (higher number = higher
priority in this kernel; idle is 0):

| Task | Priority | Behaviour |
|---|---|---|
| Low | 1 | Takes the mutex, then computes for a long time holding it |
| Med | 2 | Pure CPU hog. Never blocks, never yields, shares nothing |
| High | 3 | Wants the same mutex |

High waiting on Low is **not** the bug. Low holds the mutex; High has to wait;
that is the correct and expected cost of sharing. It is **bounded** by the
length of Low's critical section.

The bug is that **Med — priority 2, which touches nothing High touches —
effectively preempts High, priority 3.** And because Med's runtime is arbitrary,
High's wait is **unbounded**: no amount of design-time analysis can compute a
worst case.

### The scheduler is not at fault

At every single tick, `scheduler()` correctly returns the highest-priority READY
task. The **scheduling invariant is upheld perfectly.**

What is violated is a different invariant:

> **Blocking-time invariant:** a high-priority task's blocking time should be
> bounded by the length of the critical section it is waiting on.

This distinction is the heart of the whole topic. The fix is not a better
scheduler; it is giving the scheduler better information about priority.

Med is also not at fault. Med is just running. It never touched the mutex and
does not know it exists.

### Why the medium task must busy-wait

The demo's Med task spins on a counter rather than calling `rtos_delay()`:

```c
static void busy_work_ms(uint32_t ms)
{
    for (uint32_t m = 0; m < ms; m++)
        for (volatile uint32_t i = 0; i < BUSY_LOOPS_PER_MS; i++) { }
}
```

`rtos_delay()` sets `TASK_BLOCKED` and switches away. A BLOCKED task is not on
the ready list, so Low would immediately get the CPU, finish its critical
section, and release the mutex. **No inversion.** Med must remain READY and
consume CPU.

The `volatile` on the loop counter forces a real load/store each iteration so
the loop survives optimisation. At `-O0` this is belt and braces; keep it so the
demo doesn't evaporate if someone raises the optimisation level.

### The role of round-robin

`rr_cursor[]` in `scheduler()` is indexed by priority — **round-robin only
operates within a single priority level.** Med (2) and Low (1) are at different
levels, so the cursor never gives Low a slice. Med gets **100%** of the CPU, not
a share.

A useful variant to try: set Med and Low to the *same* priority. Round-robin
then splits the CPU, Low gets every other slice, and it eventually releases the
mutex. The inversion degrades to a mere delay.

> **The inversion is total precisely because Med sits strictly above Low.**
> Equal priority makes it a delay; strictly-greater makes it unbounded.

---

## 7. Priority inheritance

### The idea

When a high-priority task blocks on a mutex, temporarily raise the **owner's**
priority to match the waiter's. The owner now outranks the medium tasks, runs to
the end of its critical section, releases, and drops back down.

It does not eliminate High's wait. It **bounds** it:

| | High's blocking time |
|---|---|
| **Without inheritance** | Low's remaining critical section **+ the total runtime of every task with priority between Low and High**. Unbounded. |
| **With inheritance** | Low's remaining critical section + context-switch overhead. **Bounded, and computable at design time from the source.** |

That second row is the entire engineering value: it makes the system
**analysable**. You can put a number on worst-case latency and defend it.

### Representing priority

The TCB splits the single `priority` field into two:

```c
uint32_t base_priority;       /* what task_create() was given. Never changes. */
uint32_t effective_priority;  /* what scheduler() compares. Raised by inheritance. */
```

**Invariant: `effective_priority >= base_priority`, always.**

The old `priority` field was *deleted* rather than kept as an alias, so every
site that used it became a compile error and had to be consciously assigned to
one or the other.

And one line in the scheduler changes:

```c
if ((int32_t)task_list[i]->effective_priority > highest_priority) { ... }
```

**That single line is what makes priority inheritance work.** Everything else in
`sync.c` is bookkeeping to keep `effective_priority` correct.

The rejected alternative — keep one `priority` field and stash the old value
inside the mutex — breaks the moment one task holds two mutexes (see
`inherit_recompute` below).

### The boost: `inherit_propagate`

```c
static void inherit_propagate(TCB *blocked)
{
    TCB *waiter = blocked;

    for (uint32_t depth = 0; depth < INHERIT_MAX_DEPTH; depth++) {

        if (waiter->blocked_on == NULL) return;

        sync_object_t *obj = (sync_object_t *)waiter->blocked_on;
        if (obj->type != SYNC_TYPE_MUTEX) return;      /* no owner to boost */

        TCB *owner = ((mutex_t *)obj)->owner;
        if (owner == NULL) return;

        if (owner->effective_priority >= waiter->effective_priority) return;

        owner->effective_priority = waiter->effective_priority;

        waiter = owner;                                /* follow the chain */
    }

    sync_inherit_depth_exceeded++;
}
```

**Why it is a loop (transitive inheritance).** Consider:

```
High (3) blocks on M1, owned by Mid (2)
Mid  (2) is itself blocked on M2, owned by Low (1)
```

Boosting only Mid achieves nothing — **Mid is BLOCKED and cannot run**, so it
cannot release M1 no matter how high its priority. The boost has to reach Low,
the task that is actually runnable. So the walk follows the chain:
waiter → its mutex → that mutex's owner → is *that* owner blocked? → repeat.

**Why the early return is safe.** If `owner->effective_priority >= waiter's`,
we stop — and skipping the rest of the chain is provably correct. Every link
further up was itself raised to at least its own waiter's priority when *it*
blocked. So if this owner is already high enough, every task above it in the
chain is too. Nothing downstream would change.

**Why the depth cap.** A cycle of mutex owners is a deadlock. The demo should
never create one, but an unbounded walk over a cycle would hang inside a
critical section with interrupts masked — the worst possible place. The cap
turns a hang into an incremented counter you can find in the debugger.

### The restore: `inherit_recompute`

This is the subtlest function in week 4.

**The naive version is wrong:**

```c
t->effective_priority = t->base_priority;   /* WRONG when >1 mutex is held */
```

Counter-example:

```
Low (base 1) holds BOTH M1 and M2
  H_A (prio 3) blocks on M1  ->  Low boosted to 3
  H_B (prio 4) blocks on M2  ->  Low boosted to 4
  Med (prio 2) spins forever

Low unlocks M2.
  Naive restore: Low.effective = 1
  But H_A is STILL blocked on M1, which Low STILL holds.
  Med (2) > Low (1), so Med preempts Low.
  H_A is starved again — the inversion is back.
```

**The correct rule:**

> `effective = max(base_priority, highest effective_priority among all tasks
> still blocked on a mutex this task still owns)`

```c
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

        if (w->effective_priority > p) p = w->effective_priority;
    }

    t->effective_priority = p;
}
```

Recomputing from scratch rather than restoring a saved value is what makes
nesting and out-of-order unlocking safe.

**This is where the "no wait queue" decision pays off.** Answering "who is still
waiting on something I hold?" requires walking *outward* from a task to its held
mutexes. With the scanning approach, that is the same scan with the predicate
inverted — scan for any task whose `blocked_on` points at a mutex whose
`owner == me`. No per-task list of held mutexes is needed. The two design
choices compose; that is worth pointing out when presenting the project.

### Why the recompute excludes the just-released mutex

In `mutex_unlock`, `m->owner = NULL` happens *before* `inherit_recompute(me)`.
This correctly handles both:

- The waiter we just woke: its `blocked_on` is now `NULL`, so it is skipped.
- Any *other* waiter still blocked on this same mutex: their `blocked_on` still
  points at it, but `((mutex_t*)obj)->owner` is now `NULL`, not `me`, so the
  `owner != t` test excludes them too. Correct — we no longer hold it.

---

## 8. The demo scenario

`src/demo.c`. Kept entirely out of the kernel — `task.c` no longer knows any
application task exists.

### Parameters

```c
#define PRIO_LOW    1u        #define ROUND_TICKS        1000u
#define PRIO_MED    2u        #define OFFSET_LOW_START      0u
#define PRIO_HIGH   3u        #define OFFSET_MED_START     10u
                              #define OFFSET_HIGH_REQUEST  20u
#define LOW_CRITICAL_MS   200u
#define MED_HOG_MS        300u
#define HIGH_WORK_MS        5u
```

### Determinism

Most people's inversion demo is flaky because the ordering isn't guaranteed. The
scenario only works if:

1. Low is **already holding** the mutex before High asks for it, and
2. Med is **already hogging** before High blocks.

This is enforced by staggering the tasks against absolute tick deadlines within
a fixed-length round:

```c
static uint32_t next_round_start(void)
{
    return ((tick_count / ROUND_TICKS) + 1u) * ROUND_TICKS;
}
```

Each task computes this independently at the top of its loop and then
`rtos_delay_until(start + ITS_OFFSET)`. They agree on the round because all work
completes well inside the 1000-tick window.

`rtos_delay_until()` (absolute) is used rather than `rtos_delay()` (relative)
because a relative delay drifts by however long the task ran before calling it —
and the whole point here is that the tasks run for wildly different durations in
the two modes. Absolute deadlines mean every round starts from an identical,
quiescent state, which is what makes the two modes comparable.

### Alternating the mode

The inheritance flag flips at the round boundary, inside the Low task:

```c
round_number++;
sync_set_inheritance((round_number & 1u) ? 0 : 1);
```

Low owns offset 0, which is the only instant in a round where nothing is blocked
and no mutex is held — the safe point to change the rule. Odd rounds run broken,
even rounds run fixed.

The payoff: **a single logic-analyser capture contains the bug and the fix, back
to back, under identical conditions.** PA9 tells you which round is which. That
is a far stronger demonstration than two separate builds.

### Measurement

High brackets the blocking call and records:

```c
uint32_t req = tick_count;
mutex_lock(&shared_mutex);       /* <-- blocks here */
uint32_t acq = tick_count;
record(inherit_on, req, acq);
```

Results land in a ring buffer plus two rolling averages:

```c
volatile uint32_t demo_avg_blocked_inherit_off;
volatile uint32_t demo_avg_blocked_inherit_on;
```

Reading those two variables in GDB is the milestone in one glance.

### Calibration

`BUSY_LOOPS_PER_MS` (2000) is an estimate for 16 MHz at `-O0` and **must be
tuned on the board**: scope PA5 and time one Low critical section in an
inheritance-ON round — the pin should stay high for ~200 ms. Scale the constant
by the measured ratio.

The demo still works if this is off by 2×, because the two modes differ by more
than that. But the numbers are only quotable once it's tuned.

---

## 9. Instrumentation and tracing

### Channel map

| Pin | Signal |
|---|---|
| PA5 | Low task running |
| PA6 | Med task running |
| PA7 | **High task running** — the milestone signal |
| PA8 | Mutex currently held |
| PA9 | Priority inheritance enabled this round |

Idle drives all three task pins low, so gaps in the waveform are real idle time.

### Driven from the switch, not the task

The pins are set in `commit_switch_to()`, not in the task bodies:

```c
void commit_switch_to(TCB *nt) {
    next_task = nt;
    nt->state = TASK_RUNNING;
    nt->context_switch_count++;
    trace_pin_select(nt->trace_pin);
    SCB_ICSR |= PENDSVSET;
}
```

This matters: the waveform is then **the scheduler's decision**, not the
application's opinion of it. A task cannot lie about when it ran.

It runs a few microseconds before the actual PendSV switch (PendSV is the
lowest-priority exception and tail-chains almost immediately). Against a 1 ms
tick that skew is invisible, and doing it in C keeps the assembly switcher
untouched.

### Why BSRR, not ODR

Weeks 1–3 toggled pins with `*GPIOA_ODR ^= (1U << PA5)` — a read-modify-write.
That is not safe from a context that can be interrupted by another writer.

BSRR is write-only: writing bit *n* sets pin *n*, writing bit *n+16* clears it.
One store, no read, no race:

```c
void trace_pin_select(uint32_t pin_mask)
{
    /* Upper half clears all three task pins, lower half raises the one we want.
       Both land in the same store, so the analyser never sees two task pins
       high at once, or none. */
    *GPIOA_BSRR = (TRACE_TASK_PIN_MASK << 16) | (pin_mask & TRACE_TASK_PIN_MASK);
}
```

The atomicity has a visible payoff: the trace never shows a glitch frame with
two tasks apparently running.

### Expected waveform

```
                    ROUND N  (PA9 low, inheritance OFF)
  Low   PA5  ██                            ████████████████████
  Med   PA6    ██████████████████████████
  High  PA7                                                    ███
  Mtx   PA8  ████████████████████████████████████████████████
             +0  +10  +20            +310              +500  +501
                      ↑ High blocks                          ↑ acquires
                      └──────────── 481 ms ───────────────────┘

                    ROUND N+1  (PA9 high, inheritance ON)
  Low   PA5  ██        ████████████████████
  Med   PA6    ████████                    ███████████████████
  High  PA7                                 ███
  Mtx   PA8  ██████████████████████
             +0  +10  +20            +210 +211
                      ↑ boost to 3         ↑ acquires
                      └──── 191 ms ────────┘
```

**481 ms vs 191 ms.**

The clearest visual tell is the PA5/PA6 swap at `+20` in round N+1: Low, a
priority-1 task, preempting Med, a priority-2 task. That inversion *of the pins*
is the inheritance working.

---

## 10. Bugs fixed from weeks 1–3

Four pre-existing defects blocked week 4.

**1. `main()` initialised everything twice.**

```c
task_system_init();  systick_init();
task_system_init();  systick_init();   /* duplicated */
```

`task_create()` appends to `task_list[]` unconditionally, so this registered six
TCBs — A, B, C, then A, B, C again — with the second pass overwriting each TCB's
`sp` while stale pointers to the first pass remained in the list. The scheduler
was round-robining over aliased duplicates.

**2. `__disable_irq()` / `__enable_irq()` were undefined.** They are CMSIS
intrinsics, but no CMSIS header was included and the build uses `-nostdlib
-ffreestanding` with no include path. The critical section code could not link.
Now defined as inline asm in `port.h`.

**3. `yield()` and `rtos_delay()` ran with interrupts enabled.**

```c
current_task->state = TASK_READY;
TCB *nt = scheduler();          /* <-- SysTick can land here */
commit_switch_to(nt);
```

If SysTick fired between `scheduler()` and `commit_switch_to()`, the handler
would run its own `scheduler()`, pend PendSV, and — since PendSV tail-chains
after SysTick returns — switch us out *mid-`yield()`*. When the task was
eventually resumed it would finish the call and commit a **stale `next_task`**,
switching to a task the scheduler no longer chose. Both functions are now
wrapped in critical sections.

**4. `build.ninja` ran C files through the `assemble` rule.** Harmless in
practice (both rules invoke `$cc -c`) but mislabelled; corrected, and `demo.c`
added.

Additionally, `svc.S` hardcoded `task_a_tcb` as the first task. It now
bootstraps from `current_task`, which `main()` sets from `scheduler()` — so the
kernel picks the first task rather than the assembly hardcoding it:

```asm
ldr    r0, =current_task
ldr    r0, [r0]           @ r0 = current_task  (a TCB*)
ldr    r0, [r0]           @ r0 = current_task->sp   (sp is TCB offset 0)
```

Note the **two** dereferences: `current_task` is a pointer *variable*, so you
load its address, then its value, then the TCB's first member.

---

## 11. Design decision register

| # | Decision | Chosen | Rejected | Why |
|---|---|---|---|---|
| 1 | Distinguishing block reasons | `void *blocked_on` in TCB | New `TASK_BLOCKED_SEM` state; sentinel `waketick = UINT32_MAX` | Scales to "wait with timeout" (both fields at once); also records *which* object, which inheritance needs. A state enum can't express the combination without exploding. |
| 2 | Wait queue | None — scan `task_list[]` | Intrusive priority-ordered list in TCB; FIFO array per object | n ≤ 8. No memory overhead, no list surgery, and immune to the "boosted waiter breaks list ordering" bug. Composes with decision 4's outward walk. |
| 3 | Wake selection | Highest effective priority | FIFO | FIFO wake can queue a high-priority task behind a low one — inversion by another name. |
| 4 | Priority representation | `base_priority` + `effective_priority` | Single field, save old value in the mutex | Saving in the mutex breaks with two held mutexes. Recomputing from scratch handles nesting and out-of-order unlock. |
| 5 | Inheritance scope | Transitive (walk the owner chain) | Single-level | Boosting a *blocked* owner achieves nothing — it still can't run. The boost must reach a runnable task. |
| 6 | Wake timing | Deferred to next tick | Immediate yield inside give/unlock | Simpler. Costs ≤1 ms, which is noise against a 200 ms critical section. Revisit if the CS drops below ~10 ms. |
| 7 | Recursive mutex lock | Detect and return −1 | Support with a count; deadlock | A recursion count plus a count-aware unlock is state the demo doesn't need. Detecting is two lines and turns a hang into a counter. |
| 8 | Trace mechanism | GPIO from `commit_switch_to()` + RAM results | Toggle inside task bodies; UART printf | Pins driven by the switch record the scheduler's decision, not the app's claim. UART would perturb the timing being measured and cost a driver. |
| 9 | GPIO write | BSRR (single store) | ODR read-modify-write | RMW from a preemptible context can lose a write; BSRR is atomic by construction. |
| 10 | Critical nesting counter | One global | Per-task in TCB | Correct because switches only occur at nesting == 0. Per-task becomes necessary only if that invariant is ever broken. |
| 11 | Demo task location | Separate `demo.c` | Inside `task.c` | Kernel has no knowledge of application tasks; `task_system_init()` brings up only idle. |
| 12 | Demo mode switching | Alternate every round at runtime | Two separate builds; a button | One capture contains both behaviours under identical conditions. |

---

## 12. Tick-by-tick flow traces

Times are offsets from the round start `T`. Assumes `BUSY_LOOPS_PER_MS` is
calibrated.

### Round with inheritance OFF

| Tick | Event | Ready set (effective prio) | Running |
|---|---|---|---|
| T+0 | Low wakes, flips mode off, `mutex_lock` succeeds (free), begins 200 ms CS | Low(1) | **Low** |
| T+10 | Med wakes. 2 > 1, preempts. Low has spent 10 ms of its 200 | Low(1), Med(2) | **Med** |
| T+20 | High wakes. 3 > 2, preempts. Calls `mutex_lock` → owner is Low → `blocked_on = &M`, BLOCKED. **No boost.** `scheduler()` → Med | Low(1), Med(2) | **Med** |
| T+310 | Med completes 300 ms of CPU, calls `rtos_delay_until(next round)` → BLOCKED | Low(1) | **Low** |
| T+500 | Low completes its 200 ms, calls `mutex_unlock`: owner = NULL, wakes High (READY). Low keeps running (still prio 1), gives the semaphore, delays → BLOCKED | High(3) | Low → idle |
| T+501 | SysTick runs `scheduler()` → High. High's `while` re-tests: owner is NULL → acquires | High(3) | **High** |

**High blocked from T+20 to T+501 = 481 ms.**

Note what determined that number: `MED_HOG_MS`. Not the critical section. Make
Med hog for 5 seconds and High waits 5 seconds. **Unbounded.**

### Round with inheritance ON

| Tick | Event | Ready set (effective prio) | Running |
|---|---|---|---|
| T+0 | Low wakes, flips mode on (PA9 high), locks, begins 200 ms CS | Low(1) | **Low** |
| T+10 | Med wakes, preempts. Low has spent 10 ms | Low(1), Med(2) | **Med** |
| T+20 | High wakes, preempts, calls `mutex_lock` → owner is Low → BLOCKED. **`inherit_propagate`: Low.effective 1 → 3.** Chain stops (Low isn't blocked). `scheduler()` sees Low(3) > Med(2) | **Low(3)**, Med(2) | **Low** |
| T+20…T+210 | Low runs its remaining 190 ms **at priority 3**. Med is starved — by a task whose base priority is below it | Low(3), Med(2) | **Low** |
| T+210 | Low completes the CS. `mutex_unlock`: owner = NULL, wake High, `inherit_recompute(Low)` → nobody waits on anything Low holds → back to base 1. Low continues briefly at prio 1, then delays | Med(2), High(3) | Low → … |
| T+211 | SysTick → `scheduler()` → High(3). Acquires the mutex | Med(2), High(3) | **High** |
| T+216 | High finishes its 5 ms, unlocks, delays to next round | Med(2) | **Med** |
| T+506 | Med finally completes its 300 ms of CPU and delays | — | idle |

**High blocked from T+20 to T+211 = 191 ms.**

Now what determined *that* number: `LOW_CRITICAL_MS`. Med's runtime does not
appear in the answer at all. **Bounded.**

### The two rows that matter

| | Blocking time | Determined by | Analysable? |
|---|---|---|---|
| Inheritance off | 481 ms | `MED_HOG_MS` (arbitrary) | **No** |
| Inheritance on | 191 ms | `LOW_CRITICAL_MS` (yours) | **Yes** |

---

## 13. Known limitations

Being explicit about these is worth more in an interview than pretending they
don't exist.

1. **Not built or run.** No toolchain was available. Expect compile errors on
   first build; the timing figures are derived, not measured.
2. **No timeouts.** `sem_take()` and `mutex_lock()` wait forever. The `blocked_on`
   design was chosen specifically so this can be added — set both `blocked_on`
   and `waketick`, and let `task_check_wakeups()` handle the deadline path — but
   it isn't implemented.
3. **Deferred wake costs up to 1 tick.** For up to 1 ms after an unlock, a
   lower-priority task runs while a higher-priority one is READY. Bounded, but
   real.
4. **No deadlock detection.** `INHERIT_MAX_DEPTH` stops the inheritance *walk*
   from hanging on a cycle, but nothing detects or breaks the deadlock itself.
5. **Recursive locking unsupported**, only detected.
6. **No priority ceiling protocol.** Inheritance is reactive (boost when
   contention occurs). The ceiling protocol is proactive (boost on acquisition,
   to the highest priority of any task that *could* want the mutex) and also
   prevents deadlock. Inheritance was chosen because the inversion is directly
   observable; the ceiling protocol prevents the scenario from ever being
   visible, which makes a worse demo.
7. **O(n) scans** in `highest_priority_waiter` and `inherit_recompute`. Fine at
   n ≤ 8; would need real queues at n in the hundreds.
8. **`BUSY_LOOPS_PER_MS` needs board calibration.**
9. **Blocking inside a caller-held critical section hangs.** Documented as a
   caller contract in `sync.c`, not enforced. An assert would be better.

---

## 14. Build and run

```bash
ninja                       # -> build/minirtos.elf
```

Requires `arm-none-eabi-gcc` and `ninja` on PATH.

### Reading the milestone

Flash, let it run through several rounds, then halt in GDB:

```
(gdb) print demo_avg_blocked_inherit_off
(gdb) print demo_avg_blocked_inherit_on
(gdb) print demo_results
(gdb) print demo_result_count
```

Sanity counters — all three should be zero:

```
(gdb) print sync_error_recursive_lock
(gdb) print sync_error_bad_unlock
(gdb) print sync_inherit_depth_exceeded
```

### Capturing the waveform

Probe PA5–PA9. Trigger on PA9 to separate the modes. Capture ≥2 seconds to get
one round of each. Measure the gap from the PA6→PA5 transition at `+20` (or the
PA8 falling edge) to the PA7 rising edge.

### Suggested experiments

| Change | Expected result |
|---|---|
| Set Med to priority 1 (same as Low) | Round-robin splits the CPU; the inversion degrades from unbounded to a bounded delay |
| Raise `MED_HOG_MS` to 600 | Inheritance-off time grows to ~800 ms; inheritance-on time **does not change**. This is the clearest single demonstration that one is bounded and the other isn't |
| Comment out `inherit_recompute()` in `mutex_unlock` | Low stays boosted forever; Med is permanently starved. Shows the restore is load-bearing |
| Change `while (s->count == 0)` to `if` in `sem_take` | Introduces the double-entry race deliberately |
| Remove `blocked_on == NULL` from `task_check_wakeups` | Reintroduces the original bug: semaphore waiters release spuriously on the next tick |

---

## 15. Interview questions

The ones most likely to be asked, with the answers this implementation gives.

**Q. What is priority inversion?**
A task delayed by a lower-priority task it shares no resource with. High waiting
on Low for the mutex is normal and bounded; High waiting on *Med*, which shares
nothing with it, is the pathology — and it's unbounded because Med's runtime is
arbitrary.

**Q. Is the scheduler broken?**
No. It correctly returns the highest-priority READY task at every tick. The
scheduling invariant holds perfectly. What's violated is the blocking-time
invariant: High's wait should be bounded by the critical section it's waiting
on. The fix isn't a better scheduler, it's better priority information.

**Q. Why can't a semaphore support priority inheritance?**
Structurally, not as an implementation gap: a semaphore isn't *held* by anyone.
When a task blocks on it there is no identifiable owner whose priority you could
raise. A mutex has an `owner` field, so there is a specific task to boost. That
one pointer is the entire difference.

**Q. Where does the boost happen, and why there?**
In `mutex_lock`, on the blocking path — the moment the high task *discovers* the
mutex is taken. Not at unlock. The owner needs to be accelerated for the
duration of its critical section, which starts now.

**Q. Why is the inheritance walk a loop rather than a single boost?**
Transitive case. If the owner is itself blocked on another mutex, boosting it
does nothing — a BLOCKED task can't run regardless of priority, so it can't
release anything. The boost must propagate down the ownership chain until it
reaches a task that is actually runnable.

**Q. On unlock, what do you restore the priority to?**
`max(base_priority, highest priority of any task still blocked on a mutex I
still hold)`. Restoring straight to base is wrong with two held mutexes: a task
still waiting on the mutex you kept would be starved again the moment a medium
task preempts you. I recompute from scratch rather than restoring a saved value,
which is what makes nested and out-of-order unlocking safe.

**Q. Why does `mutex_unlock` clear the owner before recomputing?**
So the recompute doesn't count waiters on the mutex being released — their claim
on my priority ends when I hand it over. Recomputing first would find them still
blocked on a mutex I own and keep me boosted permanently.

**Q. Why `while` and not `if` in `sem_take`?**
Two reasons. The wake is deferred, so between being marked READY and actually
being scheduled another task can consume the count. And re-checking makes any
spurious wake a wasted context switch rather than a silent double-entry into a
protected region. Same reason `pthread_cond_wait` requires a loop.

**Q. Why must the blocking path switch immediately, when the wake path can be
deferred?**
Asymmetry of consequences. A task that blocks must not execute another
instruction — otherwise it walks straight into the critical section it was just
excluded from. A task being woken isn't running, so waking it late is merely
late, not wrong.

**Q. Your critical-section nesting counter is one global shared by all tasks.
Isn't that a bug?**
It's correct because context switches can only happen when interrupts are on,
and interrupts are only on when the counter is zero. So every switch point sees
it at zero and no task can observe another's depth. The blocking primitives
uphold that deliberately: they exit the critical section before handing over the
CPU. If I ever added a path that switched with nesting > 0, the counter would
have to move into the TCB.

**Q. Why did you use `blocked_on` instead of a new task state?**
It scales. A state enum can't express "blocked on this object, but give up after
N ticks" without a combinatorial explosion. Two orthogonal fields can. It also
records *which* object, which the inheritance walk needs in order to find the
owner.

**Q. Why no wait queues?**
n ≤ 8, so a scan is cheaper than maintaining lists, and it eliminates a bug
class: a priority-ordered intrusive list stops being ordered the moment a
waiter's priority is boosted by inheritance. Scanning reads the current priority
every time. It also makes the reverse query — "who still waits on something I
hold?" — the same scan with the predicate inverted, which is exactly what the
priority restore needs.

**Q. Priority inheritance vs priority ceiling?**
Inheritance is reactive: boost when contention actually occurs. Ceiling is
proactive: on acquiring a mutex, immediately raise to the highest priority of
any task that could ever want it. Ceiling also prevents deadlock and bounds
blocking to a single critical section, but it needs static analysis of who uses
what, and it penalises the uncontended case. I chose inheritance because the
inversion is directly observable — the ceiling protocol prevents the scenario
from ever becoming visible, which makes for a worse demonstration.

**Q. What does inheritance actually buy you?**
Not a shorter wait in every case — it doesn't eliminate High's blocking. It
makes the blocking time **bounded and computable at design time**. Without it,
worst-case latency is a function of every medium-priority task in the system.
With it, it's a function of your own critical section. The system becomes
analysable.
