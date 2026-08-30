#include "task.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_TASKS 8
TCB *current_task = NULL;
TCB *next_task = NULL;

TCB task_a_tcb; 
TCB task_b_tcb;
TCB task_c_tcb;
static _Alignas(8) uint32_t task_a_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_b_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_c_stack[TASK_STACK_WORDS];

static TCB *task_list[MAX_TASKS];
static uint32_t task_count = 0;
#define MAX_PRIORITIES 8

// Array of cursors for Round-Robin scheduling per priority level
// Tracks the most recently selected TCB for priority 'p'
TCB* rr_cursor[MAX_PRIORITIES] = { NULL }; 


TCB idle_task_tcb;
TCB* idle_task = &idle_task_tcb; //  satisfies the 'extern TCB* idle_task'

// Cortex-M register definition to trigger PendSV
#define SCB_ICSR       (*(volatile unsigned long*)0xE000ED04)
#define PENDSVSET      (1UL << 28)
uint32_t *task_stack_init( uint32_t *stack_high,void (*task_function)(void))
{
    uint32_t *sp = stack_high;

    /* Make stack 8-byte aligned (Cortex-M exception frames require appropriate
     stack alignment. The architecture expects the stack to maintain 8-byte 
     alignment at exception boundaries Our uint32_t array is naturally 4-byte 
     aligned, but we want to explicitly ensure 8-byte alignment..)*/
    sp = (uint32_t *)((uintptr_t)sp & ~((uintptr_t)0x7));

    /* Hardware exception frame */
    *(--sp) = 0x01000000;                  /* xPSR */
    *(--sp) = ((uint32_t)task_function) | 1U; /* PC(because the exception frame's PC must represent a valid Thumb execution address.) */
    *(--sp) = 0xFFFFFFFD;                  /* LR */
    *(--sp) = 0x00000000;                  /* R12 */
    *(--sp) = 0x00000000;                  /* R3 */
    *(--sp) = 0x00000000;                  /* R2 */
    *(--sp) = 0x00000000;                  /* R1 */
    *(--sp) = 0x00000000;                  /* R0 */
    
    /* R4-R11 */
    *(--sp) = 0xBBBBBBBB;  /* R11 */
    *(--sp) = 0xAAAAAAAA;  /* R10 */
    *(--sp) = 0x99999999;  /* R9 */
    *(--sp) = 0x88888888;  /* R8 */
    *(--sp) = 0x77777777;  /* R7 */
    *(--sp) = 0x66666666;  /* R6 */
    *(--sp) = 0x55555555;  /* R5 */
    *(--sp) = 0x44444444;  /* R4 */
    
    return sp;
}

void task_b(void)
{
    volatile uint32_t counter = 0;

    while (1)
    {
        counter++;
    }
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
        2,
        "Task C"
    );

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

void yield(void) {
    TCB *nt = scheduler();
    if (nt != current_task) {
        current_task->state = TASK_READY;   // old task, via current_task (not yet reassigned)
        nt->state = TASK_RUNNING;            // new task, via nt
        next_task = nt;
        SCB_ICSR |= PENDSVSET;
    }
}

void task_a(void)
{
    volatile uint32_t counter = 0;

    while (1)
    {
        counter++;

        if (counter == 1000000U)
        {
            yield();

            while (1)
            {
            }
        }
    }
}

void task_c(void)
{
    volatile uint32_t counter = 0;

    while (1)
    {
        counter++;

        if (counter == 1000000U)
        {
            yield();

            while (1)
            {
            }
        }
    }
}