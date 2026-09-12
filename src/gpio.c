#include "gpio.h"

volatile uint32_t *GPIOA_MODER =
    (volatile uint32_t *)(GPIOA_BASE + GPIO_MODER_OFFSET);

volatile uint32_t *RCC_AHB1ENR =
    (volatile uint32_t *)(RCC_BASE + RCC_AHB1ENR_OFFSET);

volatile uint32_t *GPIOA_ODR =
    (volatile uint32_t *)(GPIOA_BASE + GPIO_ODR_OFFSET);

volatile uint32_t *GPIOA_BSRR =
    (volatile uint32_t *)(GPIOA_BASE + GPIO_BSRR_OFFSET);


void GPIO_Init(void)
{
    /* Enable GPIOA clock */
    *RCC_AHB1ENR |= (1U << 0);

    /* PA5..PA9 -> general purpose output (MODER = 01, two bits per pin) */
    for (uint32_t pin = PA5; pin <= PA9; pin++)
    {
        *GPIOA_MODER &= ~(0b11U << (pin * 2));
        *GPIOA_MODER |=  (0b01U << (pin * 2));
    }

    /* Start with every trace channel low. */
    *GPIOA_BSRR = (TRACE_TASK_PIN_MASK | TRACE_PIN_MUTEX | TRACE_PIN_INHERIT) << 16;
}


void GPIOA5_Toggle(void)
{
    *GPIOA_ODR ^= (1U << PA5);
}

void GPIOA6_Toggle(void)
{
    *GPIOA_ODR ^= (1U << PA6);
}

void GPIOA7_Toggle(void)
{
    *GPIOA_ODR ^= (1U << PA7);
}

void trace_pin_set(uint32_t pin_mask)
{
    *GPIOA_BSRR = pin_mask;
}

void trace_pin_clear(uint32_t pin_mask)
{
    *GPIOA_BSRR = pin_mask << 16;
}

void trace_pin_select(uint32_t pin_mask)
{
    /* Upper half clears all three task pins, lower half raises the one we
       want. Both halves land in the same store, so the analyser never sees
       a moment with two task pins high or none high. */
    *GPIOA_BSRR = (TRACE_TASK_PIN_MASK << 16) | (pin_mask & TRACE_TASK_PIN_MASK);
}
