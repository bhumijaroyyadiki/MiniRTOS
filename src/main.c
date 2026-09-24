#include "task.h"
#include "port.h"
#include "gpio.h"
#include "demo.h"

void main(void)
{
    GPIO_Init();

    /* Kernel first (idle task), then the application registers its own tasks.
       Called exactly once each -- task_create() appends to task_list[]
       unconditionally, so a second call would register a second, aliased copy
       of every task and quietly corrupt the scheduler. */
    task_system_init();
    baseline_init();
    dwt_init();
    /* Let the scheduler choose the first task rather than hardcoding one.
       svc.S bootstraps from current_task, so whatever is picked here is what
       actually starts. */
    current_task = scheduler();
    current_task->state = TASK_RUNNING;
    trace_pin_select(current_task->trace_pin);

    /* SysTick last: the handler dereferences current_task, so it must not be
       able to fire before current_task is valid. */
    systick_init();

    __asm volatile ("svc #0");

    while (1) {}
}
