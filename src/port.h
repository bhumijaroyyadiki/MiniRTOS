#ifndef PORT_H
#define PORT_H

#include <stdint.h>

#define SYST_CTRL  (*(volatile uint32_t*)0xE000E010)
#define SYST_LOAD  (*(volatile uint32_t*)0xE000E014)
#define SYST_VAL   (*(volatile uint32_t*)0xE000E018)

#define SYST_CTRL_ENABLE    (1UL << 0)
#define SYST_CTRL_TICKINT   (1UL << 1)
#define SYST_CTRL_CLKSOURCE (1UL << 2)

void systick_init(void);
void SysTick_Handler(void);
void task_check_wakeups(void);

//Critical sections
void critical_section_enter(void);
void critical_section_exit(void);
extern volatile uint32_t tick_count;
#endif