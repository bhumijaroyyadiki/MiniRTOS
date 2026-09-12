#ifndef DEMO_H
#define DEMO_H

#include <stdint.h>

/* One measurement per round: how long the High task sat blocked between
   asking for the mutex and actually getting the CPU back. */
typedef struct {
    uint32_t round;
    uint32_t inheritance_on;   /* which mode this round ran in */
    uint32_t request_tick;     /* High called mutex_lock()      */
    uint32_t acquire_tick;     /* High resumed holding it       */
    uint32_t blocked_ticks;    /* acquire - request, in ms      */
} inversion_result_t;

#define DEMO_RESULT_SLOTS 16

extern volatile inversion_result_t demo_results[DEMO_RESULT_SLOTS];
extern volatile uint32_t demo_result_count;

/* Rolling averages, so one glance at these two in the debugger is the whole
   milestone. */
extern volatile uint32_t demo_avg_blocked_inherit_off;
extern volatile uint32_t demo_avg_blocked_inherit_on;

void demo_init(void);

#endif
