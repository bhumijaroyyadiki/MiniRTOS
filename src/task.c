#include "task.h"
// Assuming your TCB struct is named 'TCB_t' or 'TaskControlBlock'
TCB task_a_tcb; 
TCB task_b_tcb;
static _Alignas(8) uint32_t task_a_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_b_stack[TASK_STACK_WORDS];
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

void task_a(void)
{
    volatile uint32_t counter = 0;

    while (1)
    {
        counter++;

        if (counter == 1000000U)
        {
            __asm volatile (
                "ldr r0, =task_a_tcb \n"
                "ldr r1, =task_b_tcb \n"
                "svc #1              \n"
                :
                :
                : "r0", "r1", "memory"
            );

            /*
             * If the context switch works, execution should
             * never return here because Task B runs forever.
             */
            while (1)
            {
            }
        }
    }
}

void task_b(void)
{
    volatile uint32_t counter = 0;

    while (1)
    {
        counter++;
    }
}

void task_system_init(void)
{
    task_a_tcb.stack_low = &task_a_stack[0];
    task_a_tcb.stack_high = &task_a_stack[TASK_STACK_WORDS];

    task_a_tcb.sp = task_stack_init(
        task_a_tcb.stack_high,
        task_a
    );

    task_b_tcb.stack_low = &task_b_stack[0];
    task_b_tcb.stack_high = &task_b_stack[TASK_STACK_WORDS];
    task_b_tcb.sp = task_stack_init(
        task_b_tcb.stack_high,
        task_b
    );
}


