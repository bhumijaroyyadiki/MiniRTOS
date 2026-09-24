// demo_baseline.c — minimal workload for scheduler measurement, nothing else
#include "task.h"
#include "port.h"

#define PRIO_A 2u
#define PRIO_B 2u
#define PRIO_C 2u
#define PRIO_D 2u

static TCB task_a_tcb, task_b_tcb, task_c_tcb, task_d_tcb;
static _Alignas(8) uint32_t task_a_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_b_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_c_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t task_d_stack[TASK_STACK_WORDS];

static void task_a(void)
{
    while (1) {
        __asm volatile ("nop");
        yield();
    }
}

static void task_b(void)
{
    while (1) {
        __asm volatile ("nop");
        yield();
    }
}

static void task_c(void)
{
    while (1) {
        __asm volatile ("nop");
        yield();
    }
}

static void task_d(void)
{
    while (1) {
        __asm volatile ("nop");
        yield();
    }
}

void baseline_init(void)
{
    task_create(&task_a_tcb, task_a_stack, TASK_STACK_WORDS, task_a, PRIO_A, "A");
    task_create(&task_b_tcb, task_b_stack, TASK_STACK_WORDS, task_b, PRIO_B, "B");
    task_create(&task_c_tcb, task_c_stack, TASK_STACK_WORDS, task_c, PRIO_C, "C");
    task_create(&task_d_tcb, task_d_stack, TASK_STACK_WORDS, task_d, PRIO_D, "D");
}