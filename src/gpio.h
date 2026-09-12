#ifndef GPIO_H
#define GPIO_H

#include "stm32f446re.h"
#include <stdint.h>

#define PA5 5
#define PA6 6
#define PA7 7
#define PA8 8
#define PA9 9

/* ---- Logic-analyser trace channels -------------------------------------
 * One pin per demo task: the pin is HIGH for exactly as long as that task
 * owns the CPU. The pins are driven from commit_switch_to(), not from the
 * task bodies, so the waveform is the scheduler's decision rather than the
 * application's opinion of it.
 *
 *   PA5  Low task running
 *   PA6  Med task running
 *   PA7  High task running      <- the milestone signal
 *   PA8  mutex currently held
 *   PA9  priority inheritance enabled for this round
 *
 * Idle drives all task pins low, so gaps in the waveform are real idle time.
 */
#define TRACE_PIN_LOW       (1U << PA5)
#define TRACE_PIN_MED       (1U << PA6)
#define TRACE_PIN_HIGH      (1U << PA7)
#define TRACE_PIN_MUTEX     (1U << PA8)
#define TRACE_PIN_INHERIT   (1U << PA9)

/* The pins commit_switch_to() owns. Everything in this mask is cleared on
   every switch, then the incoming task's single bit is set. */
#define TRACE_TASK_PIN_MASK (TRACE_PIN_LOW | TRACE_PIN_MED | TRACE_PIN_HIGH)

extern volatile uint32_t *RCC_AHB1ENR;
extern volatile uint32_t *GPIOA_MODER;
extern volatile uint32_t *GPIOA_ODR;
extern volatile uint32_t *GPIOA_BSRR;

void GPIO_Init(void);

void GPIOA5_Toggle(void);
void GPIOA6_Toggle(void);
void GPIOA7_Toggle(void);

/* Atomic (single-store) pin control, safe from handler context. */
void trace_pin_set(uint32_t pin_mask);
void trace_pin_clear(uint32_t pin_mask);

/* Clear every task pin and raise just `pin_mask`, in one bus write. */
void trace_pin_select(uint32_t pin_mask);

#endif
