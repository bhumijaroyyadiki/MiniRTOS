#include "task.h"
#include "port.h"
#include <stdint.h>
#include <stddef.h>
#include "gpio.h"

TCB *current_task = NULL;
TCB *next_task = NULL;

static _Alignas(8) uint32_t idle_stack[IDLE_STACK_WORDS];
static TCB *task_list[MAX_TASKS];
uint32_t task_count = 0;

/* ---- Instrumentation: kept, minimal ----
   switch_t0/t1/t2: PendSV trigger -> handler-entry -> handler-exit cycle
   timestamps (t1/t2 written from context_switch.s). sched_time_last/max:
   cost of a single scheduler() call, worst case observed since boot.
   spurious_wake_count: canary for the blocked_on/waketick bug class -- stays
   at 0 permanently now that task_check_wakeups() is fixed; a nonzero value
   here in the future means that bug class came back. */
volatile uint32_t switch_t0 = 0;
volatile uint32_t switch_t1 = 0;
volatile uint32_t switch_t2 = 0;

volatile uint32_t sched_time_last = 0;
volatile uint32_t sched_time_max  = 0;

volatile uint32_t systick_latency_last = 0;
volatile uint32_t systick_latency_max  = 0;

volatile uint32_t spurious_wake_count = 0;
/* Array of cursors for Round-Robin scheduling per priority level.
   Tracks the most recently selected TCB for priority 'p'.
   Indexed by EFFECTIVE priority, so a task that has been boosted round-robins
   against its new peers, not its old ones. */
static TCB *rr_cursor[MAX_PRIORITIES] = { NULL };

TCB idle_task_tcb;
TCB *idle_task = &idle_task_tcb;

static void idle_task_function(void);

uint32_t *task_stack_init(uint32_t *stack_high, void (*task_function)(void))
{
    uint32_t *sp = stack_high;

    sp = (uint32_t *)((uintptr_t)sp & ~((uintptr_t)0x7));

    /* Hardware exception frame */
    *(--sp) = 0x01000000;                     /* xPSR */
    *(--sp) = ((uint32_t)task_function) | 1U;  /* PC */
    *(--sp) = 0xFFFFFFFD;                     /* LR */
    *(--sp) = 0x00000000;                     /* R12 */
    *(--sp) = 0x00000000;                     /* R3 */
    *(--sp) = 0x00000000;                     /* R2 */
    *(--sp) = 0x00000000;                     /* R1 */
    *(--sp) = 0x00000000;                     /* R0 */

    /* R4-R11 */
    *(--sp) = 0xBBBBBBBB;
    *(--sp) = 0xAAAAAAAA;
    *(--sp) = 0x99999999;
    *(--sp) = 0x88888888;
    *(--sp) = 0x77777777;
    *(--sp) = 0x66666666;
    *(--sp) = 0x55555555;
    *(--sp) = 0x44444444;

    return sp;
}

int task_create(
    TCB *tcb,
    uint32_t *stack,
    uint32_t stack_words,
    void (*task_function)(void),
    uint8_t priority,
    const char *name
)
{
    if (tcb == NULL || stack == NULL || task_function == NULL) {
        return -1;
    }
    if (task_count >= MAX_TASKS) {
        return -1;
    }
    if (priority >= MAX_PRIORITIES) {
        return -1;
    }

    tcb->stack_low  = &stack[0];
    tcb->stack_high = &stack[stack_words];
    for (uint32_t i = 0; i < stack_words; i++) {
    stack[i] = 0xDEADBEEF;
    }
    tcb->sp = task_stack_init(tcb->stack_high, task_function);

    tcb->base_priority      = priority;
    tcb->effective_priority = priority;
    tcb->state              = TASK_READY;
    tcb->name                = name;
    tcb->waketick            = 0;
    tcb->blocked_on          = NULL;
    tcb->context_switch_count = 0;
    tcb->trace_pin           = 0;
    tcb->cpu_cycles_total = 0;
    tcb->last_resume_cycle = 0;
    task_list[task_count++] = tcb;
    return 0;
}

void task_system_init(void)
{
    idle_task_tcb.stack_low  = &idle_stack[0];
    idle_task_tcb.stack_high = &idle_stack[IDLE_STACK_WORDS];
    for (uint32_t i = 0; i < IDLE_STACK_WORDS; i++) {
    idle_stack[i] = 0xDEADBEEF;
    }
    idle_task_tcb.sp = task_stack_init(idle_task_tcb.stack_high, idle_task_function);

    idle_task_tcb.base_priority      = 0;
    idle_task_tcb.effective_priority = 0;
    idle_task_tcb.state              = TASK_READY;
    idle_task_tcb.name                = "Idle";
    idle_task_tcb.waketick            = 0;
    idle_task_tcb.blocked_on          = NULL;
    idle_task_tcb.context_switch_count = 0;
    idle_task_tcb.trace_pin           = 0;
}

TCB *task_get_current(void) { return current_task; }

void task_set_state(TCB *tcb, task_state_t state)
{
    if (tcb != NULL) tcb->state = state;
}

void task_set_trace_pin(TCB *tcb, uint32_t pin_mask)
{
    if (tcb != NULL) tcb->trace_pin = pin_mask;
}

uint32_t task_get_count(void) { return task_count; }

TCB *task_get_by_index(uint32_t index)
{
    if (index >= task_count) return NULL;
    return task_list[index];
}

TCB* scheduler(void)
{
    int32_t  highest_priority = -1;
    uint32_t i;

    for (i = 0; i < task_count; i++) {
        if (task_list[i] != NULL && task_list[i]->state == TASK_READY) {
            if ((int32_t)task_list[i]->effective_priority > highest_priority) {
                highest_priority = (int32_t)task_list[i]->effective_priority;
            }
        }
    }

    if (highest_priority == -1) {
        return idle_task;
    }

    TCB *cursor = rr_cursor[highest_priority];
    uint32_t start_index = 0;

    if (cursor != NULL) {
        for (i = 0; i < task_count; i++) {
            if (task_list[i] == cursor) {
                start_index = i + 1;
                break;
            }
        }
    }

    TCB *selected = NULL;
    for (i = 0; i < task_count; i++) {
        uint32_t eval_index = (start_index + i) % task_count;
        TCB *candidate = task_list[eval_index];
        if (candidate != NULL &&
            candidate->state == TASK_READY &&
            candidate->effective_priority == (uint32_t)highest_priority) {
            selected = candidate;
            break;
        }
    }

    if (selected != NULL) {
        rr_cursor[highest_priority] = selected;
        return selected;
    }
    return idle_task;
}

/* Wraps a scheduler() call with the minimal timing capture used everywhere.
   Not merged into commit_switch_to(): the two callers below need the TCB*
   result, not just the elapsed time, and a function can't hand back both
   without an out-parameter or a struct -- inlining these three lines at each
   call site is the simpler choice than either of those. */
#define MEASURE_SCHEDULER_CALL(nt_var)                              \
    do {                                                             \
        uint32_t _s = DWT_CYCCNT;                                    \
        (nt_var) = scheduler();                                      \
        uint32_t _e = DWT_CYCCNT - _s;                                \
        sched_time_last = _e;                                        \
        if (_e > sched_time_max) sched_time_max = _e;                \
    } while (0)

void commit_switch_to(TCB *nt)
{
    next_task = nt;
    nt->state = TASK_RUNNING;
    nt->context_switch_count++;

    trace_pin_select(nt->trace_pin);

    switch_t0 = DWT_CYCCNT;
    SCB_ICSR |= PENDSVSET;
}

void yield(void)
{
    critical_section_enter();

    current_task->state = TASK_READY;

    TCB *nt;
    MEASURE_SCHEDULER_CALL(nt);

    if (nt != current_task) {
        commit_switch_to(nt);
    } else {
        current_task->state = TASK_RUNNING;
    }

    critical_section_exit();
}

void rtos_delay(uint32_t ms)
{
    critical_section_enter();

    current_task->waketick   = tick_count + ms;
    current_task->blocked_on = NULL;
    current_task->state      = TASK_BLOCKED;

    TCB *nt;
    MEASURE_SCHEDULER_CALL(nt);

    commit_switch_to(nt);

    critical_section_exit();
}

void rtos_delay_until(uint32_t target_tick)
{
    critical_section_enter();

    if ((int32_t)(target_tick - tick_count) <= 0) {
        critical_section_exit();
        return;
    }

    current_task->waketick   = target_tick;
    current_task->blocked_on = NULL;
    current_task->state      = TASK_BLOCKED;

    TCB *nt;
    MEASURE_SCHEDULER_CALL(nt);

    commit_switch_to(nt);

    critical_section_exit();
}

void task_check_wakeups(void)
{
    /* blocked_on == NULL is required: a task blocked on a mutex/semaphore is
       also TASK_BLOCKED, and its waketick is a stale value from some earlier
       delay call. Without this check it would be woken by tick expiry alone,
       regardless of whether the thing it's actually waiting for happened. */
    for (uint32_t i = 0; i < task_count; i++) {
        TCB *tcb = task_list[i];
        if (tcb != NULL &&
            tcb->state == TASK_BLOCKED &&
            (int32_t)(tcb->waketick - tick_count) <= 0) {

            if (tcb->blocked_on != NULL) {
                /* Would have been a spurious wake under the old logic.
                   Counted, not acted on -- canary only. */
                spurious_wake_count++;
                continue;
            }

            tcb->state = TASK_READY;
        }
    }
}

static void idle_task_function(void)
{
    while (1) {
        __asm volatile ("wfi");
    }
}

uint32_t task_stack_high_water_words(TCB *tcb)
{
    uint32_t *p = tcb->stack_low;

    while (p < tcb->sp && *p == 0xDEADBEEF) {
        p++;
    }

    return (uint32_t)(tcb->stack_high - p);
}
