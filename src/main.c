#include "task.h"

int main(void)
{
    task_system_init();

    __asm volatile ("svc #0");//This generates the SVC exception.

    while (1)
    {
    }
}
