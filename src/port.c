#include "port.h"
#include "task.h"
volatile uint32_t tick_count = 0;

void systick_init(void)
{
    SYST_LOAD = 15999;
    SYST_VAL  = 0;
    SYST_CTRL = SYST_CTRL_CLKSOURCE | SYST_CTRL_TICKINT | SYST_CTRL_ENABLE;
}

void SysTick_Handler(void)
{
    tick_count++;
    task_check_wakeups();
    current_task->state = TASK_READY;
    TCB *nt = scheduler();

    if (nt != current_task) {
        commit_switch_to(nt);
    } else {
        current_task->state = TASK_RUNNING;
    }
}

static volatile uint32_t critical_nesting = 0;

void critical_section_enter(void)
{
    __disable_irq();        // cpsid i — always, no check needed
    critical_nesting++;
}

void critical_section_exit(void)
{
    critical_nesting--;
    if (critical_nesting == 0) {
        __enable_irq();      // cpsie i — only on the outermost exit
    }
}