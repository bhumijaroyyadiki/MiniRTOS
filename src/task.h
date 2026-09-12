#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>

#define TASK_STACK_WORDS 256
#define IDLE_STACK_WORDS 128
// Cortex-M register definition to trigger PendSV
#define SCB_ICSR       (*(volatile unsigned long*)0xE000ED04)
#define PENDSVSET      (1UL << 28)
typedef enum{
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_TERMINATED
}task_state_t;

typedef struct
{
    uint32_t *sp;                   //offset 0
    uint32_t *stack_low;            //offset 4
    uint32_t *stack_high;           //offset 8

    uint32_t priority;              //offset 12
    task_state_t state;             //offset 16

    const char *name;               //offset 20

    uint32_t waketick;              //offset 24

    uint32_t context_switch_count;  //offset 28

} TCB;
extern TCB *current_task;
extern TCB *next_task;

extern TCB task_a_tcb;//not making it static because we want to access it from other files
extern TCB task_b_tcb;
extern TCB task_c_tcb;
extern TCB idle_task_tcb;
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
TCB* scheduler(void);
void commit_switch_to(TCB *nt);
void task_set_state(TCB *tcb, task_state_t state);
static void idle_task_function(void);
#endif
