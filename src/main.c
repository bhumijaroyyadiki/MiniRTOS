#include "task.h"
void main(void)
{
    task_system_init();

    current_task = &task_a_tcb;
    current_task->state = TASK_RUNNING;

    __asm volatile ("svc #0");

    while (1) {}
}