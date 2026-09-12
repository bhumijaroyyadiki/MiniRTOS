#include "demo.h"
#include "task.h"
#include "port.h"
#include "sync.h"
#include "gpio.h"

/* ========================================================================
 * The classic three-task priority inversion scenario.
 *
 *   Low  (prio 1)  takes the mutex, then does a long computation holding it
 *   Med  (prio 2)  pure CPU hog -- never blocks, never yields, shares nothing
 *   High (prio 3)  wants the same mutex
 *
 * Every round alternates the inheritance flag, so a single logic-analyser
 * capture contains the broken case and the fixed case back to back.
 * PA9 tells you which round is which.
 * ===================================================================== */

#define PRIO_LOW    1u
#define PRIO_MED    2u
#define PRIO_HIGH   3u

/* One round is long enough for everything to finish and settle, so each round
   starts from an identical quiescent state and the rounds are comparable. */
#define ROUND_TICKS         1000u

/* Offsets inside a round. The staging is what makes the demo deterministic
   rather than "usually reproduces": Low must be holding the mutex before High
   asks for it, and Med must start hogging before High blocks. */
#define OFFSET_LOW_START      0u
#define OFFSET_MED_START     10u
#define OFFSET_HIGH_REQUEST  20u

/* CPU-time, not wall-time. A tick-deadline loop would finish on schedule even
   while starved, which would hide the very starvation we are measuring. */
#define LOW_CRITICAL_MS     200u
#define MED_HOG_MS          300u
#define HIGH_WORK_MS          5u

/* Iterations of the inner spin loop that take roughly 1 ms at 16 MHz, -O0.
 *
 * CALIBRATE THIS ON THE BOARD. Put a scope on PA5 and time one Low critical
 * section with inheritance ON: the pin should stay high for ~200 ms. Scale
 * this constant by whatever ratio you actually measure. The demo still works
 * if it is off by 2x -- the two modes differ by more than that -- but the
 * numbers in the README are only trustworthy once it is tuned. */
#define BUSY_LOOPS_PER_MS  2000u

static TCB low_tcb;
static TCB med_tcb;
static TCB high_tcb;

static _Alignas(8) uint32_t low_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t med_stack[TASK_STACK_WORDS];
static _Alignas(8) uint32_t high_stack[TASK_STACK_WORDS];

/* The single contended resource. */
static mutex_t shared_mutex;

/* Present but unused by the inversion scenario -- it exists so the semaphore
   path is exercised and so the "why not just use a semaphore here?" question
   has a concrete answer sitting in the same file. */
static semaphore_t round_signal;

static volatile uint32_t round_number = 0;

volatile inversion_result_t demo_results[DEMO_RESULT_SLOTS];
volatile uint32_t demo_result_count = 0;

volatile uint32_t demo_avg_blocked_inherit_off = 0;
volatile uint32_t demo_avg_blocked_inherit_on  = 0;

static volatile uint32_t sum_off = 0, n_off = 0;
static volatile uint32_t sum_on  = 0, n_on  = 0;

/* Burn `ms` milliseconds of CPU. Deliberately does not call rtos_delay():
   a delayed task is BLOCKED and therefore off the ready list, which would
   hand the CPU straight back to Low and destroy the scenario. */
static void busy_work_ms(uint32_t ms)
{
    for (uint32_t m = 0; m < ms; m++) {
        for (volatile uint32_t i = 0; i < BUSY_LOOPS_PER_MS; i++) {
            /* volatile forces a real load/store each pass so -O0 keeps it */
        }
    }
}

/* Start tick of the next round. Every task computes this independently at the
   top of its loop; they agree because all work completes well inside a round. */
static uint32_t next_round_start(void)
{
    return ((tick_count / ROUND_TICKS) + 1u) * ROUND_TICKS;
}

static void record(uint32_t inherit_on, uint32_t req, uint32_t acq)
{
    critical_section_enter();

    uint32_t slot = demo_result_count % DEMO_RESULT_SLOTS;
    uint32_t blocked = acq - req;

    demo_results[slot].round          = round_number;
    demo_results[slot].inheritance_on = inherit_on;
    demo_results[slot].request_tick   = req;
    demo_results[slot].acquire_tick   = acq;
    demo_results[slot].blocked_ticks  = blocked;
    demo_result_count++;

    if (inherit_on) {
        sum_on += blocked;  n_on++;
        demo_avg_blocked_inherit_on = sum_on / n_on;
    } else {
        sum_off += blocked; n_off++;
        demo_avg_blocked_inherit_off = sum_off / n_off;
    }

    critical_section_exit();
}

/* ---------------------------------------------------------------- Low ---- */
static void low_task(void)
{
    while (1) {
        uint32_t start = next_round_start();
        rtos_delay_until(start + OFFSET_LOW_START);

        /* Low owns the round boundary, so it is the safe place to flip the
           mode: nothing is blocked and no mutex is held at this instant.
           Alternating rounds means one capture shows both behaviours. */
        round_number++;
        sync_set_inheritance((round_number & 1u) ? 0 : 1);

        mutex_lock(&shared_mutex);
        busy_work_ms(LOW_CRITICAL_MS);      /* long critical section */
        mutex_unlock(&shared_mutex);

        sem_give(&round_signal);
    }
}

/* ---------------------------------------------------------------- Med ---- */
static void med_task(void)
{
    while (1) {
        uint32_t start = next_round_start();
        rtos_delay_until(start + OFFSET_MED_START);

        /* The antagonist. Touches no shared data, holds nothing, and is
           entirely innocent -- which is the point. It outranks Low, so with
           inheritance off it starves Low indefinitely, and High along with it. */
        busy_work_ms(MED_HOG_MS);
    }
}

/* --------------------------------------------------------------- High ---- */
static void high_task(void)
{
    while (1) {
        uint32_t start = next_round_start();
        rtos_delay_until(start + OFFSET_HIGH_REQUEST);

        uint32_t inherit_on = (uint32_t)sync_get_inheritance();

        uint32_t req = tick_count;
        mutex_lock(&shared_mutex);          /* <-- blocks here */
        uint32_t acq = tick_count;

        /* PA7 goes high the moment this task is scheduled again. The gap
           between req and acq is the milestone measurement. */
        record(inherit_on, req, acq);

        busy_work_ms(HIGH_WORK_MS);
        mutex_unlock(&shared_mutex);
    }
}

void demo_init(void)
{
    mutex_init(&shared_mutex);
    sem_init(&round_signal, 0);

    task_create(&low_tcb,  low_stack,  TASK_STACK_WORDS, low_task,  PRIO_LOW,  "Low");
    task_create(&med_tcb,  med_stack,  TASK_STACK_WORDS, med_task,  PRIO_MED,  "Med");
    task_create(&high_tcb, high_stack, TASK_STACK_WORDS, high_task, PRIO_HIGH, "High");

    task_set_trace_pin(&low_tcb,  TRACE_PIN_LOW);
    task_set_trace_pin(&med_tcb,  TRACE_PIN_MED);
    task_set_trace_pin(&high_tcb, TRACE_PIN_HIGH);

    sync_set_inheritance(0);   /* round 1 runs broken */
}
