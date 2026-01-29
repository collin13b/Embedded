#ifndef __LED_H
#define __LED_H
#include "stm32f4xx.h"
#include <stdint.h>
typedef struct 
{
    GPIO_TypeDef* GPIOx;
    uint16_t Pin;
    
    /* data */
}led_t;


#endif // __LED_H11