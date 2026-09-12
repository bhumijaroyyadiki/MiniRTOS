#ifndef GPIO_H
#define GPIO_H

#include "stm32f446re.h"
#include <stdint.h>

#define PA5 5
#define PA6 6
#define PA7 7

extern volatile uint32_t *RCC_AHB1ENR;
extern volatile uint32_t *GPIOA_MODER;
extern volatile uint32_t *GPIOA_ODR;

void GPIO_Init(void);

void GPIOA5_Toggle(void);
void GPIOA6_Toggle(void);
void GPIOA7_Toggle(void);

#endif