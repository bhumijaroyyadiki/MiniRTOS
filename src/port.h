#ifndef PORT_H
#define PORT_H

#include <stdint.h>

#define SYST_CTRL  (*(volatile uint32_t*)0xE000E010)
#define SYST_LOAD  (*(volatile uint32_t*)0xE000E014)
#define SYST_VAL   (*(volatile uint32_t*)0xE000E018)

#define SYST_CTRL_ENABLE    (1UL << 0)
#define SYST_CTRL_TICKINT   (1UL << 1)
#define SYST_CTRL_CLKSOURCE (1UL << 2)

/* CMSIS is not on the include path (we build -nostdlib -ffreestanding), so the
   two intrinsics we actually need are spelled out here.
   The "memory" clobber is the important part: it stops the compiler hoisting a
   load or sinking a store across the barrier, which would put shared-state
   access outside the window where interrupts are actually off. */
static inline void __disable_irq(void)
{
    __asm volatile ("cpsid i" : : : "memory");
}

static inline void __enable_irq(void)
{
    __asm volatile ("cpsie i" : : : "memory");
}

void systick_init(void);
void SysTick_Handler(void);
void task_check_wakeups(void);

/* Critical sections. Nesting-counted, so an inner section exiting does not
   re-enable interrupts while an outer one is still open. */
void critical_section_enter(void);
void critical_section_exit(void);

extern volatile uint32_t tick_count;
#endif
