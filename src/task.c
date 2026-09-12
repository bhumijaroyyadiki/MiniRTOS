#include "task.h"
#include "port.h"
#include <stdint.h>
#include <stddef.h>
#include "gpio.h"
#define MAX_TASKS 8
TCB *current_task = NULL;
TCB *next_task = NULL;

TCB task_a_tcb; 
TCB task_b_tcb;
TCB task_c_tcb;
static _Alignas(8) uint32_t task_a_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_b_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_c_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t idle_stack[IDLE_STACK_WORDS];
static TCB *task_list[MAX_TASKS];
static uint32_t task_count = 0;
#define MAX_PRIORITIES 8

// Array of cursors for Round-Robin scheduling per priority level
// Tracks the most recently selected TCB for priority 'p'
TCB* rr_cursor[MAX_PRIORITIES] = { NULL }; 


TCB idle_task_tcb;
TCB* idle_task = &idle_task_tcb; //  satisfies the 'extern TCB* idle_task'

uint32_t *task_stack_init( uint32_t *stack_high,void (*task_function)(void))
{
    uint32_t *sp = stack_high; // 0x20000410

    /* Make stack 8-byte aligned (Cortex-M exception frames require appropriate
     stack alignment. The architecture expects the stack to maintain 8-byte 
     alignment at exception boundaries Our uint32_t array is naturally 4-byte 
     aligned, but we want to explicitly ensure 8-byte alignment..)*/
    sp = (uint32_t *)((uintptr_t)sp & ~((uintptr_t)0x7));

    /* Hardware exception frame */
    *(--sp) = 0x01000000;                  /* xPSR */ // 0x2000040C
    *(--sp) = ((uint32_t)task_function) | 1U; /* PC(because the exception frame's PC must represent a valid Thumb execution address.) */
    *(--sp) = 0xFFFFFFFD;                  /* LR */
    *(--sp) = 0x00000000;                  /* R12 */
    *(--sp) = 0x00000000;                  /* R3 */
    *(--sp) = 0x00000000;                  /* R2 */
    *(--sp) = 0x00000000;                  /* R1 */
    *(--sp) = 0x00000000;                  /* R0 */ //0x2000003F0
    
    /* R4-R11 */
    *(--sp) = 0xBBBBBBBB;  /* R11 */ //0x200003EC
    *(--sp) = 0xAAAAAAAA;  /* R10 */
    *(--sp) = 0x99999999;  /* R9 */
    *(--sp) = 0x88888888;  /* R8 */
    *(--sp) = 0x77777777;  /* R7 */
    *(--sp) = 0x66666666;  /* R6 */
    *(--sp) = 0x55555555;  /* R5 */
    *(--sp) = 0x44444444;  /* R4 */ //0x200003D0

    //As the stack grows downwards
    //In task startup mechanics ,software pops first and then hardware
    //So the stack pointer should point to the last pushed value, which is R4 here
    //so task_a_tcb.sp = 0x200003D0, which is the address of R4
    
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

    tcb->stack_low = &stack[0];
    tcb->stack_high = &stack[stack_words];

    tcb->sp = task_stack_init(
        tcb->stack_high,
        task_function
    );

    tcb->priority = priority;
    tcb->state = TASK_READY;
    tcb->name = name;
    tcb->waketick = 0;
    tcb->context_switch_count = 0;

    task_list[task_count++] = tcb;

    return 0;
}
void task_system_init(void)
{
    
    task_create(
        &task_a_tcb,
        task_a_stack,
        TASK_STACK_WORDS,
        task_a,
        1,
        "Task A"
    );

    task_create(
        &task_b_tcb,
        task_b_stack,
        TASK_STACK_WORDS,
        task_b,
        2,
        "Task B"
    );

    task_create(
        &task_c_tcb,
        task_c_stack,
        TASK_STACK_WORDS,
        task_c,
        3,
        "Task C"
    );
    //Initialize the idle task
    idle_task_tcb.stack_low = &idle_stack[0];
    idle_task_tcb.stack_high = &idle_stack[IDLE_STACK_WORDS];

    idle_task_tcb.sp = task_stack_init(
        idle_task_tcb.stack_high,
        idle_task_function
    );

    idle_task_tcb.priority = 0;
    idle_task_tcb.state = TASK_READY;
    idle_task_tcb.name = "Idle";
    idle_task_tcb.waketick = 0;
    idle_task_tcb.context_switch_count = 0;

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
TCB* scheduler(void) {
    int32_t highest_priority = -1;

    for (uint32_t i = 0; i < task_count; i++) {
        if (task_list[i] != NULL && task_list[i]->state == TASK_READY) {
            if ((int32_t)task_list[i]->priority > highest_priority) {
                highest_priority = (int32_t)task_list[i]->priority;
            }
        }
    }

    if (highest_priority == -1) {
        return idle_task;
    }

    //  Using the selected priority's cursor 
    TCB* cursor = rr_cursor[highest_priority];
    uint32_t start_index = 0;

    // If a previous task at this priority exists, finding its position
    if (cursor != NULL) {
        for (uint32_t i = 0; i < task_count; i++) {
            if (task_list[i] == cursor) {
                start_index = i + 1; // Begin searching right after it
                break;
            }
        }
    }

    //  Wrap around and select only the correct candidates 
    TCB* selected = NULL;
    
    for (uint32_t i = 0; i < task_count; i++) {
        // Evaluate index using modulo arithmetic for circular list behavior
        uint32_t eval_index = (start_index + i) % task_count;
        TCB* candidate = task_list[eval_index];

        if (candidate != NULL && 
            candidate->state == TASK_READY && 
            candidate->priority == highest_priority) {
            
            selected = candidate;
            break; 
        }
    }

    //  Update the cursor and return the TCB 
    if (selected != NULL) {
        rr_cursor[highest_priority] = selected;
        return selected;
    }

    return idle_task;
}

void commit_switch_to(TCB *nt) {
    next_task = nt;
    nt->state = TASK_RUNNING;
    SCB_ICSR |= PENDSVSET;
}

void yield(void) {
    current_task->state = TASK_READY;
    TCB *nt = scheduler();
    if (nt != current_task) {
        commit_switch_to(nt);
    } else {
        current_task->state = TASK_RUNNING;
    }
}

void rtos_delay(uint32_t ms) {
    current_task->waketick = tick_count + ms;
    current_task->state = TASK_BLOCKED;
    commit_switch_to(scheduler());
}
void task_check_wakeups(void){
    //Walk through the task list and check if any blocked tasks are ready to wake up
    for(uint32_t i = 0; i < task_count; i++)
    {
        TCB *tcb = task_list[i];
        if (tcb != NULL && tcb->state == TASK_BLOCKED && tcb->waketick <= tick_count)
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

void task_a(void)
{
    while (1)
    {
        GPIOA5_Toggle();
        rtos_delay(100);
    }
}

void task_b(void)
{
    while (1)
    {
        GPIOA6_Toggle();
        rtos_delay(500);
    }
}

void task_c(void)
{
    while (1)
    {
        GPIOA7_Toggle();
        rtos_delay(1000);
    }
}

