#ifndef STM32F446RE_H
#define STM32F446RE_H

#include <stdint.h>

/* Peripheral base addresses */
#define GPIOA_BASE              0x40020000U
#define RCC_BASE                0x40023800U

/* GPIO register offsets */
#define GPIO_MODER_OFFSET       0x00U
#define GPIO_ODR_OFFSET         0x14U

/* RCC register offsets */
#define RCC_AHB1ENR_OFFSET      0x30U

#endif