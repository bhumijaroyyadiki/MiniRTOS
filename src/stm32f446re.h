#ifndef STM32F446RE_H
#define STM32F446RE_H

#include <stdint.h>

/* Peripheral base addresses */
#define GPIOA_BASE              0x40020000U
#define RCC_BASE                0x40023800U

/* GPIO register offsets */
#define GPIO_MODER_OFFSET       0x00U
#define GPIO_ODR_OFFSET         0x14U
/* BSRR is write-only: writing bit n sets pin n, writing bit n+16 clears it.
   A single store can set some pins and clear others, so it needs no
   read-modify-write and is safe to call from an interrupt handler. */
#define GPIO_BSRR_OFFSET        0x18U

/* RCC register offsets */
#define RCC_AHB1ENR_OFFSET      0x30U

#endif