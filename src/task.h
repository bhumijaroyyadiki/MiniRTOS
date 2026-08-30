#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>

#define TASK_STACK_WORDS 256

typedef enum{
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
}task_state_t;

typedef struct
{
    uint32_t *sp;
    uint32_t *stack_low;
    uint32_t *stack_high;

    uint32_t priority;
    task_state_t state;

    const char *name;

    uint32_t waketick;

    uint32_t context_switch_count;

} TCB;
extern TCB *current_task;
extern TCB *next_task;

extern TCB task_a_tcb;//not making it static because we want to access it from other files
extern TCB task_b_tcb;
extern TCB task_c_tcb;
/* Declare a function called task_stack_init that takes two arguments: 
the first is a pointer representing the high end of a task's stack,
and the second is a pointer to a function that takes no arguments and
returns nothing. The task_stack_init function itself returns a pointer
to a 32-bit unsigned integer, which we'll use as the newly constructed stack pointer */
uint32_t *task_stack_init(uint32_t *stack_high,void (*task_function)(void));

/*At file scope, C allows declarations and initializers, but not ordinary assignment statements.
Those statements need to execute somewhere, so they belong inside a function*/
void task_system_init(void);
void yield(void);
extern void task_a(void);
extern void task_b(void);
extern void task_c(void);
void SVC_Handler(void);
TCB *task_get_current(void);
void task_set_state(TCB *tcb, task_state_t state);
#endif
