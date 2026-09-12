#include "task.h"
#include "port.h"
#include <stdint.h>
#include <stddef.h>
#include "gpio.h"

TCB *current_task = NULL;
TCB *next_task = NULL;

static _Alignas(8) uint32_t idle_stack[IDLE_STACK_WORDS];
static TCB *task_list[MAX_TASKS];
static uint32_t task_count = 0;

/* Array of cursors for Round-Robin scheduling per priority level.
   Tracks the most recently selected TCB for priority 'p'.
   Indexed by EFFECTIVE priority, so a task that has been boosted round-robins
   against its new peers, not its old ones. */
static TCB *rr_cursor[MAX_PRIORITIES] = { NULL };

TCB idle_task_tcb;
TCB *idle_task = &idle_task_tcb;

static void idle_task_function(void);

uint32_t *task_stack_init( uint32_t *stack_high,void (*task_function)(void))
{
    uint32_t *sp = stack_high;

    /* Make stack 8-byte aligned (Cortex-M exception frames require appropriate
     stack alignment. The architecture expects the stack to maintain 8-byte
     alignment at exception boundaries. Our uint32_t array is naturally 4-byte
     aligned, but we want to explicitly ensure 8-byte alignment.) */
    sp = (uint32_t *)((uintptr_t)sp & ~((uintptr_t)0x7));

    /* Hardware exception frame */
    *(--sp) = 0x01000000;                  /* xPSR */
    *(--sp) = ((uint32_t)task_function) | 1U; /* PC (the exception frame's PC must be a valid Thumb address) */
    *(--sp) = 0xFFFFFFFD;                  /* LR */
    *(--sp) = 0x00000000;                  /* R12 */
    *(--sp) = 0x00000000;                  /* R3 */
    *(--sp) = 0x00000000;                  /* R2 */
    *(--sp) = 0x00000000;                  /* R1 */
    *(--sp) = 0x00000000;                  /* R0 */

    /* R4-R11 */
    *(--sp) = 0xBBBBBBBB;  /* R11 */
    *(--sp) = 0xAAAAAAAA;  /* R10 */
    *(--sp) = 0x99999999;  /* R9 */
    *(--sp) = 0x88888888;  /* R8 */
    *(--sp) = 0x77777777;  /* R7 */
    *(--sp) = 0x66666666;  /* R6 */
    *(--sp) = 0x55555555;  /* R5 */
    *(--sp) = 0x44444444;  /* R4 */

    /* As the stack grows downwards. In task startup mechanics software pops
       first and then hardware, so the stack pointer must point at the last
       pushed value, which is R4. */

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
    if (tcb == NULL ||
        stack == NULL ||
        task_function == NULL)
    {
        return -1;
    }

    if (task_count >= MAX_TASKS)
    {
        return -1;
    }

    /* rr_cursor[] is indexed by priority, so an out-of-range priority would
       corrupt memory past the array rather than merely misbehave. */
    if (priority >= MAX_PRIORITIES)
    {
        return -1;
    }

    tcb->stack_low = &stack[0];
    tcb->stack_high = &stack[stack_words];

    tcb->sp = task_stack_init(
        tcb->stack_high,
        task_function
    );

    tcb->base_priority = priority;
    tcb->effective_priority = priority;   /* no inheritance active yet */
    tcb->state = TASK_READY;
    tcb->name = name;
    tcb->waketick = 0;
    tcb->blocked_on = NULL;
    tcb->context_switch_count = 0;
    tcb->trace_pin = 0;

    task_list[task_count++] = tcb;

    return 0;
}

void task_system_init(void)
{
    /* Only the idle task. Application tasks register themselves. */
    idle_task_tcb.stack_low  = &idle_stack[0];
    idle_task_tcb.stack_high = &idle_stack[IDLE_STACK_WORDS];

    idle_task_tcb.sp = task_stack_init(
        idle_task_tcb.stack_high,
        idle_task_function
    );

    idle_task_tcb.base_priority = 0;
    idle_task_tcb.effective_priority = 0;
    idle_task_tcb.state = TASK_READY;
    idle_task_tcb.name = "Idle";
    idle_task_tcb.waketick = 0;
    idle_task_tcb.blocked_on = NULL;
    idle_task_tcb.context_switch_count = 0;
    idle_task_tcb.trace_pin = 0;   /* idle drives every task pin low */
}

TCB *task_get_current(void)
{
    return current_task;
}

void task_set_state(TCB *tcb, task_state_t state)
{
    if (tcb != NULL)
    {
        tcb->state = state;
    }
}

void task_set_trace_pin(TCB *tcb, uint32_t pin_mask)
{
    if (tcb != NULL)
    {
        tcb->trace_pin = pin_mask;
    }
}

uint32_t task_get_count(void)
{
    return task_count;
}

TCB *task_get_by_index(uint32_t index)
{
    if (index >= task_count)
    {
        return NULL;
    }
    return task_list[index];
}

TCB* scheduler(void) {
    int32_t highest_priority = -1;

    /* Pass 1: what is the highest EFFECTIVE priority among ready tasks?
       Reading effective_priority here is the single line that makes priority
       inheritance work at all -- everything else is just bookkeeping to keep
       this field correct. */
    for (uint32_t i = 0; i < task_count; i++) {
        if (task_list[i] != NULL && task_list[i]->state == TASK_READY) {
            if ((int32_t)task_list[i]->effective_priority > highest_priority) {
                highest_priority = (int32_t)task_list[i]->effective_priority;
            }
        }
    }

    if (highest_priority == -1) {
        return idle_task;
    }

    /* Using the selected priority's cursor */
    TCB* cursor = rr_cursor[highest_priority];
    uint32_t start_index = 0;

    /* If a previous task at this priority exists, find its position */
    if (cursor != NULL) {
        for (uint32_t i = 0; i < task_count; i++) {
            if (task_list[i] == cursor) {
                start_index = i + 1; /* begin searching right after it */
                break;
            }
        }
    }

    /* Wrap around and select only the correct candidates */
    TCB* selected = NULL;

    for (uint32_t i = 0; i < task_count; i++) {
        uint32_t eval_index = (start_index + i) % task_count;
        TCB* candidate = task_list[eval_index];

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

void commit_switch_to(TCB *nt) {
    next_task = nt;
    nt->state = TASK_RUNNING;
    nt->context_switch_count++;

    /* Drive the trace pins here rather than inside PendSV. PendSV is the
       lowest-priority exception, so it runs within a few microseconds of this
       store -- far below the 1 ms tick we are measuring against -- and doing
       it in C keeps the assembly switcher untouched. */
    trace_pin_select(nt->trace_pin);

    SCB_ICSR |= PENDSVSET;
}

void yield(void) {
    /* scheduler() reads state that a tick could change underneath us, and
       commit_switch_to() publishes next_task. If SysTick landed between the
       two, its PendSV would tail-chain and switch us out mid-yield; when we
       were eventually resumed we would finish the call and commit a stale
       next_task, switching to a task the scheduler no longer chose. */
    critical_section_enter();

    current_task->state = TASK_READY;
    TCB *nt = scheduler();
    if (nt != current_task) {
        commit_switch_to(nt);
    } else {
        current_task->state = TASK_RUNNING;
    }

    critical_section_exit();
}

void rtos_delay(uint32_t ms) {
    critical_section_enter();

    current_task->waketick   = tick_count + ms;
    current_task->blocked_on = NULL;   /* deadline-blocked, not object-blocked */
    current_task->state      = TASK_BLOCKED;
    commit_switch_to(scheduler());

    /* PendSV is pending but cannot fire until interrupts come back on, so the
       switch happens exactly here, on the closing brace of the critical
       section -- not somewhere in the middle of it. */
    critical_section_exit();
}

void rtos_delay_until(uint32_t target_tick) {
    critical_section_enter();

    /* Signed compare so the deadline is treated as "already passed" rather
       than "4 billion ticks away" when tick_count has moved past it. */
    if ((int32_t)(target_tick - tick_count) <= 0) {
        critical_section_exit();
        return;
    }

    current_task->waketick   = target_tick;
    current_task->blocked_on = NULL;
    current_task->state      = TASK_BLOCKED;
    commit_switch_to(scheduler());

    critical_section_exit();
}

void task_check_wakeups(void){
    /* Walk the task list and wake any task whose DEADLINE has passed.
       The blocked_on test is essential: a task waiting on a semaphore is also
       TASK_BLOCKED, but its waketick is a stale value from some earlier
       rtos_delay() and is therefore already <= tick_count. Without this check
       the tick handler would release semaphore waiters on the very next tick,
       and two tasks would enter the same critical section. */
    for(uint32_t i = 0; i < task_count; i++)
    {
        TCB *tcb = task_list[i];
        if (tcb != NULL &&
            tcb->state == TASK_BLOCKED &&
            tcb->blocked_on == NULL &&
            (int32_t)(tcb->waketick - tick_count) <= 0)
        {
            tcb->state = TASK_READY;
        }
    }
}

static void idle_task_function(void)
{
    while (1)
    {
        __asm volatile ("wfi");
    }
}
