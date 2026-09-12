#include "port.h"
#include "task.h"

volatile uint32_t tick_count = 0;

void systick_init(void)
{
    SYST_LOAD = 15999;          /* 16 MHz / 16000 = 1 kHz -> 1 ms tick */
    SYST_VAL  = 0;
    SYST_CTRL = SYST_CTRL_CLKSOURCE | SYST_CTRL_TICKINT | SYST_CTRL_ENABLE;
}

void SysTick_Handler(void)
{
    tick_count++;
    task_check_wakeups();

    /* No critical section needed here: we are already in handler context and
       SysTick cannot preempt itself. PendSV is lower priority than SysTick, so
       the switch we request below tail-chains after this handler returns. */
    current_task->state = TASK_READY;
    TCB *nt = scheduler();

    if (nt != current_task) {
        commit_switch_to(nt);
    } else {
        current_task->state = TASK_RUNNING;
    }
}

/* Not volatile-qualified for atomicity -- it is only ever touched with
   interrupts already masked, so plain accesses are sufficient. volatile is
   kept so the compiler cannot cache it across the asm barriers.
 *
 * Why ONE global counter is enough, rather than one per task:
 * a context switch can only happen when interrupts are on, and interrupts are
 * only on when critical_nesting == 0. So every switch point sees the counter
 * at zero, and no task can ever observe another task's nesting depth. The
 * blocking primitives in sync.c deliberately call critical_section_exit()
 * before handing over the CPU precisely to keep that invariant true.
 *
 * If you ever add a path that switches tasks with nesting > 0, this counter
 * becomes per-task state and has to move into the TCB. */
static volatile uint32_t critical_nesting = 0;

void critical_section_enter(void)
{
    __disable_irq();        /* cpsid i -- unconditionally, before touching the counter */
    critical_nesting++;
}

void critical_section_exit(void)
{
    /* Safe to read-modify-write without protection: interrupts are off for the
       whole of this function by definition of being inside a critical section. */
    if (critical_nesting > 0) {
        critical_nesting--;
    }

    if (critical_nesting == 0) {
        __enable_irq();     /* cpsie i -- only on the outermost exit */
    }
}
