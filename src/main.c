#include "task.h"
#include "port.h"
#include "gpio.h"

void main(void)
{
    GPIO_Init();
    task_system_init();
    systick_init();
    task_system_init();
    systick_init();

    current_task = &task_a_tcb;
    current_task->state = TASK_RUNNING;

    __asm volatile ("svc #0");

    while (1) {}
}