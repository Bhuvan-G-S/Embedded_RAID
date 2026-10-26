#ifndef APP_DEMO_H
#define APP_DEMO_H

#include "raid5.h"

/* Runs one non-interactive demo, then returns to main(). */
void APP_Demo_Run(
    UART_HandleTypeDef *uart,
    W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT]
);

#endif /* APP_DEMO_H */
