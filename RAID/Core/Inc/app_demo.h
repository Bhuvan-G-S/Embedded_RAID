#ifndef APP_DEMO_H
#define APP_DEMO_H

#include "raid5.h"

/* Starts the interactive UART storage console. */
void APP_Console_Run(
    UART_HandleTypeDef *uart,
    W25Q64_HandleTypeDef flashes[RAID5_DEVICE_COUNT]);

#endif
