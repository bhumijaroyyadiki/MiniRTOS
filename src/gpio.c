#include "gpio.h"

volatile uint32_t *GPIOA_MODER =
    (volatile uint32_t *)(GPIOA_BASE + GPIO_MODER_OFFSET);

volatile uint32_t *RCC_AHB1ENR =
    (volatile uint32_t *)(RCC_BASE + RCC_AHB1ENR_OFFSET);

volatile uint32_t *GPIOA_ODR =
    (volatile uint32_t *)(GPIOA_BASE + GPIO_ODR_OFFSET);


void GPIO_Init(void)
{
    /* Enable GPIOA clock */
    *RCC_AHB1ENR |= (1U << 0);

    /* PA5, PA6, PA7 → General purpose output (01) */

    *GPIOA_MODER &= ~(0b11U << 10);
    *GPIOA_MODER |=  (0b01U << 10);

    *GPIOA_MODER &= ~(0b11U << 12);
    *GPIOA_MODER |=  (0b01U << 12);

    *GPIOA_MODER &= ~(0b11U << 14);
    *GPIOA_MODER |=  (0b01U << 14);
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