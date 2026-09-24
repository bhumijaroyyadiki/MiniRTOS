#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>

#define TASK_STACK_WORDS 256
#define IDLE_STACK_WORDS 128
#define MAX_TASKS      8
#define MAX_PRIORITIES 8

/* Cortex-M register definition to trigger PendSV */
#define SCB_ICSR       (*(volatile unsigned long*)0xE000ED04)
#define PENDSVSET      (1UL << 28)
//Instrumentation Globals
extern uint32_t task_count;

extern volatile uint32_t sched_time_last;
extern volatile uint32_t sched_time_max;

extern volatile uint32_t sched_ready_count;
extern volatile int32_t  sched_highest_prio;
extern volatile uint32_t sched_start_index;

extern volatile uint32_t sched_time_max_pass1;
extern volatile uint32_t sched_time_max_pass2;
extern volatile uint32_t sched_time_max_pass3;
extern volatile uint32_t systick_latency_last;
extern volatile uint32_t systick_latency_max;

extern volatile uint32_t spurious_wake_count;
typedef enum{
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
}task_state_t;

typedef struct
{
    /* sp MUST stay at offset 0: context_switch.s and svc.S both load the
       saved stack pointer with a bare `ldr rN, [tcb]`. */
    uint32_t *sp;                   /* offset 0  */
    uint32_t *stack_low;            /* offset 4  */
    uint32_t *stack_high;           /* offset 8  */

    /* --- priority, split for inheritance ---------------------------------
     * base_priority      what task_create() was given. Never changes.
     * effective_priority what scheduler() actually compares. Raised by
     *                    priority inheritance, recomputed on mutex_unlock.
     * Invariant: effective_priority >= base_priority, always.
     * Higher number == higher priority. Idle is 0. */
    uint32_t base_priority;
    uint32_t effective_priority;

    task_state_t state;

    /* --- what is this task blocked on? -----------------------------------
     * NULL  -> either runnable, or sleeping on a deadline (see waketick).
     * !NULL -> blocked indefinitely on this sync object; waketick is
     *          meaningless and task_check_wakeups() must not touch it.
     * This one field is what stops the tick handler from spuriously
     * releasing a task that is waiting on a semaphore. */
    void *blocked_on;

    const char *name;

    uint32_t waketick;

    uint32_t context_switch_count;

    /* GPIO bit raised while this task owns the CPU. 0 = no trace channel. */
    uint32_t trace_pin;
   uint32_t cpu_cycles_total;
   uint32_t last_resume_cycle;
} TCB;

extern TCB *current_task;
extern TCB *next_task;
extern TCB idle_task_tcb;
extern TCB *idle_task;

uint32_t *task_stack_init(uint32_t *stack_high, void (*task_function)(void));

int task_create(
    TCB *tcb,
    uint32_t *stack,
    uint32_t stack_words,
    void (*task_function)(void),
    uint8_t priority,
    const char *name
);

/* Brings up the idle task only. Application tasks are registered by the
   application (see demo.c) so the kernel has no knowledge of them. */
void task_system_init(void);

void  yield(void);
void  rtos_delay(uint32_t ms);
/* Sleep until an absolute tick. Preferred over rtos_delay() for periodic
   work: the wake-up does not drift by however long the task ran. */
void  rtos_delay_until(uint32_t target_tick);

TCB  *task_get_current(void);
TCB  *scheduler(void);
void  commit_switch_to(TCB *nt);
void  task_set_state(TCB *tcb, task_state_t state);
void  task_set_trace_pin(TCB *tcb, uint32_t pin_mask);

/* Iteration over the registered tasks. sync.c needs this to find waiters and
   to recompute an owner's inherited priority without keeping its own lists. */
uint32_t task_get_count(void);
TCB     *task_get_by_index(uint32_t index);

void SVC_Handler(void);
uint32_t task_stack_high_water_words(TCB *tcb);
#endif
